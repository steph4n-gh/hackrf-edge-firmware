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

#ifndef HACKRF_SERIAL_INTERNAL_H
#define HACKRF_SERIAL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define HACKRF_USB_MAX_SERIAL_LENGTH 32

#if defined(__GNUC__) || defined(__clang__)
	#define HACKRF_SERIAL_INTERNAL_API __attribute__((visibility("hidden")))
#else
	#define HACKRF_SERIAL_INTERNAL_API
#endif

HACKRF_SERIAL_INTERNAL_API int hackrf_usb_serial_prepare(
	char* serial_number,
	size_t serial_capacity,
	int descriptor_result);

HACKRF_SERIAL_INTERNAL_API int hackrf_usb_serial_suffix_matches(
	const char* serial_number,
	int serial_number_length,
	const char* desired_serial_number,
	size_t desired_serial_length);

HACKRF_SERIAL_INTERNAL_API int hackrf_usb_serial_format_iap(
	char* serial_number,
	size_t serial_capacity,
	const uint32_t serial_words[4]);

#undef HACKRF_SERIAL_INTERNAL_API

#endif /* HACKRF_SERIAL_INTERNAL_H */
