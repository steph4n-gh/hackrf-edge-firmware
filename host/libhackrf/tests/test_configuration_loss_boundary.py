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
