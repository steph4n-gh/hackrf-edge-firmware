/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdint.h>

typedef struct {
	uint8_t request_type;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
} usb_setup_t;

typedef struct _usb_endpoint_t {
	usb_setup_t setup;
	struct _usb_endpoint_t* in;
	struct _usb_endpoint_t* out;
} usb_endpoint_t;
