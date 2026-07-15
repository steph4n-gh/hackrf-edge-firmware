/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: BSD-3-Clause */
#include "hackrf.h"
#include "hackrf_edge_internal.h"

#include <stdio.h>
#include <string.h>

#define MOCK_MAX_STEPS 4
#define MOCK_MAX_DATA  128

typedef struct mock_step {
	uint8_t request_type;
	uint16_t index;
	uint16_t length;
	int result;
	int compare_output;
	unsigned char data[MOCK_MAX_DATA];
} mock_step;

static mock_step mock_steps[MOCK_MAX_STEPS];
static int mock_step_count;
static int mock_step_index;
static int failures;
static unsigned char dummy_device_storage;

#define CHECK(condition)                                     \
	do {                                                 \
		if (!(condition)) {                          \
			fprintf(stderr,                      \
				"%s:%d: check failed: %s\n", \
				__FILE__,                    \
				__LINE__,                    \
				#condition);                 \
			failures++;                          \
			return;                              \
		}                                            \
	} while (0)

static hackrf_device* mock_device(void)
{
	return (hackrf_device*) &dummy_device_storage;
}

static void mock_reset(void)
{
	memset(mock_steps, 0, sizeof(mock_steps));
	mock_step_count = 0;
	mock_step_index = 0;
}

static mock_step* mock_add(
	uint8_t request_type,
	uint16_t index,
	uint16_t length,
	int result)
{
	mock_step* step;

	if (mock_step_count >= MOCK_MAX_STEPS) {
		failures++;
		return &mock_steps[MOCK_MAX_STEPS - 1];
	}
	step = &mock_steps[mock_step_count++];
	step->request_type = request_type;
	step->index = index;
	step->length = length;
	step->result = result;
	return step;
}

static void mock_add_input(
	uint16_t index,
	uint16_t length,
	int result,
	const unsigned char* data)
{
	mock_step* step = mock_add(HACKRF_EDGE_CONTROL_IN, index, length, result);
	size_t copy_length = 0;

	if (result > 0) {
		copy_length = (size_t) result;
		if (copy_length > length) {
			copy_length = length;
		}
	}
	if (data != NULL && copy_length != 0u) {
		memcpy(step->data, data, copy_length);
	}
}

static void mock_add_output(
	uint16_t index,
	uint16_t length,
	int result,
	const unsigned char* expected)
{
	mock_step* step = mock_add(HACKRF_EDGE_CONTROL_OUT, index, length, result);
	step->compare_output = 1;
	if (expected != NULL && length != 0u) {
		memcpy(step->data, expected, length);
	}
}

int hackrf_edge_control_transfer(
	hackrf_device* device,
	uint8_t request_type,
	uint8_t request,
	uint16_t value,
	uint16_t index,
	unsigned char* data,
	uint16_t length,
	unsigned int timeout_ms)
{
	mock_step* step;
	size_t copy_length;

	if (device != mock_device() || mock_step_index >= mock_step_count) {
		failures++;
		return -1;
	}
	step = &mock_steps[mock_step_index++];
	if (request_type != step->request_type || request != HACKRF_EDGE_VENDOR_REQUEST ||
	    value != 0x0100u || index != step->index || length != step->length ||
	    timeout_ms != 100u) {
		failures++;
		return -1;
	}
	if (step->compare_output) {
		if ((length == 0u && data != NULL) ||
		    (length != 0u &&
		     (data == NULL || memcmp(data, step->data, length) != 0))) {
			failures++;
			return -1;
		}
	} else if (step->result > 0 && data != NULL) {
		copy_length = (size_t) step->result;
		if (copy_length > length) {
			copy_length = length;
		}
		memcpy(data, step->data, copy_length);
	}
	return step->result;
}

static int mock_complete(void)
{
	return mock_step_index == mock_step_count;
}

static uint32_t get_u32(const unsigned char* bytes)
{
	return ((uint32_t) bytes[0]) | ((uint32_t) bytes[1] << 8) |
		((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static uint64_t get_u64(const unsigned char* bytes)
{
	return get_u32(bytes) | ((uint64_t) get_u32(bytes + 4) << 32);
}

static void put_u16(unsigned char* bytes, uint16_t value)
{
	bytes[0] = (unsigned char) value;
	bytes[1] = (unsigned char) (value >> 8);
}

static void put_u32(unsigned char* bytes, uint32_t value)
{
	bytes[0] = (unsigned char) value;
	bytes[1] = (unsigned char) (value >> 8);
	bytes[2] = (unsigned char) (value >> 16);
	bytes[3] = (unsigned char) (value >> 24);
}

static void put_u64(unsigned char* bytes, uint64_t value)
{
	put_u32(bytes, (uint32_t) value);
	put_u32(bytes + 4, (uint32_t) (value >> 32));
}

static uint32_t test_crc32c(const unsigned char* bytes, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++) {
			crc = (crc & 1u) != 0u ? (crc >> 1) ^ UINT32_C(0x82f63b78) :
						 crc >> 1;
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static void build_capabilities(
	unsigned char* bytes,
	uint32_t capability_flags,
	uint32_t profile_mask)
{
	unsigned int i;

	memset(bytes, 0, 64);
	put_u32(bytes, UINT32_C(0x43474445));
	bytes[4] = 1;
	bytes[5] = 0;
	bytes[6] = 64;
	bytes[7] = 0xe0;
	put_u32(bytes + 8, capability_flags);
	put_u32(bytes + 12, profile_mask);
	put_u32(bytes + 16, 4160);
	put_u32(bytes + 20, 4096);
	put_u32(bytes + 24, 2000000);
	put_u32(bytes + 28, 20000000);
	put_u32(bytes + 32, 1000000);
	put_u16(bytes + 36, 64);
	put_u16(bytes + 38, 32);
	put_u16(bytes + 40, 128);
	put_u16(bytes + 42, 32);
	for (i = 0; i < 16; i++) {
		bytes[44 + i] = (unsigned char) (0xa0u + i);
	}
	put_u32(bytes + 60, test_crc32c(bytes, 60));
}

static void build_config(
	unsigned char* bytes,
	uint8_t profile,
	uint32_t flags,
	uint32_t rate,
	uint32_t payload,
	uint64_t tick)
{
	memset(bytes, 0, 32);
	put_u32(bytes, UINT32_C(0x51474445));
	bytes[4] = 1;
	bytes[5] = 0;
	bytes[6] = 32;
	bytes[7] = profile;
	put_u32(bytes + 8, flags);
	put_u32(bytes + 12, rate);
	put_u32(bytes + 16, payload);
	put_u64(bytes + 20, tick);
	put_u32(bytes + 28, test_crc32c(bytes, 28));
}

static void build_status(unsigned char* bytes)
{
	memset(bytes, 0, 128);
	put_u32(bytes, UINT32_C(0x53474445));
	bytes[4] = 1;
	bytes[5] = 0;
	bytes[6] = 128;
	bytes[7] = HACKRF_EDGE_PROFILE_FRAMED_C16_D4;
	put_u32(bytes + 8, 7);
	put_u32(bytes + 12,
		HACKRF_EDGE_STATUS_CONFIG_ARMED | HACKRF_EDGE_STATUS_STREAM_ACTIVE);
	put_u32(bytes + 16, HACKRF_EDGE_ERROR_NONE);
	bytes[20] = HACKRF_EDGE_PROFILE_FRAMED_C16_D4;
	bytes[21] = 1;
	bytes[22] = 4;
	bytes[23] = 1;
	put_u32(bytes + 24, 11);
	put_u32(bytes + 28, 12);
	put_u32(bytes + 32, 13);
	put_u32(bytes + 36, 14);
	put_u32(bytes + 40, 20000000);
	put_u32(bytes + 44, 5000000);
	put_u16(bytes + 48, 4);
	put_u16(bytes + 50, 0x0180);
	bytes[52] = 16;
	bytes[53] = 20;
	bytes[54] = 10;
	put_u64(bytes + 56, UINT64_C(0x0102030405060708));
	put_u64(bytes + 64, 17);
	put_u64(bytes + 72, 18);
	put_u64(bytes + 80, 19);
	put_u64(bytes + 88, 20);
	put_u64(bytes + 96, 21);
	put_u32(bytes + 104, 22);
	put_u32(bytes + 108, 23);
	put_u64(bytes + 112, 24);
	put_u32(bytes + 120, 25);
	put_u32(bytes + 124, test_crc32c(bytes, 124));
}

static void refresh_crc(unsigned char* bytes, size_t covered)
{
	put_u32(bytes + covered, test_crc32c(bytes, covered));
}

static void test_crc_check_value(void)
{
	static const unsigned char check[] = "123456789";
	CHECK(test_crc32c(check, 9) == UINT32_C(0xe3069283));
}

static void test_capabilities_valid(void)
{
	unsigned char bytes[64];
	hackrf_edge_capabilities capabilities;
	int result;

	build_capabilities(bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	mock_reset();
	mock_add_input(0, 64, 64, bytes);
	memset(&capabilities, 0, sizeof(capabilities));
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_SUCCESS);
	CHECK(mock_complete());
	CHECK(capabilities.abi_major == 1 && capabilities.abi_minor == 0);
	CHECK(capabilities.profile_mask == UINT32_C(0x0f));
	CHECK(capabilities.max_frame_bytes == 4160);
	CHECK(capabilities.max_payload_bytes == 4096);
	CHECK(capabilities.frame_header_bytes == 64);
	CHECK(capabilities.build_id[0] == 0xa0 && capabilities.build_id[15] == 0xaf);

	build_capabilities(
		bytes,
		HACKRF_EDGE_CAP_CONTROL | HACKRF_EDGE_CAP_STATUS,
		UINT32_C(0x01));
	put_u32(bytes + 16, 0);
	put_u32(bytes + 20, 0);
	refresh_crc(bytes, 60);
	mock_reset();
	mock_add_input(0, 64, 64, bytes);
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_SUCCESS);
	CHECK(capabilities.max_frame_bytes == 0);
}

static void test_discovery_errors(void)
{
	unsigned char bytes[64];
	hackrf_edge_capabilities capabilities;
	int result;

	mock_reset();
	mock_add_input(0, 64, HACKRF_EDGE_USB_ERROR_PIPE, NULL);
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_ERROR_EDGE_UNSUPPORTED);
	CHECK(mock_complete());

	build_capabilities(bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	mock_reset();
	mock_add_input(0, 64, 63, bytes);
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);
	CHECK(mock_complete());

	mock_reset();
	mock_add_input(0, 64, -7, NULL);
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_ERROR_LIBUSB);
	CHECK(mock_complete());
}

static void test_capabilities_malformed(void)
{
	unsigned char bytes[64];
	hackrf_edge_capabilities capabilities;
	int result;

	build_capabilities(bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	bytes[4] = 2;
	refresh_crc(bytes, 60);
	mock_reset();
	mock_add_input(0, 64, 64, bytes);
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);

	build_capabilities(bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	bytes[10] |= 0x80;
	refresh_crc(bytes, 60);
	mock_reset();
	mock_add_input(0, 64, 64, bytes);
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);

	build_capabilities(bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	bytes[60] ^= 1;
	mock_reset();
	mock_add_input(0, 64, 64, bytes);
	result = hackrf_edge_get_capabilities(mock_device(), &capabilities);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);
}

static void test_status(void)
{
	unsigned char bytes[128];
	hackrf_edge_status status;
	int result;

	build_status(bytes);
	mock_reset();
	mock_add_input(1, 128, 128, bytes);
	result = hackrf_edge_get_status(mock_device(), &status);
	CHECK(result == HACKRF_SUCCESS);
	CHECK(mock_complete());
	CHECK(status.active_profile == HACKRF_EDGE_PROFILE_FRAMED_C16_D4);
	CHECK(status.snapshot_sequence == 7);
	CHECK(status.input_sample_rate_hz == 20000000);
	CHECK(status.output_sample_rate_hz == 5000000);
	CHECK(status.input_tick == UINT64_C(0x0102030405060708));
	CHECK(status.event_loss_count == 25);

	build_status(bytes);
	bytes[55] = 1;
	refresh_crc(bytes, 124);
	mock_reset();
	mock_add_input(1, 128, 128, bytes);
	result = hackrf_edge_get_status(mock_device(), &status);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);

	build_status(bytes);
	bytes[22] = 2;
	refresh_crc(bytes, 124);
	mock_reset();
	mock_add_input(1, 128, 128, bytes);
	result = hackrf_edge_get_status(mock_device(), &status);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);

	mock_reset();
	mock_add_input(1, 128, HACKRF_EDGE_USB_ERROR_PIPE, NULL);
	result = hackrf_edge_get_status(mock_device(), &status);
	CHECK(result == HACKRF_ERROR_LIBUSB);
}

static void test_get_config(void)
{
	unsigned char bytes[32];
	hackrf_edge_config config;
	int result;

	build_config(
		bytes,
		HACKRF_EDGE_PROFILE_FRAMED_C8,
		HACKRF_EDGE_CONFIG_ONE_SHOT | HACKRF_EDGE_CONFIG_EMIT_EVENTS,
		10000000,
		1024,
		0);
	mock_reset();
	mock_add_input(2, 32, 32, bytes);
	result = hackrf_edge_get_config(mock_device(), &config);
	CHECK(result == HACKRF_SUCCESS);
	CHECK(config.profile == HACKRF_EDGE_PROFILE_FRAMED_C8);
	CHECK(config.input_sample_rate_hz == 10000000);
	CHECK(config.target_payload_bytes == 1024);

	build_config(
		bytes,
		HACKRF_EDGE_PROFILE_FRAMED_C8,
		HACKRF_EDGE_CONFIG_ONE_SHOT | HACKRF_EDGE_CONFIG_SCHEDULED,
		0,
		0,
		0);
	mock_reset();
	mock_add_input(2, 32, 32, bytes);
	result = hackrf_edge_get_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);

	build_config(
		bytes,
		HACKRF_EDGE_PROFILE_FRAMED_C8,
		HACKRF_EDGE_CONFIG_ONE_SHOT,
		0,
		0,
		0);
	bytes[28] ^= 0x01;
	mock_reset();
	mock_add_input(2, 32, 32, bytes);
	result = hackrf_edge_get_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_EDGE_MALFORMED);
}

static void test_set_config(void)
{
	unsigned char capabilities_bytes[64];
	unsigned char expected_config[32];
	hackrf_edge_config config;
	int result;

	build_capabilities(capabilities_bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	memset(&config, 0, sizeof(config));
	config.profile = HACKRF_EDGE_PROFILE_FRAMED_C16_D4;
	config.flags = HACKRF_EDGE_CONFIG_ONE_SHOT | HACKRF_EDGE_CONFIG_EMIT_EVENTS |
		HACKRF_EDGE_CONFIG_STRICT_RATE;
	config.input_sample_rate_hz = 10000000;
	config.target_payload_bytes = 1024;
	build_config(
		expected_config,
		config.profile,
		config.flags,
		config.input_sample_rate_hz,
		config.target_payload_bytes,
		config.apply_at_input_tick);

	mock_reset();
	mock_add_input(0, 64, 64, capabilities_bytes);
	mock_add_output(3, 32, 32, expected_config);
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_SUCCESS);
	CHECK(mock_complete());
	CHECK(get_u32(expected_config) == UINT32_C(0x51474445));
	CHECK(get_u64(expected_config + 20) == 0u);
	CHECK(get_u32(expected_config + 28) == test_crc32c(expected_config, 28));

	config.flags = 0;
	mock_reset();
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());

	config.flags = HACKRF_EDGE_CONFIG_ONE_SHOT | HACKRF_EDGE_CONFIG_EMIT_EVENTS;
	build_capabilities(
		capabilities_bytes,
		HACKRF_EDGE_CAP_CONTROL | HACKRF_EDGE_CAP_STATUS |
			HACKRF_EDGE_CAP_FRAMED_RX | HACKRF_EDGE_CAP_C16,
		UINT32_C(0x0f));
	mock_reset();
	mock_add_input(0, 64, 64, capabilities_bytes);
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());

	build_capabilities(capabilities_bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	build_config(
		expected_config,
		config.profile,
		config.flags,
		config.input_sample_rate_hz,
		config.target_payload_bytes,
		config.apply_at_input_tick);
	mock_reset();
	mock_add_input(0, 64, 64, capabilities_bytes);
	mock_add_output(3, 32, 31, expected_config);
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_LIBUSB);
}

static void test_set_config_validation(void)
{
	unsigned char capabilities_bytes[64];
	unsigned char expected_config[32];
	hackrf_edge_config config;
	int result;

	build_capabilities(capabilities_bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	memset(&config, 0, sizeof(config));
	config.profile = HACKRF_EDGE_PROFILE_FRAMED_C16_D4;
	config.flags = HACKRF_EDGE_CONFIG_ONE_SHOT;
	config.input_sample_rate_hz = 1000000;
	config.target_payload_bytes = 1024;
	mock_reset();
	mock_add_input(0, 64, 64, capabilities_bytes);
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());

	config.input_sample_rate_hz = 10000000;
	config.target_payload_bytes = 8192;
	mock_reset();
	mock_add_input(0, 64, 64, capabilities_bytes);
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());

	config.target_payload_bytes = 1023;
	mock_reset();
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());

	config.target_payload_bytes = 1024;
	build_capabilities(
		capabilities_bytes,
		HACKRF_EDGE_CAP_CONTROL | HACKRF_EDGE_CAP_STATUS |
			HACKRF_EDGE_CAP_FRAMED_RX,
		UINT32_C(0x03));
	mock_reset();
	mock_add_input(0, 64, 64, capabilities_bytes);
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());

	build_capabilities(capabilities_bytes, UINT32_C(0x7f), UINT32_C(0x0f));
	config.flags = HACKRF_EDGE_CONFIG_ONE_SHOT | HACKRF_EDGE_CONFIG_SCHEDULED;
	config.apply_at_input_tick = UINT64_C(0x1020304050607080);
	build_config(
		expected_config,
		config.profile,
		config.flags,
		config.input_sample_rate_hz,
		config.target_payload_bytes,
		config.apply_at_input_tick);
	mock_reset();
	mock_add_input(0, 64, 64, capabilities_bytes);
	mock_add_output(3, 32, 32, expected_config);
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_SUCCESS);
	CHECK(mock_complete());

	memset(&config, 0, sizeof(config));
	config.profile = HACKRF_EDGE_PROFILE_LEGACY_C8;
	config.flags = HACKRF_EDGE_CONFIG_RESET_COUNTERS;
	mock_reset();
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());

	config.flags = 0;
	config.input_sample_rate_hz = 8000000;
	mock_reset();
	result = hackrf_edge_set_config(mock_device(), &config);
	CHECK(result == HACKRF_ERROR_INVALID_PARAM);
	CHECK(mock_complete());
}

static void test_reset_and_parameters(void)
{
	unsigned char status_bytes[128];
	hackrf_edge_capabilities capabilities;
	hackrf_edge_config config;
	hackrf_edge_status status;
	int result;

	mock_reset();
	mock_add_output(4, 0, 0, NULL);
	result = hackrf_edge_reset(mock_device());
	CHECK(result == HACKRF_SUCCESS);
	CHECK(mock_complete());

	build_status(status_bytes);
	put_u32(status_bytes + 16, HACKRF_EDGE_ERROR_BUSY);
	refresh_crc(status_bytes, 124);
	mock_reset();
	mock_add_output(4, 0, HACKRF_EDGE_USB_ERROR_PIPE, NULL);
	mock_add_input(1, 128, 128, status_bytes);
	result = hackrf_edge_reset(mock_device());
	CHECK(result == HACKRF_ERROR_BUSY);
	CHECK(mock_complete());

	/* A non-stall transport failure must not consult a stale BUSY status latch. */
	mock_reset();
	mock_add_output(4, 0, -7, NULL);
	mock_add_input(1, 128, 128, status_bytes);
	result = hackrf_edge_reset(mock_device());
	CHECK(result == HACKRF_ERROR_LIBUSB);
	CHECK(mock_step_index == 1);

	CHECK(hackrf_edge_get_capabilities(NULL, &capabilities) ==
	      HACKRF_ERROR_INVALID_PARAM);
	CHECK(hackrf_edge_get_capabilities(mock_device(), NULL) ==
	      HACKRF_ERROR_INVALID_PARAM);
	CHECK(hackrf_edge_get_config(NULL, &config) == HACKRF_ERROR_INVALID_PARAM);
	CHECK(hackrf_edge_get_status(NULL, &status) == HACKRF_ERROR_INVALID_PARAM);
	CHECK(hackrf_edge_set_config(NULL, &config) == HACKRF_ERROR_INVALID_PARAM);
	CHECK(hackrf_edge_reset(NULL) == HACKRF_ERROR_INVALID_PARAM);
}

int main(void)
{
	test_crc_check_value();
	test_capabilities_valid();
	test_discovery_errors();
	test_capabilities_malformed();
	test_status();
	test_get_config();
	test_set_config();
	test_set_config_validation();
	test_reset_and_parameters();

	if (failures != 0) {
		fprintf(stderr, "%d HackRF Edge test failure(s)\n", failures);
		return 1;
	}
	printf("HackRF Edge offline protocol tests passed\n");
	return 0;
}
