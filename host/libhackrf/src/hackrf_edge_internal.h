/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: BSD-3-Clause */
#ifndef HACKRF_EDGE_INTERNAL_H
#define HACKRF_EDGE_INTERNAL_H

#include "hackrf.h"

/* libusb's vendor/device request type values, kept out of the codec source. */
#define HACKRF_EDGE_CONTROL_IN         0xc0u
#define HACKRF_EDGE_CONTROL_OUT        0x40u
#define HACKRF_EDGE_USB_ERROR_PIPE     (-9)
#define HACKRF_EDGE_CONTROL_TIMEOUT_MS 100u

#if defined(__GNUC__) || defined(__clang__)
	#define HACKRF_EDGE_INTERNAL_API __attribute__((visibility("hidden")))
#else
	#define HACKRF_EDGE_INTERNAL_API
#endif

/* Implemented by hackrf.c and replaced by the offline unit-test transport. */
HACKRF_EDGE_INTERNAL_API int hackrf_edge_control_transfer(
	hackrf_device* device,
	uint8_t request_type,
	uint8_t request,
	uint16_t value,
	uint16_t index,
	unsigned char* data,
	uint16_t length,
	unsigned int timeout_ms);

#undef HACKRF_EDGE_INTERNAL_API

#endif /* HACKRF_EDGE_INTERNAL_H */
