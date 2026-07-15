#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression checks for the M0 exact-loss stop acknowledgement boundary."""

from pathlib import Path
import sys


def require(condition, message):
	if not condition:
		raise AssertionError(message)


def instructions(block):
	return [
		line.split("//", 1)[0].strip()
		for line in block.splitlines()
		if line.split("//", 1)[0].strip()
	]


def acknowledge_stop(total, current, num_shortfalls, longest, previous_longest):
	"""Model the state contract implemented by finish_open_shortfall."""
	if current == 0:
		return total, current, num_shortfalls, longest
	return total, 0, num_shortfalls - 1, previous_longest


def initial_flag_result(request_pending, current):
	"""Model await_sgpio's already-set flag classification."""
	if request_pending:
		# The checked stop path commits any open loss without latching an error.
		return "idle", 0, 0
	return "idle", 3, 0


def main():
	require(len(sys.argv) == 2, "usage: test_m0_stop_boundary.py sgpio_m0.s")
	source = Path(sys.argv[1]).read_text(encoding="utf-8")
	require("checked_rollback:" in source, "missing checked stop handler")
	require("finish_open_shortfall:" in source, "missing open-shortfall stop path")
	require("tx_loop:" in source, "missing end label for checked stop handler")
	require("request_or_missed:" in source, "missing shared initial-flag handler")
	await_macro = source.split(".macro await_sgpio name", 1)[1].split(".endm", 1)[0]
	require(
		"bcs \\name\\()_initial_flag" in await_macro,
		"already-set SGPIO flag bypasses request classification",
	)
	require(
		"bcs missed_deadline" not in await_macro,
		"already-set SGPIO flag still unconditionally latches a deadline error",
	)
	request_or_missed = source.split("request_or_missed:", 1)[1].split(
		"tx_initial_flag:", 1
	)[0]
	request_instructions = instructions(request_or_missed)
	require(
		"on_request checked_rollback" in request_instructions,
		"initial-set handler does not route a pending request to checked stop",
	)
	require(
		"b missed_deadline" in request_instructions,
		"initial-set handler no longer reports a true unrequested deadline miss",
	)
	for mode in ("tx", "wait", "rx"):
		veneer = source.split(f"{mode}_initial_flag:", 1)[1]
		require(
			instructions(veneer)[0] == "b request_or_missed",
			f"{mode} initial-set veneer does not reach request classifier",
		)

	checked = source.split("checked_rollback:", 1)[1].split(
		"finish_open_shortfall:", 1
	)[0]
	finish = source.split("finish_open_shortfall:", 1)[1].split("tx_loop:", 1)[0]
	checked_instructions = instructions(checked)
	finish_instructions = instructions(finish)

	# A low word of zero can still describe an open event after 4 GiB. The stop
	# path must inspect the high word and route either representation to commit.
	require(
		any("bne finish_open_shortfall" in line for line in checked_instructions),
		"low-word open shortfall does not reach the commit path",
	)
	require(
		any("#CURRENT_SHORTFALL_HIGH" in line for line in checked_instructions),
		"stop handler does not detect a high-word-only open shortfall",
	)
	require(
		any(line == "beq idle" for line in checked_instructions),
		"high-word-only open shortfall is not distinguished from an empty event",
	)

	# Exact cumulative loss is append-only for one activation. Stop commits the
	# open event by clearing current; it must never subtract from total.
	require(
		not any("#TOTAL_SHORTFALL" in line for line in finish_instructions),
		"stop acknowledgement mutates the exact cumulative loss total",
	)
	require(
		"mov length, hi_zero" in finish_instructions,
		"stop acknowledgement does not materialize zero for current loss",
	)
	require(
		any(
			line.startswith("str length,") and "#CURRENT_SHORTFALL]" in line
			for line in finish_instructions
		),
		"stop acknowledgement does not clear current_shortfall",
	)
	require(
		any(
			line.startswith("str length,") and "#CURRENT_SHORTFALL_HIGH]" in line
			for line in finish_instructions
		),
		"stop acknowledgement does not clear current_shortfall_high",
	)
	require(
		any("#LONGEST_SHORTFALL" in line for line in finish_instructions)
		and any("#NUM_SHORTFALLS" in line for line in finish_instructions),
		"legacy shortfall summaries are no longer rolled back at stop",
	)

	# Ordinary loss, a low-word wrap, and a high-word-only open event all become
	# fully confirmed. Legacy event summaries retain their historical rollback.
	for total, current in (
		(96, 32),
		(0x1_0000_0020, 0x40),
		(0x2_0000_0000, 0x1_0000_0000),
	):
		before = total - current
		result = acknowledge_stop(total, current, 7, 224, 160)
		result_total, result_current, result_count, result_longest = result
		require(result_total - result_current == total, "open loss was not committed")
		require(result_total >= before, "confirmed exact loss moved backward")
		require(result_count == 6, "legacy shortfall count was not rolled back")
		require(result_longest == 160, "legacy longest shortfall was not restored")

	require(
		acknowledge_stop(96, 0, 7, 224, 160) == (96, 0, 7, 224),
		"no-open-shortfall stop changed counters",
	)

	# m0_set_mode writes the request before raising SGPIO. RX, TX, and WAIT must
	# all treat that already-set flag as a stop wakeup, never uncertain RF loss.
	for mode in ("rx", "tx", "wait"):
		result_mode, error, current = initial_flag_result(True, 32 if mode != "wait" else 0)
		require(result_mode == "idle", f"{mode} request did not stop")
		require(error == 0, f"{mode} stop wakeup latched MISSED_DEADLINE")
		require(current == 0, f"{mode} stop did not close open-loss publication")
	result_mode, error, current = initial_flag_result(False, 32)
	require(result_mode == "idle" and error == 3 and current == 0,
		"unrequested already-set flag is not classified as a missed deadline")
	print("M0 exact-loss stop-boundary checks passed")


if __name__ == "__main__":
	main()
