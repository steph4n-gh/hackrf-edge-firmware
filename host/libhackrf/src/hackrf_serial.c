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

#include <string.h>

int hackrf_usb_serial_prepare(
	char* serial_number,
	size_t serial_capacity,
	int descriptor_result)
{
	size_t serial_number_length;

	if (serial_number == NULL || serial_capacity == 0u || descriptor_result < 0) {
		return -1;
	}

	serial_number_length = (size_t) descriptor_result;
	if (serial_number_length > HACKRF_USB_MAX_SERIAL_LENGTH) {
		serial_number_length = HACKRF_USB_MAX_SERIAL_LENGTH;
	}
	if (serial_number_length >= serial_capacity) {
		serial_number_length = serial_capacity - 1u;
	}

	serial_number[serial_number_length] = '\0';
	return (int) serial_number_length;
}

int hackrf_usb_serial_suffix_matches(
	const char* serial_number,
	int serial_number_length,
	const char* desired_serial_number,
	size_t desired_serial_length)
{
	if (serial_number == NULL || desired_serial_number == NULL ||
	    serial_number_length < 0 ||
	    desired_serial_length > (size_t) serial_number_length) {
		return 0;
	}

	return memcmp(serial_number +
			      ((size_t) serial_number_length - desired_serial_length),
		      desired_serial_number,
		      desired_serial_length) == 0;
}

int hackrf_usb_serial_format_iap(
	char* serial_number,
	size_t serial_capacity,
	const uint32_t serial_words[4])
{
	static const char hex_digits[] = "0123456789abcdef";
	size_t word_index;
	size_t nibble_index;

	if (serial_number == NULL ||
	    serial_capacity < HACKRF_USB_MAX_SERIAL_LENGTH + 1u || serial_words == NULL) {
		return -1;
	}

	for (word_index = 0u; word_index < 4u; word_index++) {
		for (nibble_index = 0u; nibble_index < 8u; nibble_index++) {
			const uint32_t nibble = (serial_words[word_index] >>
						 (28u - (uint32_t) nibble_index * 4u)) &
				0xfu;
			serial_number[word_index * 8u + nibble_index] =
				hex_digits[nibble];
		}
	}
	serial_number[HACKRF_USB_MAX_SERIAL_LENGTH] = '\0';
	return HACKRF_USB_MAX_SERIAL_LENGTH;
}
