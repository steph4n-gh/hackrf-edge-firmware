/*
 * Copyright 2026 HackRF Edge contributors
 *
 * This file is part of HackRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <usb_request.h>
#include <usb_type.h>

#define EDGE_VENDOR_REQUEST 0xe0u
#define EDGE_ABI_WVALUE 0x0100u

#define EDGE_FRAME_HEADER_BYTES 64u
#define EDGE_FRAME_BYTES 0x4000u
#define EDGE_FRAME_PAYLOAD_BYTES (EDGE_FRAME_BYTES - EDGE_FRAME_HEADER_BYTES)
#define EDGE_FRAME_OUTPUT_SAMPLES (EDGE_FRAME_PAYLOAD_BYTES / 2u)

typedef enum {
	EDGE_PROFILE_LEGACY_C8 = 0,
	EDGE_PROFILE_FRAMED_C8 = 1,
} edge_profile_t;

usb_request_status_t usb_vendor_request_edge(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage);

void edge_reset_state(void);
void edge_transceiver_request_superseded(uint32_t requested_mode);
bool edge_rx_start(
	uint32_t input_sample_rate_hz,
	uint32_t request_sequence,
	uint32_t* activation_epoch,
	bool* framed);
void edge_rx_stop(uint32_t activation_epoch);
bool edge_rx_is_framed(uint32_t activation_epoch);

bool edge_build_frame_header(
	uint8_t* header,
	uint32_t input_sample_rate_hz,
	uint32_t activation_epoch);
bool edge_patch_frame_for_usb_short(uint8_t* header);
void edge_frame_completed(uint32_t activation_epoch);
void edge_note_dropped_bytes(uint32_t activation_epoch, uint32_t byte_count);
void edge_note_hardware_loss_bytes(uint32_t activation_epoch, uint64_t byte_count);
void edge_note_loss_uncertain(uint32_t activation_epoch);
void edge_note_usb_short_transfer(uint32_t activation_epoch);

void edge_update_runtime_counters(
	uint32_t activation_epoch,
	uint32_t m0_count,
	uint32_t m4_count,
	uint32_t usb_count,
	uint32_t shortfall_count,
	uint32_t longest_shortfall_bytes);
void edge_update_legacy_timeline(
	uint32_t activation_epoch,
	uint64_t confirmed_hardware_loss_bytes);

void edge_radio_changed(uint32_t changed);
bool edge_take_quarantine_request(uint32_t activation_epoch);
