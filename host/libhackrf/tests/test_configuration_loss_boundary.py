#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Source-level regression for USB configuration-loss transport shutdown."""

from pathlib import Path
import re
import sys


def require(condition, message):
	if not condition:
		raise AssertionError(message)


def function_body(source, signature):
	start = source.index(signature)
	open_brace = source.index("{", start)
	depth = 0
	for offset in range(open_brace, len(source)):
		if source[offset] == "{":
			depth += 1
		elif source[offset] == "}":
			depth -= 1
			if depth == 0:
				return source[open_brace + 1 : offset]
	raise AssertionError(f"unterminated function: {signature}")


def ordered(body, *needles):
	position = -1
	for needle in needles:
		position = body.find(needle, position + 1)
		if position < 0:
			return False
	return True


def main():
	require(
		len(sys.argv) == 4,
		"usage: test_configuration_loss_boundary.py common/usb.c "
		"hackrf_usb.c usb_api_transceiver.c",
	)
	common_usb = Path(sys.argv[1]).read_text(encoding="utf-8")
	hackrf_usb = Path(sys.argv[2]).read_text(encoding="utf-8")
	transceiver = Path(sys.argv[3]).read_text(encoding="utf-8")

	bus_reset = function_body(common_usb, "static void usb_bus_reset(")
	require(
		"usb_set_configuration(device, 0);" in bus_reset,
		"USB bus reset no longer publishes configuration loss",
	)
	usb_isr = function_body(common_usb, "void USB0_ISR_NAME(")
	require(
		ordered(
			usb_isr,
			"usb_check_for_setup_events();",
			"usb_check_for_transfer_events();",
		),
		"USB control SETUP no longer precedes aborted control completions",
	)

	request_mode = function_body(transceiver, "void request_transceiver_mode(")
	require(
		ordered(
			request_mode,
			"usb_endpoint_flush(&usb_endpoint_bulk_in);",
			"usb_endpoint_is_complete(USB_ENDPOINT_BULK_IN_ADDRESS)",
			"usb_queue_head(usb_endpoint_bulk_in.address)->total_bytes",
			"edge_transport_reconcile_usb_completion_token(",
			"edge_bulk_transfer_complete(",
			"usb_endpoint_flush(&usb_endpoint_bulk_out);",
			"edge_transceiver_request_superseded(mode);",
			"transceiver_request.mode = mode;",
			"transceiver_request.seq++;",
		),
		"OFF does not reconcile a completed Edge bulk-IN frame before reset",
	)
	require(
		"EDGE_FRAME_BYTES,\n\t\t\tbulk_in_completion_observed," in request_mode,
		"OFF completion reconciliation no longer consumes its COMPLETE witness",
	)
	flush_once = function_body(
		common_usb, "static void usb_flush_primed_endpoints_once("
	)
	require(
		ordered(
			flush_once,
			"usb_wait_for_endpoint_priming_to_finish(mask);",
			"usb_flush_endpoints(mask);",
			"usb_wait_for_endpoint_flushing_to_finish(mask);",
		),
		"single-pass endpoint flush no longer performs one de-prime attempt",
	)
	require(
		"USB0_ENDPTSTAT" not in flush_once,
		"single-pass ROM handoff flush acquired an ENDPTSTAT retry",
	)
	flush_primed = function_body(
		common_usb, "static void usb_flush_primed_endpoints("
	)
	require(
		ordered(
			flush_primed,
			"do {",
			"usb_flush_primed_endpoints_once(mask);",
			"} while (USB0_ENDPTSTAT & mask);",
		),
		"endpoint flush no longer retries controller-refused de-prime attempts",
	)
	controller_reset = function_body(common_usb, "static void usb_controller_reset(")
	require(
		ordered(
			controller_reset,
			"usb_disable_all_endpoints();",
			"usb_clear_all_pending_interrupts();",
			"usb_flush_primed_endpoints_once(0xFFFFFFFF);",
			"usb_controller_stop();",
			"USB0_USBCMD_D = USB0_USBCMD_D_RST;",
		),
		"ROM handoff no longer uses a single-pass flush before controller reset",
	)
	require(
		"usb_reset_all_endpoints();" not in controller_reset,
		"ROM handoff can re-enter the ENDPTSTAT retry loop before reset",
	)
	require(
		"USB0_ENDPTSTAT" not in controller_reset
		and "usb_flush_primed_endpoints(" not in controller_reset
		and controller_reset.count(
			"usb_flush_primed_endpoints_once(0xFFFFFFFF);"
		)
		== 1,
		"ROM handoff must make exactly one single-pass flush attempt",
	)
	reset_all = function_body(common_usb, "static void usb_reset_all_endpoints(")
	require(
		ordered(
			reset_all,
			"usb_disable_all_endpoints();",
			"usb_clear_all_pending_interrupts();",
			"usb_flush_all_primed_endpoints();",
		),
		"full endpoint reset no longer disables, clears, then de-primes",
	)
	flush_all = function_body(
		common_usb, "static void usb_flush_all_primed_endpoints("
	)
	require(
		"usb_flush_primed_endpoints(0xFFFFFFFF);" in flush_all,
		"full endpoint reset no longer uses the ENDPTSTAT retry helper",
	)
	endpoint_flush = function_body(common_usb, "void usb_endpoint_flush(")
	require(
		endpoint_flush.count("usb_flush_primed_endpoints(") == 2
		and "usb_flush_primed_endpoints_once(" not in endpoint_flush,
		"runtime endpoint abort no longer uses the ENDPTSTAT retry helper",
	)
	require(
		"usb_reset_all_endpoints();" in bus_reset,
		"USB bus reset no longer uses the full endpoint de-prime procedure",
	)
	for status in (
		"USB_TD_DTD_TOKEN_STATUS_ACTIVE",
		"USB_TD_DTD_TOKEN_STATUS_HALTED",
		"USB_TD_DTD_TOKEN_STATUS_BUFFER_ERROR",
		"USB_TD_DTD_TOKEN_STATUS_TRANSACTION_ERROR",
	):
		require(
			status in request_mode,
			f"OFF completion reconciliation ignores {status}",
		)

	configuration_changed = function_body(
		hackrf_usb, "void usb_configuration_changed("
	)
	require(
		"usb_set_configuration_changed_cb(usb_configuration_changed);"
		in hackrf_usb,
		"HackRF no longer installs the configuration-loss callback",
	)
	require(
		ordered(
			configuration_changed,
			"request_transceiver_mode(TRANSCEIVER_MODE_OFF);",
			"edge_reset_state();",
			"usb_endpoint_init(&usb_endpoint_bulk_in, false);",
			"usb_endpoint_init(&usb_endpoint_bulk_out, false);",
		),
		"configuration loss does not request OFF before endpoint reinitialization",
	)
	require(
		re.search(
			r"device->configuration\s*!=\s*NULL\s*&&\s*"
			r"device->configuration->number\s*==\s*1",
			configuration_changed,
		)
		is not None,
		"configuration-zero path can dereference a null configuration",
	)

	rx = function_body(transceiver, "void rx_mode(")
	require(
		ordered(
			rx,
			"const uint32_t sampling_completed = m0_state.m0_count;",
			"const bool no_reserve = edge_observe_loss_and_transitions();",
			"edge_start_dma_if_possible(sampling_completed, no_reserve);",
		),
		"framed RX does not snapshot M0 count before observing loss",
	)
	start_dma = function_body(
		transceiver, "static void edge_start_dma_if_possible("
	)
	require(
		"m0_state.m0_count" not in start_dma,
		"frame reservation rereads live M0 count after loss observation",
	)
	require(
		ordered(
			rx,
			"edge_cancel_dma();",
			"edge_rx_stop(edge_activation_epoch);",
			"transceiver_shutdown();",
			"edge_transport_reset(&edge_transport, edge_activation_epoch);",
		),
		"framed stop no longer cancels DMA and flushes USB before ownership reset",
	)
	print("USB configuration-loss transport boundary checks passed")


if __name__ == "__main__":
	main()
