#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression checks for legacy-first use of the shared DMA ISR."""

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
		len(sys.argv) == 2,
		"usage: test_legacy_dma_ownership.py usb_api_transceiver.c",
	)
	transceiver = Path(sys.argv[1]).read_text(encoding="utf-8")

	require(
		re.search(
			r"static\s+edge_transport_state_t\s+edge_transport\s*=\s*"
			r"EDGE_TRANSPORT_STATE_INITIALIZER\s*;",
			transceiver,
		)
		is not None,
		"static Edge transport lacks a legacy-safe ownership initializer",
	)

	startup = function_body(transceiver, "void transceiver_startup(")
	require(
		ordered(
			startup,
			"edge_reset_dma_ownership(edge_activation_epoch);",
			"transceiver_dma_setup();",
		),
		"RX/TX startup does not clear framed DMA ownership before enabling DMA",
	)

	dma_isr = function_body(transceiver, "void dma_isr_impl(")
	require(
		"edge_transport_dma_owned(&edge_transport)" in dma_isr,
		"shared DMA ISR does not validate framed ownership",
	)
	require(
		"edge_transport_dma_chunk_valid(" in dma_isr,
		"shared DMA ISR can underflow a framed continuation",
	)
	require(
		"edge_transport.dma_slot != EDGE_TRANSPORT_SLOT_NONE" not in dma_isr,
		"shared DMA ISR still treats an unchecked sentinel as framed ownership",
	)
	require(
		"edge_abandon_dma_ownership();" in dma_isr,
		"invalid framed DMA does not abandon only its own slot",
	)
	require(
		"edge_reset_dma_ownership(" not in dma_isr,
		"DMA ISR error path can free a different USB-owned frame",
	)
	abandon = function_body(transceiver, "static void edge_abandon_dma_ownership(")
	require(
		ordered(
			abandon,
			"edge_transport_cancel_dma(&edge_transport);",
			"edge_clear_dma_progress();",
		),
		"DMA abandonment does not cancel only its slot and clear progress",
	)
	require(
		"edge_transport_reset" not in abandon
		and "edge_reset_dma_ownership" not in abandon,
		"DMA abandonment can free READY or USB-owned frame slots",
	)

	start_next = function_body(
		transceiver, "static bool edge_start_next_dma_chunk("
	)
	for required in (
		"edge_dma_remaining == 0",
		"edge_dma_source_offset >= USB_SAMP_BUFFER_SIZE",
		"edge_dma_destination_offset >= USB_BULK_BUFFER_SIZE",
		"chunk > DMA_MAX_TRANSFER_SIZE",
		"chunk > USB_BULK_BUFFER_SIZE - edge_dma_destination_offset",
	):
		require(required in start_next, f"framed DMA bound is missing: {required}")

	print("Legacy-first shared-DMA ownership checks passed")


if __name__ == "__main__":
	main()
