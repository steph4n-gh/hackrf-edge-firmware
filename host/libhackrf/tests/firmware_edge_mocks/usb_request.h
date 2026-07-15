/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "usb_type.h"

typedef enum {
	USB_TRANSFER_STAGE_SETUP,
	USB_TRANSFER_STAGE_DATA,
	USB_TRANSFER_STAGE_STATUS,
} usb_transfer_stage_t;

typedef enum {
	USB_REQUEST_STATUS_OK = 0,
	USB_REQUEST_STATUS_STALL = 1,
} usb_request_status_t;
