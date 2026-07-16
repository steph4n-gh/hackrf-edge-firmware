#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep status and requested-configuration CLI fields unambiguous."""

from pathlib import Path
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


def main():
	require(len(sys.argv) == 2, "usage: test_edge_cli_output_contract.py hackrf_edge.c")
	source = Path(sys.argv[1]).read_text(encoding="utf-8")
	status = function_body(source, "static void print_status(")
	config = function_body(source, "static void print_config(")

	require(
		status.count('printf("Requested profile: %s (%u)\\n"') == 1,
		"status output lacks one Requested profile label",
	)
	require(
		config.count('printf("Config profile: %s (%u)\\n"') == 1,
		"configuration output lacks one Config profile label",
	)
	require(
		"Requested profile:" not in config,
		"configuration output reuses the status Requested profile label",
	)
	require(
		"Config profile:" not in status,
		"status output reuses the requested-configuration Config profile label",
	)
	require(
		source.count('printf("Requested profile: %s (%u)\\n"') == 1,
		"source contains more than one Requested profile formatter",
	)
	require(
		source.count('printf("Config profile: %s (%u)\\n"') == 1,
		"source contains more than one Config profile formatter",
	)
	print("Edge CLI profile-label contract checks passed")


if __name__ == "__main__":
	main()
