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

#include "usb_api_edge.h"

#include <stddef.h>
#include <string.h>

#include <fixed_point.h>
#include <firmware_info.h>
#include <m0_state.h>
#include <radio.h>
#include <transceiver_mode.h>
#include <usb_queue.h>

#include "usb_api_transceiver.h"

#define EDGE_MAGIC_FRAME 0x45474445u
#define EDGE_MAGIC_CAPABILITIES 0x43474445u
#define EDGE_MAGIC_CONFIG 0x51474445u
#define EDGE_MAGIC_STATUS 0x53474445u

#define EDGE_ABI_MAJOR 1u
#define EDGE_ABI_MINOR 0u
#define EDGE_CAPABILITIES_BYTES 64u
#define EDGE_CONFIG_BYTES 32u
#define EDGE_STATUS_BYTES 128u
#define EDGE_EVENT_BYTES 32u

#define EDGE_CONTROL_GET_CAPABILITIES 0u
#define EDGE_CONTROL_GET_STATUS 1u
#define EDGE_CONTROL_GET_CONFIG 2u
#define EDGE_CONTROL_SET_CONFIG 3u
#define EDGE_CONTROL_RESET 4u

#define EDGE_CAP_CONTROL (1u << 0)
#define EDGE_CAP_STATUS (1u << 1)
#define EDGE_CAP_FRAMED_RX (1u << 2)
#define EDGE_CAP_EXACT_LOSS_COUNT (1u << 6)

#define EDGE_CONFIG_ONE_SHOT (1u << 0)
#define EDGE_CONFIG_RESET_COUNTERS (1u << 1)
#define EDGE_CONFIG_EMIT_EVENTS (1u << 2)
#define EDGE_CONFIG_SCHEDULED (1u << 3)
#define EDGE_CONFIG_STRICT_RATE (1u << 4)
#define EDGE_CONFIG_KNOWN_FLAGS \
	(EDGE_CONFIG_ONE_SHOT | EDGE_CONFIG_RESET_COUNTERS | \
	 EDGE_CONFIG_EMIT_EVENTS | EDGE_CONFIG_SCHEDULED | EDGE_CONFIG_STRICT_RATE)

#define EDGE_FRAME_STREAM_START (1u << 0)
#define EDGE_FRAME_DISCONTINUITY (1u << 2)
#define EDGE_FRAME_DROPPED_INPUT (1u << 3)
#define EDGE_FRAME_LOSS_COUNT_UNCERTAIN (1u << 4)
#define EDGE_FRAME_RETUNE (1u << 5)
#define EDGE_FRAME_SETTLING (1u << 6)
#define EDGE_FRAME_CONFIG_CHANGED (1u << 7)
#define EDGE_FRAME_GAIN_CHANGED (1u << 8)
#define EDGE_FRAME_CLOCK_RESET (1u << 9)

#define EDGE_STATUS_CONFIG_ARMED (1u << 0)
#define EDGE_STATUS_STREAM_ACTIVE (1u << 1)
#define EDGE_STATUS_DISCONTINUITY_LATCHED (1u << 3)
#define EDGE_STATUS_EVENT_LOSS_LATCHED (1u << 4)
#define EDGE_STATUS_RX_OVERRUN_LATCHED (1u << 5)
#define EDGE_STATUS_LEGACY_FALLBACK (1u << 7)

#define EDGE_ERROR_NONE 0u
#define EDGE_ERROR_BAD_COMMAND 1u
#define EDGE_ERROR_BAD_LENGTH 2u
#define EDGE_ERROR_BAD_VERSION 3u
#define EDGE_ERROR_BAD_CRC 4u
#define EDGE_ERROR_BAD_PROFILE 5u
#define EDGE_ERROR_BAD_RATE 6u
#define EDGE_ERROR_BAD_PAYLOAD 7u
#define EDGE_ERROR_BUSY 8u
#define EDGE_ERROR_UNSUPPORTED 9u

#define EDGE_MIN_INPUT_RATE_HZ 2000000u
#define EDGE_MAX_INPUT_RATE_HZ 16000000u

#define EDGE_REQUEST_TYPE_OUT 0x40u
#define EDGE_REQUEST_TYPE_IN 0xc0u

extern volatile uint32_t usb_completed;

typedef struct {
	uint8_t requested_profile;
	uint8_t active_profile;
	bool config_armed;
	bool stream_active;
	bool quarantine_requested;
	bool legacy_fallback;
	uint32_t requested_flags;
	uint32_t requested_input_rate_hz;
	uint32_t requested_payload_bytes;
	uint32_t snapshot_sequence;
	uint32_t status_latches;
	uint32_t last_error;
	uint32_t stream_epoch;
	uint32_t config_epoch;
	uint32_t gain_epoch;
	uint32_t next_frame_sequence;
	uint32_t pending_frame_flags;
	uint64_t input_tick;
	uint64_t dropped_input_samples;
	uint64_t hardware_loss_bytes;
	uint64_t emitted_output_samples;
	uint64_t m0_count_bytes;
	uint64_t m4_count_bytes;
	uint64_t usb_completed_bytes;
	uint32_t previous_m0_count;
	uint32_t previous_m4_count;
	uint32_t previous_usb_count;
	uint64_t previous_hardware_loss_count;
	bool loss_uncertain_reported;
	bool usb_short_pending;
	uint32_t shortfall_count;
	uint32_t longest_shortfall_bytes;
	uint64_t last_transition_input_tick;
} edge_state_t;

static edge_state_t edge_state;
static uint8_t edge_control_in[EDGE_STATUS_BYTES] __attribute__((aligned(4)));
static uint8_t edge_control_out[EDGE_CONFIG_BYTES] __attribute__((aligned(4)));
static uint8_t edge_control_out_completed;

static uint32_t irq_save(void)
{
#if defined(HACKRF_EDGE_HOST_TEST)
	return 0;
#else
	uint32_t primask;
	__asm__ volatile(
		"mrs %0, primask\n"
		"cpsid i\n"
		: "=r"(primask)
		:
		: "memory");
	return primask;
#endif
}

static void irq_restore(uint32_t primask)
{
#if defined(HACKRF_EDGE_HOST_TEST)
	(void) primask;
#else
	__asm__ volatile("msr primask, %0" : : "r"(primask) : "memory");
#endif
}

static void put_u16(uint8_t* output, uint16_t value)
{
	output[0] = (uint8_t) value;
	output[1] = (uint8_t) (value >> 8);
}

static void put_u32(uint8_t* output, uint32_t value)
{
	output[0] = (uint8_t) value;
	output[1] = (uint8_t) (value >> 8);
	output[2] = (uint8_t) (value >> 16);
	output[3] = (uint8_t) (value >> 24);
}

static void put_u64(uint8_t* output, uint64_t value)
{
	put_u32(output, (uint32_t) value);
	put_u32(output + 4, (uint32_t) (value >> 32));
}

static uint32_t get_u32(const uint8_t* input)
{
	return (uint32_t) input[0] | ((uint32_t) input[1] << 8) |
		((uint32_t) input[2] << 16) | ((uint32_t) input[3] << 24);
}

static uint64_t get_u64(const uint8_t* input)
{
	return (uint64_t) get_u32(input) | ((uint64_t) get_u32(input + 4) << 32);
}

static uint32_t crc32c(const uint8_t* data, size_t length)
{
	uint32_t crc = 0xffffffffu;
	for (size_t byte = 0; byte < length; ++byte) {
		crc ^= data[byte];
		for (unsigned int bit = 0; bit < 8; ++bit) {
			const uint32_t mask = 0u - (crc & 1u);
			crc = (crc >> 1) ^ (0x82f63b78u & mask);
		}
	}
	return crc ^ 0xffffffffu;
}

static uint32_t applied_sample_rate_hz(void)
{
	const uint64_t rate =
		radio_reg_read(&radio, RADIO_BANK_APPLIED, RADIO_SAMPLE_RATE);
	if (rate == RADIO_UNSET) {
		return 0;
	}
	return (uint32_t) ((rate + (SR_FP_ONE_HZ / 2u)) / SR_FP_ONE_HZ);
}

static uint32_t requested_sample_rate_hz(void)
{
	uint64_t rate =
		radio_reg_read(&radio, RADIO_BANK_ACTIVE, RADIO_SAMPLE_RATE);
	/* An unset ACTIVE rate means the next radio_update() retains APPLIED. This
	 * preserves Edge arming for configurations established before the active
	 * bank acquired an explicit sample-rate value. */
	if (rate == RADIO_UNSET) {
		rate = radio_reg_read(
			&radio, RADIO_BANK_APPLIED, RADIO_SAMPLE_RATE);
	}
	/* The v1 Edge contract describes its rate in whole hertz. Fractional
	 * SET_SAMPLE_RATE requests cannot be represented without ambiguity. */
	if (rate == RADIO_UNSET || (rate & (SR_FP_ONE_HZ - 1u)) != 0) {
		return 0;
	}
	return (uint32_t) (rate / SR_FP_ONE_HZ);
}

static uint8_t applied_u8(radio_register_t reg)
{
	const uint64_t value = radio_reg_read(&radio, RADIO_BANK_APPLIED, reg);
	return value == RADIO_UNSET ? 0u : (uint8_t) value;
}

static void canonical_legacy_config(void)
{
	edge_state.requested_profile = EDGE_PROFILE_LEGACY_C8;
	edge_state.requested_flags = 0;
	edge_state.requested_input_rate_hz = 0;
	edge_state.requested_payload_bytes = 0;
	edge_state.config_armed = false;
}

static void latch_error(uint32_t error)
{
	const uint32_t primask = irq_save();
	edge_state.last_error = error;
	irq_restore(primask);
}

void edge_reset_state(void)
{
	const uint32_t primask = irq_save();
	const uint32_t stream_epoch = edge_state.stream_epoch + 1u;
	const uint32_t config_epoch = edge_state.config_epoch;
	const uint32_t gain_epoch = edge_state.gain_epoch;
	memset(&edge_state, 0, sizeof(edge_state));
	edge_state.stream_epoch = stream_epoch;
	edge_state.config_epoch = config_epoch;
	edge_state.gain_epoch = gain_epoch;
	edge_state.active_profile = EDGE_PROFILE_LEGACY_C8;
	canonical_legacy_config();
	/* Establish raw-counter baselines at the reset boundary so a subsequent
	 * status snapshot reports reset-local values rather than replaying the
	 * preceding activation. */
	edge_state.previous_m0_count = m0_state.m0_count;
	edge_state.previous_m4_count = m0_state.m4_count;
	edge_state.previous_usb_count = usb_completed;
	irq_restore(primask);
}

void edge_transceiver_request_superseded(uint32_t requested_mode)
{
	const uint32_t primask = irq_save();
	/* Edge framing applies only to ordinary RX. Any other requested mode
	 * supersedes an armed one-shot before the main loop can consume it, so a
	 * later ordinary RX cannot unexpectedly inherit framed transport. */
	if (requested_mode != TRANSCEIVER_MODE_RX && edge_state.config_armed &&
	    !edge_state.stream_active) {
		canonical_legacy_config();
		edge_state.active_profile = EDGE_PROFILE_LEGACY_C8;
	}
	irq_restore(primask);
}

static void build_capabilities(uint8_t* output)
{
	memset(output, 0, EDGE_CAPABILITIES_BYTES);
	put_u32(output + 0, EDGE_MAGIC_CAPABILITIES);
	output[4] = EDGE_ABI_MAJOR;
	output[5] = EDGE_ABI_MINOR;
	output[6] = EDGE_CAPABILITIES_BYTES;
	output[7] = EDGE_VENDOR_REQUEST;
	put_u32(
		output + 8,
		EDGE_CAP_CONTROL | EDGE_CAP_STATUS | EDGE_CAP_FRAMED_RX |
			EDGE_CAP_EXACT_LOSS_COUNT);
	put_u32(
		output + 12,
		(1u << EDGE_PROFILE_LEGACY_C8) | (1u << EDGE_PROFILE_FRAMED_C8));
	put_u32(output + 16, EDGE_FRAME_BYTES);
	put_u32(output + 20, EDGE_FRAME_PAYLOAD_BYTES);
	put_u32(output + 24, EDGE_MIN_INPUT_RATE_HZ);
	put_u32(output + 28, EDGE_MAX_INPUT_RATE_HZ);
	put_u16(output + 36, EDGE_FRAME_HEADER_BYTES);
	put_u16(output + 38, EDGE_CONFIG_BYTES);
	put_u16(output + 40, EDGE_STATUS_BYTES);
	put_u16(output + 42, EDGE_EVENT_BYTES);
	memcpy(output + 44, firmware_info.version_string, 16);
	put_u32(output + 60, crc32c(output, 60));
}

static void build_config(uint8_t* output)
{
	uint8_t profile;
	uint32_t flags;
	uint32_t input_rate;
	uint32_t payload_bytes;
	const uint32_t primask = irq_save();
	if (edge_state.config_armed || edge_state.stream_active) {
		profile = edge_state.requested_profile;
		flags = edge_state.requested_flags;
		input_rate = edge_state.requested_input_rate_hz;
		payload_bytes = edge_state.requested_payload_bytes;
	} else {
		profile = EDGE_PROFILE_LEGACY_C8;
		flags = 0;
		input_rate = 0;
		payload_bytes = 0;
	}
	irq_restore(primask);

	memset(output, 0, EDGE_CONFIG_BYTES);
	put_u32(output + 0, EDGE_MAGIC_CONFIG);
	output[4] = EDGE_ABI_MAJOR;
	output[5] = EDGE_ABI_MINOR;
	output[6] = EDGE_CONFIG_BYTES;
	output[7] = profile;
	put_u32(output + 8, flags);
	put_u32(output + 12, input_rate);
	put_u32(output + 16, payload_bytes);
	put_u32(output + 28, crc32c(output, 28));
}

void edge_update_runtime_counters(
	uint32_t activation_epoch,
	uint32_t m0_count,
	uint32_t m4_count,
	uint32_t usb_count,
	uint32_t shortfall_count,
	uint32_t longest_shortfall_bytes)
{
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch) {
		irq_restore(primask);
		return;
	}
	edge_state.m0_count_bytes += (uint32_t) (m0_count - edge_state.previous_m0_count);
	edge_state.m4_count_bytes += (uint32_t) (m4_count - edge_state.previous_m4_count);
	edge_state.usb_completed_bytes +=
		(uint32_t) (usb_count - edge_state.previous_usb_count);
	edge_state.previous_m0_count = m0_count;
	edge_state.previous_m4_count = m4_count;
	edge_state.previous_usb_count = usb_count;
	edge_state.shortfall_count = shortfall_count;
	edge_state.longest_shortfall_bytes = longest_shortfall_bytes;
	irq_restore(primask);
}

static void build_status(uint8_t* output)
{
	edge_state_t snapshot;
	uint32_t live_epoch = 0;
	uint8_t live_profile = EDGE_PROFILE_LEGACY_C8;
	bool live = false;
	uint32_t primask = irq_save();
	if (edge_state.stream_active) {
		live = true;
		live_epoch = edge_state.stream_epoch;
		live_profile = edge_state.active_profile;
	}
	irq_restore(primask);
	if (live) {
		edge_update_runtime_counters(
			live_epoch,
			m0_state.m0_count,
			m0_state.m4_count,
			usb_completed,
			m0_state.num_shortfalls,
			m0_state.longest_shortfall);
		if (live_profile == EDGE_PROFILE_LEGACY_C8) {
			edge_update_legacy_timeline(
				live_epoch,
				m0_confirmed_shortfall_bytes(&m0_state));
		}
	}

	primask = irq_save();
	edge_state.snapshot_sequence++;
	snapshot = edge_state;
	irq_restore(primask);

	uint32_t flags = snapshot.status_latches;
	if (snapshot.config_armed) {
		flags |= EDGE_STATUS_CONFIG_ARMED;
	}
	if (snapshot.stream_active) {
		flags |= EDGE_STATUS_STREAM_ACTIVE;
	}
	if (snapshot.legacy_fallback) {
		flags |= EDGE_STATUS_LEGACY_FALLBACK;
	}

	const uint32_t rate = applied_sample_rate_hz();
	uint64_t opmode = radio_reg_read(&radio, RADIO_BANK_APPLIED, RADIO_OPMODE);
	if (opmode == RADIO_UNSET) {
		opmode = transceiver_request.mode;
	}

	memset(output, 0, EDGE_STATUS_BYTES);
	put_u32(output + 0, EDGE_MAGIC_STATUS);
	output[4] = EDGE_ABI_MAJOR;
	output[5] = EDGE_ABI_MINOR;
	output[6] = EDGE_STATUS_BYTES;
	output[7] = snapshot.active_profile;
	put_u32(output + 8, snapshot.snapshot_sequence);
	put_u32(output + 12, flags);
	put_u32(output + 16, snapshot.last_error);
	output[20] = snapshot.requested_profile;
	output[21] = (uint8_t) opmode;
	output[22] = 2;
	output[23] = applied_u8(RADIO_GAIN_RX_RF) ? 1u : 0u;
	put_u32(output + 24, snapshot.stream_epoch);
	put_u32(output + 28, snapshot.config_epoch);
	put_u32(output + 32, snapshot.gain_epoch);
	put_u32(output + 36, snapshot.next_frame_sequence);
	put_u32(output + 40, rate);
	put_u32(output + 44, rate);
	put_u16(output + 48, 1);
	output[52] = applied_u8(RADIO_GAIN_RX_IF);
	output[53] = applied_u8(RADIO_GAIN_RX_BB);
	output[54] = applied_u8(RADIO_GAIN_TX_IF);
	put_u64(output + 56, snapshot.input_tick);
	put_u64(output + 64, snapshot.dropped_input_samples);
	put_u64(output + 72, snapshot.emitted_output_samples);
	put_u64(output + 80, snapshot.m0_count_bytes);
	put_u64(output + 88, snapshot.m4_count_bytes);
	put_u64(output + 96, snapshot.usb_completed_bytes);
	put_u32(output + 104, snapshot.shortfall_count);
	put_u32(output + 108, snapshot.longest_shortfall_bytes);
	put_u64(output + 112, snapshot.last_transition_input_tick);
	put_u32(output + 124, crc32c(output, 124));
}

static bool set_config(const uint8_t* input)
{
	if (get_u32(input + 0) != EDGE_MAGIC_CONFIG ||
	    input[4] != EDGE_ABI_MAJOR || input[5] != EDGE_ABI_MINOR ||
	    input[6] != EDGE_CONFIG_BYTES) {
		latch_error(EDGE_ERROR_BAD_VERSION);
		return false;
	}
	if (crc32c(input, 28) != get_u32(input + 28)) {
		latch_error(EDGE_ERROR_BAD_CRC);
		return false;
	}

	const uint8_t profile = input[7];
	const uint32_t flags = get_u32(input + 8);
	const uint32_t input_rate = get_u32(input + 12);
	const uint32_t payload_bytes = get_u32(input + 16);
	const uint64_t apply_at = get_u64(input + 20);

	if (flags & ~EDGE_CONFIG_KNOWN_FLAGS) {
		latch_error(EDGE_ERROR_UNSUPPORTED);
		return false;
	}
	if (edge_state.stream_active || transceiver_request.mode != TRANSCEIVER_MODE_OFF) {
		latch_error(EDGE_ERROR_BUSY);
		return false;
	}
	if (profile == EDGE_PROFILE_LEGACY_C8) {
		if (flags != 0 || input_rate != 0 || payload_bytes != 0 || apply_at != 0) {
			latch_error(EDGE_ERROR_BAD_PROFILE);
			return false;
		}
		const uint32_t primask = irq_save();
		canonical_legacy_config();
		edge_state.active_profile = EDGE_PROFILE_LEGACY_C8;
		edge_state.config_epoch++;
		edge_state.last_error = EDGE_ERROR_NONE;
		irq_restore(primask);
		return true;
	}
	if (profile != EDGE_PROFILE_FRAMED_C8) {
		latch_error(EDGE_ERROR_BAD_PROFILE);
		return false;
	}
	if (!(flags & EDGE_CONFIG_ONE_SHOT)) {
		latch_error(EDGE_ERROR_BAD_PROFILE);
		return false;
	}
	if ((flags & (EDGE_CONFIG_EMIT_EVENTS | EDGE_CONFIG_SCHEDULED)) || apply_at != 0) {
		latch_error(EDGE_ERROR_UNSUPPORTED);
		return false;
	}
	if (payload_bytes != 0 && payload_bytes != EDGE_FRAME_PAYLOAD_BYTES) {
		latch_error(EDGE_ERROR_BAD_PAYLOAD);
		return false;
	}
	/* SET_SAMPLE_RATE writes RADIO_BANK_ACTIVE before acknowledging its USB
	 * request; radio_update() copies that value to APPLIED later in the main
	 * loop. Validate the requested value here so an immediately following Edge
	 * SET_CONFIG cannot observe and reject the previous applied rate. The
	 * realized APPLIED rate is independently revalidated by edge_rx_start(). */
	const uint32_t requested_rate = requested_sample_rate_hz();
	if (requested_rate < EDGE_MIN_INPUT_RATE_HZ ||
	    requested_rate > EDGE_MAX_INPUT_RATE_HZ ||
	    (input_rate != 0 && input_rate != requested_rate)) {
		latch_error(EDGE_ERROR_BAD_RATE);
		return false;
	}

	const uint32_t primask = irq_save();
	edge_state.requested_profile = profile;
	edge_state.requested_flags = flags;
	edge_state.requested_input_rate_hz = input_rate;
	edge_state.requested_payload_bytes = payload_bytes;
	edge_state.config_armed = true;
	edge_state.legacy_fallback = false;
	edge_state.config_epoch++;
	edge_state.last_error = EDGE_ERROR_NONE;
	irq_restore(primask);
	return true;
}

static bool setup_matches(
	const usb_endpoint_t* endpoint,
	uint8_t request_type,
	uint16_t length)
{
	return endpoint->setup.request_type == request_type &&
		endpoint->setup.value == EDGE_ABI_WVALUE &&
		endpoint->setup.length == length;
}

usb_request_status_t usb_vendor_request_edge(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	const uint16_t command = endpoint->setup.index;
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		edge_control_out_completed = 0;
		switch (command) {
		case EDGE_CONTROL_GET_CAPABILITIES:
			if (!setup_matches(endpoint, EDGE_REQUEST_TYPE_IN, EDGE_CAPABILITIES_BYTES)) {
				return USB_REQUEST_STATUS_STALL;
			}
			build_capabilities(edge_control_in);
			usb_transfer_schedule_block(
				endpoint->in, edge_control_in, EDGE_CAPABILITIES_BYTES, NULL, NULL);
			usb_transfer_schedule_ack(endpoint->out);
			return USB_REQUEST_STATUS_OK;
		case EDGE_CONTROL_GET_STATUS:
			if (!setup_matches(endpoint, EDGE_REQUEST_TYPE_IN, EDGE_STATUS_BYTES)) {
				return USB_REQUEST_STATUS_STALL;
			}
			build_status(edge_control_in);
			usb_transfer_schedule_block(
				endpoint->in, edge_control_in, EDGE_STATUS_BYTES, NULL, NULL);
			usb_transfer_schedule_ack(endpoint->out);
			return USB_REQUEST_STATUS_OK;
		case EDGE_CONTROL_GET_CONFIG:
			if (!setup_matches(endpoint, EDGE_REQUEST_TYPE_IN, EDGE_CONFIG_BYTES)) {
				return USB_REQUEST_STATUS_STALL;
			}
			build_config(edge_control_in);
			usb_transfer_schedule_block(
				endpoint->in, edge_control_in, EDGE_CONFIG_BYTES, NULL, NULL);
			usb_transfer_schedule_ack(endpoint->out);
			return USB_REQUEST_STATUS_OK;
		case EDGE_CONTROL_SET_CONFIG:
			if (!setup_matches(endpoint, EDGE_REQUEST_TYPE_OUT, EDGE_CONFIG_BYTES)) {
				latch_error(EDGE_ERROR_BAD_LENGTH);
				return USB_REQUEST_STATUS_STALL;
			}
			usb_transfer_schedule_block(
				endpoint->out, edge_control_out, EDGE_CONFIG_BYTES, NULL, NULL);
			return USB_REQUEST_STATUS_OK;
		case EDGE_CONTROL_RESET:
			if (!setup_matches(endpoint, EDGE_REQUEST_TYPE_OUT, 0)) {
				return USB_REQUEST_STATUS_STALL;
			}
			/* RESET is Edge-local and must never stop or flush an existing
			 * HackRF RX/TX session. It is safe once the legacy API has requested
			 * OFF, including the brief main-loop shutdown window after its ACK. */
			if (transceiver_request.mode != TRANSCEIVER_MODE_OFF) {
				latch_error(EDGE_ERROR_BUSY);
				return USB_REQUEST_STATUS_STALL;
			}
			edge_reset_state();
			usb_transfer_schedule_ack(endpoint->in);
			return USB_REQUEST_STATUS_OK;
		default:
			latch_error(EDGE_ERROR_BAD_COMMAND);
			return USB_REQUEST_STATUS_STALL;
		}
	}

	if (stage == USB_TRANSFER_STAGE_DATA && command == EDGE_CONTROL_SET_CONFIG) {
		edge_control_out_completed =
			(uint8_t) usb_transfer_completed_length(endpoint->out);
		if (edge_control_out_completed != EDGE_CONFIG_BYTES) {
			latch_error(EDGE_ERROR_BAD_LENGTH);
			return USB_REQUEST_STATUS_STALL;
		}
		if (!set_config(edge_control_out)) {
			return USB_REQUEST_STATUS_STALL;
		}
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

bool edge_rx_start(
	uint32_t input_sample_rate_hz,
	uint32_t request_sequence,
	uint32_t* activation_epoch,
	bool* framed)
{
	const uint32_t primask = irq_save();
	if (transceiver_request.mode != TRANSCEIVER_MODE_RX ||
	    transceiver_request.seq != request_sequence) {
		if (activation_epoch != NULL) {
			*activation_epoch = edge_state.stream_epoch;
		}
		if (framed != NULL) {
			*framed = false;
		}
		irq_restore(primask);
		return false;
	}
	const bool requested_framed =
		edge_state.config_armed &&
		edge_state.requested_profile == EDGE_PROFILE_FRAMED_C8;
	const bool supported_rate =
		input_sample_rate_hz >= EDGE_MIN_INPUT_RATE_HZ &&
		input_sample_rate_hz <= EDGE_MAX_INPUT_RATE_HZ &&
		(edge_state.requested_input_rate_hz == 0 ||
		 edge_state.requested_input_rate_hz == input_sample_rate_hz);
	edge_state.active_profile = requested_framed && supported_rate ?
		EDGE_PROFILE_FRAMED_C8 : EDGE_PROFILE_LEGACY_C8;
	edge_state.config_armed = false;
	edge_state.stream_active = true;
	edge_state.stream_epoch++;
	edge_state.next_frame_sequence = 0;
	edge_state.input_tick = 0;
	edge_state.dropped_input_samples = 0;
	edge_state.hardware_loss_bytes = 0;
	edge_state.emitted_output_samples = 0;
	edge_state.m0_count_bytes = 0;
	edge_state.m4_count_bytes = 0;
	edge_state.usb_completed_bytes = 0;
	edge_state.previous_m0_count = 0;
	edge_state.previous_m4_count = 0;
	edge_state.previous_usb_count = 0;
	edge_state.previous_hardware_loss_count = 0;
	edge_state.loss_uncertain_reported = false;
	edge_state.usb_short_pending = false;
	edge_state.shortfall_count = 0;
	edge_state.longest_shortfall_bytes = 0;
	edge_state.status_latches = 0;
	edge_state.quarantine_requested = false;
	edge_state.legacy_fallback = requested_framed && !supported_rate;
	if (requested_framed && !supported_rate) {
		edge_state.last_error = EDGE_ERROR_BAD_RATE;
	}
	edge_state.pending_frame_flags =
		EDGE_FRAME_STREAM_START | EDGE_FRAME_CLOCK_RESET;
	if (edge_state.active_profile != EDGE_PROFILE_LEGACY_C8) {
		/* SET_CONFIG records acceptance; applying that distinct configuration
		 * at this activation is a second observable epoch transition. */
		edge_state.config_epoch++;
		edge_state.pending_frame_flags |= EDGE_FRAME_CONFIG_CHANGED;
	}
	const bool use_framed =
		edge_state.active_profile == EDGE_PROFILE_FRAMED_C8;
	if (activation_epoch != NULL) {
		*activation_epoch = edge_state.stream_epoch;
	}
	if (framed != NULL) {
		*framed = use_framed;
	}
	irq_restore(primask);
	return true;
}

void edge_rx_stop(uint32_t activation_epoch)
{
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch) {
		irq_restore(primask);
		return;
	}
	edge_state.stream_active = false;
	edge_state.active_profile = EDGE_PROFILE_LEGACY_C8;
	canonical_legacy_config();
	/* Preserve the activation's fallback result for post-stream diagnostics.
	 * A normal one-shot reversion to legacy is not itself a fallback. */
	edge_state.quarantine_requested = false;
	irq_restore(primask);
}

bool edge_rx_is_framed(uint32_t activation_epoch)
{
	const uint32_t primask = irq_save();
	const bool framed =
		edge_state.stream_epoch == activation_epoch &&
		edge_state.stream_active &&
		edge_state.active_profile == EDGE_PROFILE_FRAMED_C8;
	irq_restore(primask);
	return framed;
}

bool edge_build_frame_header(
	uint8_t* output,
	uint32_t input_sample_rate_hz,
	uint32_t activation_epoch)
{
	uint32_t frame_sequence;
	uint32_t stream_epoch;
	uint32_t config_epoch;
	uint32_t flags;
	uint64_t first_input_tick;
	uint64_t dropped_input_samples;

	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch ||
	    !edge_state.stream_active ||
	    edge_state.active_profile != EDGE_PROFILE_FRAMED_C8) {
		irq_restore(primask);
		return false;
	}
	frame_sequence = edge_state.next_frame_sequence++;
	stream_epoch = edge_state.stream_epoch;
	config_epoch = edge_state.config_epoch;
	flags = edge_state.pending_frame_flags;
	edge_state.pending_frame_flags = 0;
	first_input_tick = edge_state.input_tick;
	dropped_input_samples = edge_state.dropped_input_samples;
	edge_state.input_tick += EDGE_FRAME_OUTPUT_SAMPLES;
	irq_restore(primask);

	memset(output, 0, EDGE_FRAME_HEADER_BYTES);
	put_u32(output + 0, EDGE_MAGIC_FRAME);
	output[4] = EDGE_ABI_MAJOR;
	output[5] = EDGE_ABI_MINOR;
	output[6] = EDGE_FRAME_HEADER_BYTES;
	output[7] = EDGE_PROFILE_FRAMED_C8;
	put_u32(output + 8, flags);
	put_u32(output + 12, frame_sequence);
	put_u32(output + 16, stream_epoch);
	put_u32(output + 20, config_epoch);
	put_u64(output + 24, first_input_tick);
	put_u64(output + 32, dropped_input_samples);
	put_u32(output + 40, input_sample_rate_hz);
	put_u32(output + 44, EDGE_FRAME_OUTPUT_SAMPLES);
	put_u16(output + 48, 1);
	output[53] = 2;
	put_u32(output + 56, EDGE_FRAME_PAYLOAD_BYTES);
	put_u32(output + 60, crc32c(output, 60));
	return true;
}

bool edge_patch_frame_for_usb_short(uint8_t* output)
{
	/* The sole production caller holds USB0 masked from ownership validation
	 * through schedule, and the latch producer is the USB0 completion path. */
	if (!edge_state.usb_short_pending) {
		return false;
	}
	edge_state.usb_short_pending = false;

	put_u32(output + 8, get_u32(output + 8) | EDGE_FRAME_DISCONTINUITY);
	put_u32(output + 60, crc32c(output, 60));
	return true;
}

void edge_frame_completed(uint32_t activation_epoch)
{
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch == activation_epoch) {
		edge_state.emitted_output_samples += EDGE_FRAME_OUTPUT_SAMPLES;
	}
	irq_restore(primask);
}

static void note_dropped_samples(
	uint32_t activation_epoch,
	uint64_t byte_count,
	bool hardware)
{
	if (byte_count == 0) {
		return;
	}
	const uint64_t samples = byte_count / 2u;
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch) {
		irq_restore(primask);
		return;
	}
	edge_state.input_tick += samples;
	edge_state.dropped_input_samples += samples;
	edge_state.pending_frame_flags |=
		EDGE_FRAME_DISCONTINUITY | EDGE_FRAME_DROPPED_INPUT;
	edge_state.status_latches |= EDGE_STATUS_DISCONTINUITY_LATCHED;
	if (hardware) {
		edge_state.hardware_loss_bytes += byte_count;
		edge_state.status_latches |= EDGE_STATUS_RX_OVERRUN_LATCHED;
	}
	edge_state.last_transition_input_tick = edge_state.input_tick;
	irq_restore(primask);
}

void edge_note_dropped_bytes(uint32_t activation_epoch, uint32_t byte_count)
{
	note_dropped_samples(activation_epoch, byte_count, false);
}

void edge_note_hardware_loss_bytes(uint32_t activation_epoch, uint64_t byte_count)
{
	note_dropped_samples(activation_epoch, byte_count, true);
}

void edge_note_loss_uncertain(uint32_t activation_epoch)
{
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch ||
	    edge_state.loss_uncertain_reported) {
		irq_restore(primask);
		return;
	}
	edge_state.loss_uncertain_reported = true;
	edge_state.pending_frame_flags |=
		EDGE_FRAME_DISCONTINUITY | EDGE_FRAME_DROPPED_INPUT |
		EDGE_FRAME_LOSS_COUNT_UNCERTAIN;
	edge_state.status_latches |=
		EDGE_STATUS_DISCONTINUITY_LATCHED | EDGE_STATUS_RX_OVERRUN_LATCHED;
	edge_state.last_transition_input_tick = edge_state.input_tick;
	irq_restore(primask);
}

void edge_note_usb_short_transfer(uint32_t activation_epoch)
{
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch) {
		irq_restore(primask);
		return;
	}
	edge_state.usb_short_pending = true;
	edge_state.status_latches |= EDGE_STATUS_DISCONTINUITY_LATCHED;
	irq_restore(primask);
}

void edge_update_legacy_timeline(
	uint32_t activation_epoch,
	uint64_t confirmed_hardware_loss_bytes)
{
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch) {
		irq_restore(primask);
		return;
	}
	/* Ordered M0 publication makes a torn 64-bit snapshot move only backward.
	 * Ignore it and accept every forward delta smaller than half the 64-bit
	 * domain. A single physical activation cannot approach that bound. */
	const int64_t loss_delta = (int64_t) (
		confirmed_hardware_loss_bytes -
		edge_state.previous_hardware_loss_count);
	if (loss_delta > 0) {
		edge_state.hardware_loss_bytes += (uint64_t) loss_delta;
		edge_state.previous_hardware_loss_count =
			confirmed_hardware_loss_bytes;
	}
	edge_state.input_tick =
		(edge_state.m0_count_bytes + edge_state.hardware_loss_bytes) / 2u;
	edge_state.dropped_input_samples = edge_state.hardware_loss_bytes / 2u;
	edge_state.emitted_output_samples = edge_state.usb_completed_bytes / 2u;
	irq_restore(primask);
}

void edge_radio_changed(uint32_t changed)
{
	const uint32_t relevant =
		RADIO_REG_GROUP_FREQ | RADIO_REG_GROUP_RATE | RADIO_REG_GROUP_GAIN;
	if (!(changed & relevant)) {
		return;
	}

	const uint32_t primask = irq_save();
	if (!edge_state.stream_active) {
		irq_restore(primask);
		return;
	}
	if (changed & RADIO_REG_GROUP_FREQ) {
		edge_state.pending_frame_flags |=
			EDGE_FRAME_RETUNE | EDGE_FRAME_SETTLING |
			EDGE_FRAME_DISCONTINUITY;
		edge_state.status_latches |= EDGE_STATUS_DISCONTINUITY_LATCHED;
	}
	if (changed & RADIO_REG_GROUP_RATE) {
		edge_state.pending_frame_flags |=
			EDGE_FRAME_CONFIG_CHANGED | EDGE_FRAME_SETTLING |
			EDGE_FRAME_DISCONTINUITY;
		edge_state.status_latches |= EDGE_STATUS_DISCONTINUITY_LATCHED;
		edge_state.config_epoch++;
	}
	if (changed & RADIO_REG_GROUP_GAIN) {
		edge_state.pending_frame_flags |= EDGE_FRAME_GAIN_CHANGED;
		edge_state.gain_epoch++;
	}
	edge_state.last_transition_input_tick = edge_state.input_tick;
	if (edge_state.active_profile == EDGE_PROFILE_FRAMED_C8) {
		edge_state.quarantine_requested = true;
	}
	irq_restore(primask);
}

bool edge_take_quarantine_request(uint32_t activation_epoch)
{
	const uint32_t primask = irq_save();
	if (edge_state.stream_epoch != activation_epoch) {
		irq_restore(primask);
		return false;
	}
	const bool requested = edge_state.quarantine_requested;
	edge_state.quarantine_requested = false;
	irq_restore(primask);
	return requested;
}
