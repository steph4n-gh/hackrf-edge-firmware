/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdint.h>

#include "usb_type.h"

typedef void (*transfer_completion_cb)(void*, unsigned int);

int usb_transfer_schedule_block(
	const usb_endpoint_t* endpoint,
	void* data,
	uint32_t maximum_length,
	transfer_completion_cb completion_cb,
	void* user_data);
int usb_transfer_schedule_ack(const usb_endpoint_t* endpoint);
unsigned int usb_transfer_completed_length(const usb_endpoint_t* endpoint);
