/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Offline behavioral tests for the production HackRF Edge firmware module. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_api_edge.h"
#include "usb_api_transceiver.h"

#include <firmware_info.h>
#include <fixed_point.h>
#include <m0_state.h>
#include <radio.h>
#include <usb_queue.h>

#define CHECK(condition)                                                        \
	do {                                                                      \
		if (!(condition)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,       \
				__LINE__, #condition);                                  \
			exit(1);                                                      \
		}                                                                 \
	} while (0)

enum {
	REQUEST_OUT = 0x40,
	REQUEST_IN = 0xc0,
	COMMAND_CAPABILITIES = 0,
	COMMAND_STATUS = 1,
	COMMAND_CONFIG = 2,
	COMMAND_SET_CONFIG = 3,
	COMMAND_RESET = 4,
	CONFIG_ONE_SHOT = 1u << 0,
	FRAME_STREAM_START = 1u << 0,
	FRAME_DISCONTINUITY = 1u << 2,
	FRAME_DROPPED_INPUT = 1u << 3,
	FRAME_LOSS_COUNT_UNCERTAIN = 1u << 4,
	FRAME_RETUNE = 1u << 5,
	FRAME_SETTLING = 1u << 6,
	FRAME_CONFIG_CHANGED = 1u << 7,
	FRAME_GAIN_CHANGED = 1u << 8,
	FRAME_CLOCK_RESET = 1u << 9,
	STATUS_CONFIG_ARMED = 1u << 0,
	STATUS_STREAM_ACTIVE = 1u << 1,
	STATUS_DISCONTINUITY = 1u << 3,
	STATUS_RX_OVERRUN = 1u << 5,
	STATUS_LEGACY_FALLBACK = 1u << 7,
	ERROR_BAD_LENGTH = 2,
};

volatile struct m0_state m0_state;
volatile transceiver_request_t transceiver_request;
volatile uint32_t usb_completed;
radio_t radio;
const struct firmware_info_t firmware_info = {
	.version_string = "edge-test-4ec914",
};

static uint64_t radio_values[2][8];
static void* scheduled_data;
static uint32_t scheduled_length;
static const usb_endpoint_t* scheduled_endpoint;
static unsigned int ack_count;
static const usb_endpoint_t* ack_endpoint;
static unsigned int completed_length;

static usb_endpoint_t endpoint_in;
static usb_endpoint_t endpoint_out;
static usb_endpoint_t endpoint_control;

uint64_t radio_reg_read(
	radio_t* instance,
	radio_register_bank_t bank,
	radio_register_t reg)
{
	(void) instance;
	return radio_values[bank][reg];
}

int usb_transfer_schedule_block(
	const usb_endpoint_t* endpoint,
	void* data,
	uint32_t maximum_length,
	transfer_completion_cb completion_cb,
	void* user_data)
{
	(void) completion_cb;
	(void) user_data;
	scheduled_endpoint = endpoint;
	scheduled_data = data;
	scheduled_length = maximum_length;
	return 0;
}

int usb_transfer_schedule_ack(const usb_endpoint_t* endpoint)
{
	ack_count++;
	ack_endpoint = endpoint;
	return 0;
}

unsigned int usb_transfer_completed_length(const usb_endpoint_t* endpoint)
{
	CHECK(endpoint == &endpoint_out);
	return completed_length;
}

static uint16_t get_u16(const uint8_t* data)
{
	return (uint16_t) data[0] | ((uint16_t) data[1] << 8);
}

static uint32_t get_u32(const uint8_t* data)
{
	return (uint32_t) data[0] | ((uint32_t) data[1] << 8) |
		((uint32_t) data[2] << 16) | ((uint32_t) data[3] << 24);
}

static uint64_t get_u64(const uint8_t* data)
{
	return (uint64_t) get_u32(data) | ((uint64_t) get_u32(data + 4) << 32);
}

static void put_u32(uint8_t* data, uint32_t value)
{
	data[0] = (uint8_t) value;
	data[1] = (uint8_t) (value >> 8);
	data[2] = (uint8_t) (value >> 16);
	data[3] = (uint8_t) (value >> 24);
}

static uint32_t crc32c(const uint8_t* data, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t byte;
	for (byte = 0; byte < length; ++byte) {
		unsigned int bit;
		crc ^= data[byte];
		for (bit = 0; bit < 8; ++bit) {
			const uint32_t mask = 0u - (crc & 1u);
			crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & mask);
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static void set_radio_rate_hz(radio_register_bank_t bank, uint32_t rate_hz)
{
	radio_values[bank][RADIO_SAMPLE_RATE] =
		(uint64_t) rate_hz * SR_FP_ONE_HZ;
}

static void clear_schedule(void)
{
	scheduled_data = NULL;
	scheduled_length = 0;
	scheduled_endpoint = NULL;
	ack_count = 0;
	ack_endpoint = NULL;
}

static usb_request_status_t setup_request(
	uint8_t type,
	uint16_t command,
	uint16_t length)
{
	clear_schedule();
	endpoint_control.setup.request_type = type;
	endpoint_control.setup.request = EDGE_VENDOR_REQUEST;
	endpoint_control.setup.value = EDGE_ABI_WVALUE;
	endpoint_control.setup.index = command;
	endpoint_control.setup.length = length;
	return usb_vendor_request_edge(&endpoint_control, USB_TRANSFER_STAGE_SETUP);
}

static void read_control(uint16_t command, uint8_t* output, uint16_t length)
{
	CHECK(setup_request(REQUEST_IN, command, length) == USB_REQUEST_STATUS_OK);
	CHECK(scheduled_endpoint == &endpoint_in);
	CHECK(scheduled_length == length);
	CHECK(ack_count == 1 && ack_endpoint == &endpoint_out);
	memcpy(output, scheduled_data, length);
}

static usb_request_status_t set_config(
	uint8_t profile,
	uint32_t flags,
	uint32_t rate,
	uint32_t payload,
	int corrupt_crc)
{
	uint8_t config[32] = {0};
	put_u32(config + 0, UINT32_C(0x51474445));
	config[4] = 1;
	config[5] = 0;
	config[6] = 32;
	config[7] = profile;
	put_u32(config + 8, flags);
	put_u32(config + 12, rate);
	put_u32(config + 16, payload);
	put_u32(config + 28, crc32c(config, 28));
	if (corrupt_crc) {
		config[28] ^= 1u;
	}

	CHECK(setup_request(REQUEST_OUT, COMMAND_SET_CONFIG, 32) ==
	      USB_REQUEST_STATUS_OK);
	CHECK(scheduled_endpoint == &endpoint_out);
	CHECK(scheduled_length == sizeof(config));
	memcpy(scheduled_data, config, sizeof(config));
	completed_length = sizeof(config);
	clear_schedule();
	endpoint_control.setup.request_type = REQUEST_OUT;
	endpoint_control.setup.value = EDGE_ABI_WVALUE;
	endpoint_control.setup.index = COMMAND_SET_CONFIG;
	endpoint_control.setup.length = 32;
	return usb_vendor_request_edge(&endpoint_control, USB_TRANSFER_STAGE_DATA);
}

static usb_request_status_t replay_short_config(unsigned int actual_length)
{
	uint8_t config[32] = {0};
	put_u32(config + 0, UINT32_C(0x51474445));
	config[4] = 1;
	config[6] = 32;
	config[7] = EDGE_PROFILE_FRAMED_C8;
	put_u32(config + 8, CONFIG_ONE_SHOT);
	put_u32(config + 12, 8000000);
	put_u32(config + 28, crc32c(config, 28));

	CHECK(actual_length < sizeof(config));
	CHECK(setup_request(REQUEST_OUT, COMMAND_SET_CONFIG, 32) ==
	      USB_REQUEST_STATUS_OK);
	CHECK(scheduled_endpoint == &endpoint_out);
	memcpy(scheduled_data, config, actual_length);
	completed_length = actual_length;
	clear_schedule();
	return usb_vendor_request_edge(&endpoint_control, USB_TRANSFER_STAGE_DATA);
}

static void reset_fixture(void)
{
	memset((void*) &m0_state, 0, sizeof(m0_state));
	memset((void*) &transceiver_request, 0, sizeof(transceiver_request));
	memset(radio_values, 0, sizeof(radio_values));
	usb_completed = 0;
	completed_length = 0;
	radio_values[RADIO_BANK_APPLIED][RADIO_SAMPLE_RATE] =
		UINT64_C(8000000) * SR_FP_ONE_HZ;
	radio_values[RADIO_BANK_ACTIVE][RADIO_SAMPLE_RATE] =
		UINT64_C(8000000) * SR_FP_ONE_HZ;
	radio_values[RADIO_BANK_APPLIED][RADIO_OPMODE] = TRANSCEIVER_MODE_OFF;
	radio_values[RADIO_BANK_ACTIVE][RADIO_OPMODE] = TRANSCEIVER_MODE_OFF;
	endpoint_control.in = &endpoint_in;
	endpoint_control.out = &endpoint_out;
	edge_reset_state();
}

static void test_capabilities_and_request_shape(void)
{
	uint8_t bytes[64];
	read_control(COMMAND_CAPABILITIES, bytes, sizeof(bytes));
	CHECK(get_u32(bytes + 0) == UINT32_C(0x43474445));
	CHECK(bytes[4] == 1 && bytes[5] == 0 && bytes[6] == 64);
	CHECK(bytes[7] == EDGE_VENDOR_REQUEST);
	CHECK(get_u32(bytes + 8) == UINT32_C(0x47));
	CHECK(get_u32(bytes + 12) == UINT32_C(0x03));
	CHECK(get_u32(bytes + 16) == 16384);
	CHECK(get_u32(bytes + 20) == 16320);
	CHECK(get_u32(bytes + 24) == 2000000);
	CHECK(get_u32(bytes + 28) == 16000000);
	CHECK(get_u32(bytes + 32) == 0);
	CHECK(get_u16(bytes + 36) == 64);
	CHECK(get_u16(bytes + 38) == 32);
	CHECK(get_u16(bytes + 40) == 128);
	CHECK(get_u16(bytes + 42) == 32);
	CHECK(memcmp(bytes + 44, firmware_info.version_string, 16) == 0);
	CHECK(get_u32(bytes + 60) == crc32c(bytes, 60));

	CHECK(setup_request(REQUEST_OUT, COMMAND_CAPABILITIES, 64) ==
	      USB_REQUEST_STATUS_STALL);
	endpoint_control.setup.request_type = REQUEST_IN;
	endpoint_control.setup.value = 0;
	CHECK(usb_vendor_request_edge(&endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(setup_request(REQUEST_IN, COMMAND_CAPABILITIES, 63) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(setup_request(REQUEST_IN, 99, 64) == USB_REQUEST_STATUS_STALL);

	CHECK(transceiver_clamp_shortfall_limit(0) == 0);
	CHECK(transceiver_clamp_shortfall_limit(UINT32_C(0xffffffe0)) ==
	      UINT32_C(0xffffffe0));
	CHECK(transceiver_clamp_shortfall_limit(UINT32_C(0xffffffe1)) ==
	      UINT32_C(0xffffffe0));
	CHECK(transceiver_clamp_shortfall_limit(UINT32_MAX) ==
	      UINT32_C(0xffffffe0));
}

static void test_config_frames_loss_and_transitions(void)
{
	uint8_t config[32];
	uint8_t header[64];
	uint8_t status[128];
	uint32_t epoch;
	uint32_t activation_epoch;
	uint32_t accepted_config_epoch;
	bool framed;

	read_control(COMMAND_CONFIG, config, sizeof(config));
	CHECK(config[7] == EDGE_PROFILE_LEGACY_C8);
	CHECK(get_u32(config + 8) == 0 && get_u32(config + 12) == 0);
	CHECK(get_u32(config + 16) == 0 && get_u64(config + 20) == 0);
	CHECK(get_u32(config + 28) == crc32c(config, 28));

	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
			      16320,
			      0) == USB_REQUEST_STATUS_OK);
	CHECK(ack_count == 1 && ack_endpoint == &endpoint_in);
	read_control(COMMAND_STATUS, status, sizeof(status));
	accepted_config_epoch = get_u32(status + 28);
	CHECK(accepted_config_epoch != 0);
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(framed);

	CHECK(edge_build_frame_header(header, 8000000, activation_epoch));
	CHECK(get_u32(header + 0) == UINT32_C(0x45474445));
	CHECK(header[7] == EDGE_PROFILE_FRAMED_C8);
	CHECK(get_u32(header + 8) ==
	      (FRAME_STREAM_START | FRAME_CONFIG_CHANGED | FRAME_CLOCK_RESET));
	CHECK(get_u32(header + 12) == 0);
	epoch = get_u32(header + 16);
	CHECK(epoch != 0);
	CHECK(get_u32(header + 20) == accepted_config_epoch + 1u);
	CHECK(get_u64(header + 24) == 0);
	CHECK(get_u64(header + 32) == 0);
	CHECK(get_u32(header + 40) == 8000000);
	CHECK(get_u32(header + 44) == 8160);
	CHECK(get_u16(header + 48) == 1 && get_u16(header + 50) == 0);
	CHECK(header[52] == 0 && header[53] == 2);
	CHECK(get_u16(header + 54) == 0 && get_u32(header + 56) == 16320);
	CHECK(get_u32(header + 60) == crc32c(header, 60));

	edge_note_hardware_loss_bytes(activation_epoch, 64);
	edge_note_dropped_bytes(activation_epoch, 32);
	CHECK(edge_build_frame_header(header, 8000000, activation_epoch));
	CHECK(get_u32(header + 12) == 1 && get_u32(header + 16) == epoch);
	CHECK(get_u64(header + 24) == 8208);
	CHECK(get_u64(header + 32) == 48);
	CHECK((get_u32(header + 8) &
	       (FRAME_DISCONTINUITY | FRAME_DROPPED_INPUT)) ==
	      (FRAME_DISCONTINUITY | FRAME_DROPPED_INPUT));

	/* USB transport loss is exact from the next sequence/tick gap. It must
	 * not be mislabeled as raw ADC input loss. */
	edge_note_usb_short_transfer(activation_epoch);
	CHECK(edge_build_frame_header(header, 8000000, activation_epoch));
	CHECK(get_u32(header + 12) == 2);
	CHECK(get_u64(header + 24) == 16368);
	CHECK(get_u64(header + 32) == 48);
	CHECK(get_u32(header + 8) == 0);
	CHECK(edge_patch_frame_for_usb_short(header));
	CHECK(get_u32(header + 8) == FRAME_DISCONTINUITY);
	CHECK(get_u32(header + 60) == crc32c(header, 60));

	edge_radio_changed(RADIO_REG_GROUP_FREQ | RADIO_REG_GROUP_GAIN);
	CHECK(edge_take_quarantine_request(activation_epoch));
	CHECK(!edge_take_quarantine_request(activation_epoch));
	CHECK(edge_build_frame_header(header, 8000000, activation_epoch));
	CHECK((get_u32(header + 8) &
	       (FRAME_RETUNE | FRAME_SETTLING | FRAME_DISCONTINUITY |
		FRAME_GAIN_CHANGED)) ==
	      (FRAME_RETUNE | FRAME_SETTLING | FRAME_DISCONTINUITY |
	       FRAME_GAIN_CHANGED));
	edge_frame_completed(activation_epoch);

	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(status[7] == EDGE_PROFILE_FRAMED_C8);
	CHECK((get_u32(status + 12) &
	       (STATUS_STREAM_ACTIVE | STATUS_DISCONTINUITY |
		STATUS_RX_OVERRUN)) ==
	      (STATUS_STREAM_ACTIVE | STATUS_DISCONTINUITY |
	       STATUS_RX_OVERRUN));
	CHECK((get_u32(status + 12) & STATUS_LEGACY_FALLBACK) == 0);
	CHECK(get_u64(status + 64) == 48);
	CHECK(get_u64(status + 72) == 8160);
	CHECK(get_u32(status + 28) == accepted_config_epoch + 1u);
	CHECK(get_u16(status + 50) == 0);
	CHECK(get_u32(status + 120) == 0);
	CHECK(get_u32(status + 124) == crc32c(status, 124));

	edge_rx_stop(activation_epoch);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(status[7] == EDGE_PROFILE_LEGACY_C8);
	CHECK((get_u32(status + 12) & STATUS_STREAM_ACTIVE) == 0);
	CHECK((get_u32(status + 12) & STATUS_LEGACY_FALLBACK) == 0);
	read_control(COMMAND_CONFIG, config, sizeof(config));
	CHECK(config[7] == EDGE_PROFILE_LEGACY_C8 && get_u32(config + 8) == 0);
}

static void test_loss_monotonicity_and_reset_isolation(void)
{
	uint8_t status[128];
	uint32_t activation_epoch;
	uint32_t rejected_epoch;
	bool framed;
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_OK);
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(framed);
	edge_note_hardware_loss_bytes(activation_epoch, 64);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 56) == 32);
	CHECK(get_u64(status + 64) == 32);
	CHECK((get_u32(status + 12) &
	       (STATUS_DISCONTINUITY | STATUS_RX_OVERRUN)) ==
	      (STATUS_DISCONTINUITY | STATUS_RX_OVERRUN));
	/* The legacy OFF request is ACKed before the firmware RX loop fully exits.
	 * Edge RESET is valid in that shutdown window and does not issue OFF. */
	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	CHECK(setup_request(REQUEST_OUT, COMMAND_RESET, 0) ==
	      USB_REQUEST_STATUS_OK);
	const uint32_t stale_epoch = activation_epoch;
	m0_state.m0_count = 1024;
	m0_state.m4_count = 768;
	m0_state.num_shortfalls = 7;
	m0_state.longest_shortfall = 224;
	usb_completed = 512;
	edge_note_hardware_loss_bytes(stale_epoch, 64);
	edge_note_dropped_bytes(stale_epoch, 32);
	edge_note_usb_short_transfer(stale_epoch);
	edge_frame_completed(stale_epoch);
	edge_update_runtime_counters(
		stale_epoch, 1024, 768, 512, 7, 224);
	edge_update_legacy_timeline(stale_epoch, 64);
	edge_rx_stop(stale_epoch);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u32(status + 12) == 0);
	CHECK(get_u64(status + 56) == 0);
	CHECK(get_u64(status + 64) == 0);
	CHECK(get_u64(status + 72) == 0);
	CHECK(get_u64(status + 80) == 0);
	CHECK(get_u64(status + 88) == 0);
	CHECK(get_u64(status + 96) == 0);
	CHECK(get_u32(status + 104) == 0);
	CHECK(get_u32(status + 108) == 0);
	framed = true;
	CHECK(!edge_rx_start(
		8000000,
		transceiver_request.seq - 1u,
		&rejected_epoch,
		&framed));
	CHECK(!framed);
	CHECK(rejected_epoch != stale_epoch);
}

static void test_short_completion_patches_ready_header(void)
{
	uint8_t frame_a[64];
	uint8_t frame_b[64];
	uint8_t frame_c[64];
	uint32_t activation_epoch;
	bool framed;

	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_OK);
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(framed);

	/* Model the RX-loop order: A is in flight, B is already READY, A completes
	 * short, and C is reserved/built before B is scheduled. Building C must
	 * not consume the short latch; B is patched immediately before USB. */
	CHECK(edge_build_frame_header(frame_a, 8000000, activation_epoch));
	CHECK(edge_build_frame_header(frame_b, 8000000, activation_epoch));
	CHECK(get_u32(frame_b + 12) == 1);
	CHECK(get_u32(frame_b + 8) == 0);
	edge_note_usb_short_transfer(activation_epoch);
	CHECK(edge_build_frame_header(frame_c, 8000000, activation_epoch));
	CHECK(get_u32(frame_c + 12) == 2);
	CHECK((get_u32(frame_c + 8) & FRAME_DISCONTINUITY) == 0);
	CHECK(edge_patch_frame_for_usb_short(frame_b));
	CHECK(get_u32(frame_b + 8) == FRAME_DISCONTINUITY);
	CHECK(get_u32(frame_b + 60) == crc32c(frame_b, 60));
	CHECK(!edge_patch_frame_for_usb_short(frame_b));

	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	edge_rx_stop(activation_epoch);
}

static void test_short_set_config_cannot_replay_stale_buffer(void)
{
	uint8_t status[128];
	uint32_t activation_epoch;
	uint32_t config_epoch;
	bool framed;

	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_OK);
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(framed);
	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	edge_rx_stop(activation_epoch);

	read_control(COMMAND_STATUS, status, sizeof(status));
	config_epoch = get_u32(status + 28);
	CHECK((get_u32(status + 12) & STATUS_CONFIG_ARMED) == 0);

	/* The OUT buffer still contains the preceding valid config. A ZLP or a
	 * 31-byte retry must be rejected by its actual dTD byte count before that
	 * persistent buffer can be parsed and rearm the one-shot. */
	CHECK(replay_short_config(0) == USB_REQUEST_STATUS_STALL);
	CHECK(ack_count == 0);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u32(status + 16) == ERROR_BAD_LENGTH);
	CHECK(get_u32(status + 28) == config_epoch);
	CHECK((get_u32(status + 12) & STATUS_CONFIG_ARMED) == 0);

	CHECK(replay_short_config(31) == USB_REQUEST_STATUS_STALL);
	CHECK(ack_count == 0);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u32(status + 16) == ERROR_BAD_LENGTH);
	CHECK(get_u32(status + 28) == config_epoch);
	CHECK((get_u32(status + 12) & STATUS_CONFIG_ARMED) == 0);
}

static void test_legacy_timeline_unwrap(void)
{
	uint8_t status[128];
	uint32_t activation_epoch;
	bool framed;

	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(!framed);

	m0_state.m0_count = 100;
	m0_state.m4_count = 80;
	m0_state.total_shortfall = 64;
	m0_state.current_shortfall = 0;
	usb_completed = 60;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 56) == 82);
	CHECK(get_u64(status + 64) == 32);
	CHECK(get_u64(status + 72) == 30);

	/* A current-before-total publication window can look backward. It is
	 * ignored rather than decreasing the public logical timeline. */
	m0_state.m0_count = 140;
	m0_state.m4_count = 120;
	m0_state.current_shortfall = 32;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 56) == 102);
	CHECK(get_u64(status + 64) == 32);

	m0_state.total_shortfall = 96;
	m0_state.current_shortfall = 0;
	usb_completed = 100;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 56) == 118);
	CHECK(get_u64(status + 64) == 48);
	CHECK(get_u64(status + 72) == 50);

	/* One still-open event may legitimately cross 2^31 bytes. It remains
	 * unconfirmed until recovery, then advances the public timeline exactly. */
	m0_state.total_shortfall = UINT32_C(0x80000080);
	m0_state.current_shortfall = UINT32_C(0x80000020);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 64) == 48);
	m0_state.current_shortfall = 0;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 64) == UINT64_C(0x40000040));
	CHECK(get_u64(status + 56) == UINT64_C(0x40000086));

	/* The total counter's low-first/high-second wrap publication can only
	 * appear backward between the two stores, then resumes exactly. */
	m0_state.total_shortfall = UINT32_C(0xffffffe0);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 64) == UINT64_C(0x7ffffff0));
	m0_state.total_shortfall = 0;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 64) == UINT64_C(0x7ffffff0));
	m0_state.total_shortfall_high = 1;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 64) == UINT64_C(0x80000000));

	/* Current publishes high first at its low-word wrap. Each possible torn
	 * snapshot stays at or behind the last confirmed 2^32-byte position. */
	m0_state.total_shortfall_high = 1;
	m0_state.total_shortfall = UINT32_C(0xffffffe0);
	m0_state.current_shortfall = UINT32_C(0xffffffe0);
	read_control(COMMAND_STATUS, status, sizeof(status));
	m0_state.current_shortfall_high = 1;
	read_control(COMMAND_STATUS, status, sizeof(status));
	m0_state.current_shortfall = 0;
	read_control(COMMAND_STATUS, status, sizeof(status));
	m0_state.total_shortfall = 0;
	read_control(COMMAND_STATUS, status, sizeof(status));
	m0_state.total_shortfall_high = 2;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 64) == UINT64_C(0x80000000));

	/* Recovery confirms the complete 4 GiB open event in one update. */
	m0_state.current_shortfall_high = 0;
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 64) == UINT64_C(0x100000000));

	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	edge_rx_stop(activation_epoch);
}

static void test_wide_and_uncertain_framed_loss(void)
{
	uint8_t header[64];
	uint8_t status[128];
	uint32_t activation_epoch;
	bool framed;

	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_OK);
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(framed);

	edge_note_hardware_loss_bytes(
		activation_epoch, UINT64_C(0x100000040));
	edge_note_loss_uncertain(activation_epoch);
	edge_note_loss_uncertain(activation_epoch);
	CHECK(edge_build_frame_header(header, 8000000, activation_epoch));
	CHECK(get_u64(header + 24) == UINT64_C(0x80000020));
	CHECK(get_u64(header + 32) == UINT64_C(0x80000020));
	CHECK((get_u32(header + 8) &
	       (FRAME_DISCONTINUITY | FRAME_DROPPED_INPUT |
		FRAME_LOSS_COUNT_UNCERTAIN)) ==
	      (FRAME_DISCONTINUITY | FRAME_DROPPED_INPUT |
	       FRAME_LOSS_COUNT_UNCERTAIN));
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u64(status + 56) == UINT64_C(0x80002000));
	CHECK(get_u64(status + 64) == UINT64_C(0x80000020));
	CHECK((get_u32(status + 12) &
	       (STATUS_DISCONTINUITY | STATUS_RX_OVERRUN)) ==
	      (STATUS_DISCONTINUITY | STATUS_RX_OVERRUN));

	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	edge_rx_stop(activation_epoch);
}

static void test_fallback_reset_and_rejection(void)
{
	uint8_t status[128];
	uint32_t before_epoch;
	uint32_t activation_epoch;
	bool framed;

	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_OK);
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		10000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(!framed);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK((get_u32(status + 12) & STATUS_LEGACY_FALLBACK) != 0);
	CHECK(get_u32(status + 16) == 6);
	edge_rx_stop(activation_epoch);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK((get_u32(status + 12) & STATUS_LEGACY_FALLBACK) != 0);

	/* Malformed input is atomic and latches the precise CRC error. */
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      1) == USB_REQUEST_STATUS_STALL);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u32(status + 16) == 4);

	/* Edge RESET never changes or flushes an active legacy TX/RX API. */
	transceiver_request.mode = TRANSCEIVER_MODE_TX;
	CHECK(setup_request(REQUEST_OUT, COMMAND_RESET, 0) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(transceiver_request.mode == TRANSCEIVER_MODE_TX);
	CHECK(ack_count == 0);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u32(status + 16) == 8);

	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	read_control(COMMAND_STATUS, status, sizeof(status));
	before_epoch = get_u32(status + 24);
	CHECK(setup_request(REQUEST_OUT, COMMAND_RESET, 0) ==
	      USB_REQUEST_STATUS_OK);
	CHECK(ack_count == 1 && ack_endpoint == &endpoint_in);
	read_control(COMMAND_STATUS, status, sizeof(status));
	CHECK(get_u32(status + 24) == before_epoch + 1u);
	CHECK(get_u32(status + 12) == 0);
	CHECK(get_u32(status + 16) == 0);

	/* Canonical legacy configuration is all-zero beyond its profile. */
	CHECK(set_config(EDGE_PROFILE_LEGACY_C8, CONFIG_ONE_SHOT, 0, 0, 0) ==
	      USB_REQUEST_STATUS_STALL);
}

static void test_sample_rate_ack_to_edge_arm_race(void)
{
	static const struct {
		uint32_t old_applied_rate;
		uint32_t new_requested_rate;
	} boundary_cases[] = {
		{1999999, 2000000},
		{16000001, 16000000},
	};
	uint8_t status[128];
	uint32_t activation_epoch;
	bool framed;

	for (size_t i = 0; i < sizeof(boundary_cases) / sizeof(boundary_cases[0]); ++i) {
		const uint32_t old_rate = boundary_cases[i].old_applied_rate;
		const uint32_t new_rate = boundary_cases[i].new_requested_rate;

		reset_fixture();
		set_radio_rate_hz(RADIO_BANK_APPLIED, old_rate);
		set_radio_rate_hz(RADIO_BANK_ACTIVE, new_rate);

		/* The immediately preceding SET_SAMPLE_RATE ACK exposes ACTIVE=new
		 * while APPLIED still contains old. Arming follows the request, not the
		 * stale realized value, and refuses a config naming that stale value. */
		CHECK(set_config(
			      EDGE_PROFILE_FRAMED_C8,
			      CONFIG_ONE_SHOT,
			      old_rate,
			      0,
			      0) == USB_REQUEST_STATUS_STALL);
		CHECK(set_config(
			      EDGE_PROFILE_FRAMED_C8,
			      CONFIG_ONE_SHOT,
			      new_rate,
			      0,
			      0) == USB_REQUEST_STATUS_OK);

		/* If RX somehow starts before the requested rate is applied, the
		 * caller's APPLIED-rate snapshot remains the authoritative activation
		 * check and safely falls back to legacy transport. */
		transceiver_request.mode = TRANSCEIVER_MODE_RX;
		transceiver_request.seq++;
		CHECK(edge_rx_start(
			old_rate,
			transceiver_request.seq,
			&activation_epoch,
			&framed));
		CHECK(!framed);
		read_control(COMMAND_STATUS, status, sizeof(status));
		CHECK((get_u32(status + 12) & STATUS_LEGACY_FALLBACK) != 0);
		CHECK(get_u32(status + 16) == 6);
		transceiver_request.mode = TRANSCEIVER_MODE_OFF;
		edge_rx_stop(activation_epoch);

		/* In normal dispatch radio_update() completes first. The exact same
		 * arm then activates framed RX at each inclusive rate boundary. */
		reset_fixture();
		set_radio_rate_hz(RADIO_BANK_APPLIED, old_rate);
		set_radio_rate_hz(RADIO_BANK_ACTIVE, new_rate);
		CHECK(set_config(
			      EDGE_PROFILE_FRAMED_C8,
			      CONFIG_ONE_SHOT,
			      new_rate,
			      0,
			      0) == USB_REQUEST_STATUS_OK);
		set_radio_rate_hz(RADIO_BANK_APPLIED, new_rate);
		transceiver_request.mode = TRANSCEIVER_MODE_RX;
		transceiver_request.seq++;
		CHECK(edge_rx_start(
			new_rate,
			transceiver_request.seq,
			&activation_epoch,
			&framed));
		CHECK(framed);
		transceiver_request.mode = TRANSCEIVER_MODE_OFF;
		edge_rx_stop(activation_epoch);
	}
}

static void test_requested_rate_validation(void)
{
	uint32_t activation_epoch;
	bool framed;

	/* An unset ACTIVE rate explicitly inherits a valid APPLIED rate. */
	reset_fixture();
	radio_values[RADIO_BANK_ACTIVE][RADIO_SAMPLE_RATE] = RADIO_UNSET;
	set_radio_rate_hz(RADIO_BANK_APPLIED, 8000000);
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_OK);
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(framed);
	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	edge_rx_stop(activation_epoch);

	/* No effective rate, a fractional-Hz request, and either rate just
	 * outside the inclusive v1 range remain unrepresentable/rejected. */
	reset_fixture();
	radio_values[RADIO_BANK_ACTIVE][RADIO_SAMPLE_RATE] = RADIO_UNSET;
	radio_values[RADIO_BANK_APPLIED][RADIO_SAMPLE_RATE] = RADIO_UNSET;
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8, CONFIG_ONE_SHOT, 0, 0, 0) ==
	      USB_REQUEST_STATUS_STALL);

	reset_fixture();
	radio_values[RADIO_BANK_ACTIVE][RADIO_SAMPLE_RATE] =
		UINT64_C(8000000) * SR_FP_ONE_HZ + (SR_FP_ONE_HZ / 2u);
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_STALL);

	reset_fixture();
	radio_values[RADIO_BANK_ACTIVE][RADIO_SAMPLE_RATE] = RADIO_UNSET;
	radio_values[RADIO_BANK_APPLIED][RADIO_SAMPLE_RATE] =
		UINT64_C(8000000) * SR_FP_ONE_HZ + (SR_FP_ONE_HZ / 2u);
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_STALL);

	reset_fixture();
	set_radio_rate_hz(RADIO_BANK_ACTIVE, 1999999);
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8, CONFIG_ONE_SHOT, 0, 0, 0) ==
	      USB_REQUEST_STATUS_STALL);

	reset_fixture();
	set_radio_rate_hz(RADIO_BANK_ACTIVE, 16000001);
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8, CONFIG_ONE_SHOT, 0, 0, 0) ==
	      USB_REQUEST_STATUS_STALL);

	reset_fixture();
	radio_values[RADIO_BANK_ACTIVE][RADIO_SAMPLE_RATE] = RADIO_UNSET;
	set_radio_rate_hz(RADIO_BANK_APPLIED, 1999999);
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8, CONFIG_ONE_SHOT, 0, 0, 0) ==
	      USB_REQUEST_STATUS_STALL);

	reset_fixture();
	radio_values[RADIO_BANK_ACTIVE][RADIO_SAMPLE_RATE] = RADIO_UNSET;
	set_radio_rate_hz(RADIO_BANK_APPLIED, 16000001);
	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8, CONFIG_ONE_SHOT, 0, 0, 0) ==
	      USB_REQUEST_STATUS_STALL);
}

static void test_nonordinary_mode_consumes_one_shot(void)
{
	uint32_t activation_epoch;
	bool framed = true;

	CHECK(set_config(
		      EDGE_PROFILE_FRAMED_C8,
		      CONFIG_ONE_SHOT,
		      8000000,
		      0,
		      0) == USB_REQUEST_STATUS_OK);
	/* Sweep supersedes the armed one-shot. Passing through OFF and then
	 * starting an ordinary RX must not resurrect latent framing. */
	edge_transceiver_request_superseded(TRANSCEIVER_MODE_RX_SWEEP);
	transceiver_request.mode = TRANSCEIVER_MODE_RX_SWEEP;
	transceiver_request.seq++;
	edge_transceiver_request_superseded(TRANSCEIVER_MODE_OFF);
	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	transceiver_request.seq++;
	transceiver_request.mode = TRANSCEIVER_MODE_RX;
	transceiver_request.seq++;
	CHECK(edge_rx_start(
		8000000, transceiver_request.seq, &activation_epoch, &framed));
	CHECK(!framed);
	transceiver_request.mode = TRANSCEIVER_MODE_OFF;
	edge_rx_stop(activation_epoch);
}

int main(void)
{
	reset_fixture();
	test_capabilities_and_request_shape();
	reset_fixture();
	test_config_frames_loss_and_transitions();
	reset_fixture();
	test_short_completion_patches_ready_header();
	reset_fixture();
	test_short_set_config_cannot_replay_stale_buffer();
	reset_fixture();
	test_loss_monotonicity_and_reset_isolation();
	reset_fixture();
	test_legacy_timeline_unwrap();
	reset_fixture();
	test_wide_and_uncertain_framed_loss();
	reset_fixture();
	test_fallback_reset_and_rejection();
	reset_fixture();
	test_sample_rate_ack_to_edge_arm_race();
	reset_fixture();
	test_requested_rate_validation();
	reset_fixture();
	test_nonordinary_mode_consumes_one_shot();
	puts("HackRF Edge firmware protocol tests passed");
	return 0;
}
