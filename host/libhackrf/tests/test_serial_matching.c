/*
 * Copyright (c) 2026 HackRF Edge contributors
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "hackrf_serial_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                     \
	do {                                                 \
		if (!(condition)) {                          \
			fprintf(stderr,                      \
				"%s:%d: check failed: %s\n", \
				__FILE__,                    \
				__LINE__,                    \
				#condition);                 \
			exit(1);                             \
		}                                            \
	} while (0)

static void test_suffix_semantics(void)
{
	char serial_number[HACKRF_USB_MAX_SERIAL_LENGTH + 1] =
		"000000000000000087c867dc2985255f";
	int serial_number_length;

	serial_number_length = hackrf_usb_serial_prepare(
		serial_number,
		sizeof(serial_number),
		HACKRF_USB_MAX_SERIAL_LENGTH);
	CHECK(serial_number_length == HACKRF_USB_MAX_SERIAL_LENGTH);
	CHECK(hackrf_usb_serial_suffix_matches(
		serial_number,
		serial_number_length,
		"000000000000000087c867dc2985255f",
		32u));
	CHECK(hackrf_usb_serial_suffix_matches(
		serial_number,
		serial_number_length,
		"2985255f",
		8u));
	CHECK(hackrf_usb_serial_suffix_matches(
		serial_number,
		serial_number_length,
		"",
		0u));
	CHECK(!hackrf_usb_serial_suffix_matches(
		serial_number,
		serial_number_length,
		"2985255F",
		8u));
}

static void test_desired_serial_longer_than_descriptor(void)
{
	char serial_number[4] = "abc";
	int serial_number_length;

	serial_number_length =
		hackrf_usb_serial_prepare(serial_number, sizeof(serial_number), 3);
	CHECK(serial_number_length == 3);
	CHECK(!hackrf_usb_serial_suffix_matches(
		serial_number,
		serial_number_length,
		"xabc",
		4u));
}

static void test_negative_descriptor_result(void)
{
	char serial_number[9] = "sentinel";
	char original[sizeof(serial_number)];
	int serial_number_length;

	memcpy(original, serial_number, sizeof(serial_number));
	serial_number_length =
		hackrf_usb_serial_prepare(serial_number, sizeof(serial_number), -5);
	CHECK(serial_number_length < 0);
	CHECK(memcmp(serial_number, original, sizeof(serial_number)) == 0);
	CHECK(!hackrf_usb_serial_suffix_matches(
		serial_number,
		serial_number_length,
		"nel",
		3u));
}

static void test_descriptor_length_is_bounded(void)
{
	char serial_number[HACKRF_USB_MAX_SERIAL_LENGTH + 1];
	int serial_number_length;

	memset(serial_number, 'a', sizeof(serial_number));
	serial_number_length =
		hackrf_usb_serial_prepare(serial_number, sizeof(serial_number), 64);
	CHECK(serial_number_length == HACKRF_USB_MAX_SERIAL_LENGTH);
	CHECK(serial_number[HACKRF_USB_MAX_SERIAL_LENGTH] == '\0');
}

static void test_ram_descriptor_falls_back_to_iap_identity(void)
{
	const uint32_t serial_words[4] = {
		0x00000000u,
		0x00000000u,
		0x87c867dcu,
		0x2985255fu,
	};
	char descriptor[15] = "RunningFromRAM";
	char hardware_serial[HACKRF_USB_MAX_SERIAL_LENGTH + 1];
	int descriptor_length;
	int hardware_serial_length;

	descriptor_length = hackrf_usb_serial_prepare(descriptor, sizeof(descriptor), 14);
	CHECK(!hackrf_usb_serial_suffix_matches(
		descriptor,
		descriptor_length,
		"000000000000000087c867dc2985255f",
		32u));

	hardware_serial_length = hackrf_usb_serial_format_iap(
		hardware_serial,
		sizeof(hardware_serial),
		serial_words);
	CHECK(hardware_serial_length == HACKRF_USB_MAX_SERIAL_LENGTH);
	CHECK(strcmp(hardware_serial, "000000000000000087c867dc2985255f") == 0);
	CHECK(hackrf_usb_serial_suffix_matches(
		hardware_serial,
		hardware_serial_length,
		"2985255f",
		8u));
}

static void test_iap_format_rejects_short_output(void)
{
	const uint32_t serial_words[4] = {0u, 0u, 0u, 1u};
	char hardware_serial[HACKRF_USB_MAX_SERIAL_LENGTH];

	CHECK(hackrf_usb_serial_format_iap(
		      hardware_serial,
		      sizeof(hardware_serial),
		      serial_words) < 0);
}

int main(void)
{
	test_suffix_semantics();
	test_desired_serial_longer_than_descriptor();
	test_negative_descriptor_result();
	test_descriptor_length_is_bounded();
	test_ram_descriptor_falls_back_to_iap_identity();
	test_iap_format_rejects_short_output();
	return 0;
}
