/*
 * Copyright 2022 Great Scott Gadgets <info@greatscottgadgets.com>
 * Modified for HackRF Edge in 2026 by HackRF Edge contributors.
 *
 * This file is part of HackRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "usb_api_m0_state.h"

#include <stddef.h>

#include <m0_state.h>
#include <usb_queue.h>
#include <usb_request.h>
#include <usb_type.h>

enum { LEGACY_M0_STATE_BYTES = 40 };
typedef char legacy_m0_state_layout_must_remain_40_bytes
	[(offsetof(struct m0_state, total_shortfall) == LEGACY_M0_STATE_BYTES) ? 1 : -1];
typedef char edge_m0_loss_layout_must_remain_stable
	[(offsetof(struct m0_state, current_shortfall) == 44u &&
	  offsetof(struct m0_state, total_shortfall_high) == 48u &&
	  offsetof(struct m0_state, current_shortfall_high) == 52u &&
	  sizeof(struct m0_state) == 56u) ? 1 : -1];

usb_request_status_t usb_vendor_request_get_m0_state(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		/* Preserve the established 40-byte USB response. Edge appends an
		 * internal exact-loss counter to the shared structure, but exposes it
		 * through the versioned Edge status ABI instead of changing this
		 * existing request. */
		usb_transfer_schedule_block(
			endpoint->in,
			(void*) &m0_state,
			LEGACY_M0_STATE_BYTES,
			NULL,
			NULL);
		usb_transfer_schedule_ack(endpoint->out);
		return USB_REQUEST_STATUS_OK;
	} else {
		return USB_REQUEST_STATUS_OK;
	}
}
