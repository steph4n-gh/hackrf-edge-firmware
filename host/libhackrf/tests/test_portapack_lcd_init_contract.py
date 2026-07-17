#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify that LCD-init size optimizations preserve both command traces."""

from pathlib import Path
import re
import sys


CALL = "portapack_lcd_data_write_command_and_data"


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
	raise AssertionError(f"unterminated block: {signature}")


def strip_comments(source):
	source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
	return re.sub(r"//[^\n]*", "", source)


def c_integer(expression):
	expression = " ".join(expression.split())
	require(
		re.fullmatch(r"[0-9A-Fa-fxXbB()<>|\s]+", expression) is not None,
		f"unsupported integer expression: {expression!r}",
	)
	value = eval(expression, {"__builtins__": {}}, {})
	require(isinstance(value, int) and 0 <= value <= 0xFF, "byte out of range")
	return value


def initializer_bytes(body):
	return [c_integer(item) for item in body.split(",") if item.strip()]


def static_arrays(source):
	arrays = {}
	pattern = re.compile(
		r"static\s+const\s+uint8_t\s+(cmd_[0-9a-f]+)\s*"
		r"\[\s*\]\s*=\s*\{(.*?)\}\s*;",
		re.DOTALL | re.IGNORECASE,
	)
	for match in pattern.finditer(strip_comments(source)):
		name = match.group(1)
		require(name not in arrays, f"duplicate command array: {name}")
		arrays[name] = initializer_bytes(match.group(2))
	return arrays


def split_arguments(arguments):
	parts = []
	start = 0
	depth = {"(": 0, "[": 0, "{": 0}
	closing = {")": "(", "]": "[", "}": "{"}
	for offset, character in enumerate(arguments):
		if character in depth:
			depth[character] += 1
		elif character in closing:
			depth[closing[character]] -= 1
		elif character == "," and all(value == 0 for value in depth.values()):
			parts.append(arguments[start:offset].strip())
			start = offset + 1
	parts.append(arguments[start:].strip())
	return parts


def call_arguments(source):
	source = strip_comments(source)
	position = 0
	while True:
		position = source.find(CALL, position)
		if position < 0:
			return
		open_paren = source.find("(", position + len(CALL))
		require(open_paren >= 0, "LCD command call has no opening parenthesis")
		depth = 0
		for offset in range(open_paren, len(source)):
			if source[offset] == "(":
				depth += 1
			elif source[offset] == ")":
				depth -= 1
				if depth == 0:
					yield split_arguments(source[open_paren + 1 : offset])
					position = offset + 1
					break
		else:
			raise AssertionError("unterminated LCD command call")


def command_trace(source):
	arrays = static_arrays(source)
	trace = []
	for arguments in call_arguments(source):
		require(len(arguments) == 3, f"unexpected LCD call: {arguments}")
		command = c_integer(arguments[0])
		data = arguments[1]
		length = arguments[2]
		if data == "0":
			payload = []
		elif data.startswith("(const uint8_t[])"):
			open_brace = data.index("{")
			close_brace = data.rindex("}")
			payload = initializer_bytes(data[open_brace + 1 : close_brace])
		else:
			require(data in arrays, f"LCD payload is not static: {data}")
			payload = arrays[data]

		if length.startswith("ARRAY_SIZEOF("):
			match = re.fullmatch(r"ARRAY_SIZEOF\((cmd_[0-9a-f]+)\)", length)
			require(match is not None and match.group(1) == data, "size/data mismatch")
		else:
			require(c_integer(length) == len(payload), "literal payload length mismatch")
		trace.append((command, payload))
	return trace


def main():
	require(len(sys.argv) == 2, "usage: test_portapack_lcd_init_contract.py portapack.c")
	source = Path(sys.argv[1]).read_text(encoding="utf-8")
	init = function_body(source, "static void portapack_lcd_init(")
	portarf = function_body(init, "if (dispid == 5537894)")
	default = init[init.index("//any other portapack display") :]

	portarf_expected = [
		(0xE0, [0x00, 0x09, 0x0C, 0x03, 0x10, 0x06, 0x34, 0x68, 0x49, 0x02, 0x0A, 0x07, 0x2C, 0x31, 0x0F]),
		(0xE1, [0x00, 0x12, 0x15, 0x02, 0x10, 0x06, 0x35, 0x35, 0x4A, 0x05, 0x10, 0x0C, 0x2F, 0x33, 0x0F]),
		(0xC0, [0x0F, 0x0F]),
		(0xC1, [0x44]),
		(0xC5, [0x00, 0x66, 0x80]),
		(0x36, [0x98]),
		(0x3A, [0x65]),
		(0xB1, [0xA0, 0x11]),
		(0xB4, [0x02]),
		(0xEE, [0x00, 0x04]),
		(0xE9, [0x00]),
		(0xF7, [0xA9, 0x51, 0x2C, 0x82]),
		(0x21, []),
		(0x11, []),
		(0x29, []),
		(0x35, [0x00]),
	]
	default_expected = [
		(0xCF, [0x00, 0xD9, 0x30]),
		(0xED, [0x64, 0x03, 0x12, 0x81]),
		(0xE8, [0x85, 0x10, 0x78]),
		(0xCB, [0x39, 0x2C, 0x00, 0x34, 0x02]),
		(0xF7, [0x20]),
		(0xEA, [0x00, 0x00]),
		(0xB1, [0x00, 0x1B]),
		(0xB5, [0x02, 0x02, 0x0A, 0x14]),
		(0xB6, [0x0A, 0xA2, 0x27, 0x00]),
		(0xC0, [0x1B]),
		(0xC1, [0x12]),
		(0xC5, [0x32, 0x3C]),
		(0xC7, [0x9B]),
		(0x36, [0xD8]),
		(0x3A, [0x55]),
		(0xF6, [0x01, 0x30, 0x00]),
		(0xF2, [0x00]),
		(0x26, [0x01]),
		(0xE0, [0x0F, 0x1D, 0x19, 0x0E, 0x10, 0x07, 0x4C, 0x63, 0x3F, 0x03, 0x0D, 0x00, 0x26, 0x24, 0x04]),
		(0xE1, [0x00, 0x1C, 0x1F, 0x02, 0x0F, 0x03, 0x35, 0x25, 0x47, 0x04, 0x0C, 0x0B, 0x29, 0x2F, 0x05]),
		(0x35, [0x00]),
	]

	require(command_trace(portarf) == portarf_expected, "PortaRF LCD command trace changed")
	require(command_trace(default) == default_expected, "default LCD command trace changed")
	require(
		re.search(r"(?m)^\s*const\s+uint8_t\s+cmd_", init) is None,
		"named LCD payload remains automatic rather than static",
	)
	print("PortaPack LCD initialization command traces passed")


if __name__ == "__main__":
	main()
