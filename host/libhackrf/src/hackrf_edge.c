/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Host-side HackRF Edge v1 control protocol support.
 *
 * Wire data is deliberately handled as bytes; no packed wire structure is
 * dereferenced here.
 */

#include "hackrf.h"
#include "hackrf_edge_internal.h"

#include <string.h>

#define EDGE_ABI_WVALUE 0x0100u

#define EDGE_COMMAND_GET_CAPABILITIES 0u
#define EDGE_COMMAND_GET_STATUS       1u
#define EDGE_COMMAND_GET_CONFIG       2u
#define EDGE_COMMAND_SET_CONFIG       3u
#define EDGE_COMMAND_RESET            4u

#define EDGE_MAGIC_CAPABILITIES UINT32_C(0x43474445)
#define EDGE_MAGIC_CONFIG       UINT32_C(0x51474445)
#define EDGE_MAGIC_STATUS       UINT32_C(0x53474445)

#define EDGE_CAPABILITIES_BYTES 64u
#define EDGE_CONFIG_BYTES       32u
#define EDGE_STATUS_BYTES       128u
#define EDGE_FRAME_HEADER_BYTES 64u
#define EDGE_EVENT_BYTES        32u

#define EDGE_CAPABILITY_FLAGS_MASK UINT32_C(0x0000007f)
#define EDGE_CONFIG_FLAGS_MASK     UINT32_C(0x0000001f)
#define EDGE_STATUS_FLAGS_MASK     UINT32_C(0x000000ff)
#define EDGE_PROFILE_MASK          UINT32_C(0x0000000f)

static uint16_t edge_read_u16(const unsigned char* bytes)
{
	return (uint16_t) (((uint16_t) bytes[0]) | ((uint16_t) bytes[1] << 8));
}

static uint32_t edge_read_u32(const unsigned char* bytes)
{
	return ((uint32_t) bytes[0]) | ((uint32_t) bytes[1] << 8) |
		((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static uint64_t edge_read_u64(const unsigned char* bytes)
{
	return ((uint64_t) edge_read_u32(bytes)) |
		((uint64_t) edge_read_u32(bytes + 4) << 32);
}

static void edge_write_u32(unsigned char* bytes, uint32_t value)
{
	bytes[0] = (unsigned char) value;
	bytes[1] = (unsigned char) (value >> 8);
	bytes[2] = (unsigned char) (value >> 16);
	bytes[3] = (unsigned char) (value >> 24);
}

static void edge_write_u64(unsigned char* bytes, uint64_t value)
{
	edge_write_u32(bytes, (uint32_t) value);
	edge_write_u32(bytes + 4, (uint32_t) (value >> 32));
}

static uint32_t edge_crc32c(const unsigned char* bytes, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++) {
			if ((crc & 1u) != 0u) {
				crc = (crc >> 1) ^ UINT32_C(0x82f63b78);
			} else {
				crc >>= 1;
			}
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static int edge_read_control(
	hackrf_device* device,
	uint16_t command,
	unsigned char* bytes,
	uint16_t length,
	int discovery)
{
	int result;

	memset(bytes, 0, length);
	result = hackrf_edge_control_transfer(
		device,
		HACKRF_EDGE_CONTROL_IN,
		HACKRF_EDGE_VENDOR_REQUEST,
		EDGE_ABI_WVALUE,
		command,
		bytes,
		length,
		HACKRF_EDGE_CONTROL_TIMEOUT_MS);
	if (result < 0) {
		if (discovery && result == HACKRF_EDGE_USB_ERROR_PIPE) {
			return HACKRF_ERROR_EDGE_UNSUPPORTED;
		}
		return HACKRF_ERROR_LIBUSB;
	}
	if (result != (int) length) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}
	return HACKRF_SUCCESS;
}

static int edge_profile_shape(uint8_t profile, uint8_t* sample_bytes, uint16_t* decimation)
{
	switch (profile) {
	case HACKRF_EDGE_PROFILE_LEGACY_C8:
	case HACKRF_EDGE_PROFILE_FRAMED_C8:
		*sample_bytes = 2;
		*decimation = 1;
		return 1;
	case HACKRF_EDGE_PROFILE_FRAMED_C16_D4:
		*sample_bytes = 4;
		*decimation = 4;
		return 1;
	case HACKRF_EDGE_PROFILE_FRAMED_C16_D8:
		*sample_bytes = 4;
		*decimation = 8;
		return 1;
	default:
		return 0;
	}
}

static int edge_decode_capabilities(
	const unsigned char* bytes,
	hackrf_edge_capabilities* capabilities)
{
	hackrf_edge_capabilities decoded;
	uint32_t c16_profiles;
	uint32_t framed_profiles;

	if (edge_read_u32(bytes) != EDGE_MAGIC_CAPABILITIES ||
	    bytes[4] != HACKRF_EDGE_ABI_MAJOR || bytes[5] != HACKRF_EDGE_ABI_MINOR ||
	    bytes[6] != EDGE_CAPABILITIES_BYTES ||
	    bytes[7] != HACKRF_EDGE_VENDOR_REQUEST ||
	    edge_read_u32(bytes + 60) != edge_crc32c(bytes, 60)) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}

	memset(&decoded, 0, sizeof(decoded));
	decoded.abi_major = bytes[4];
	decoded.abi_minor = bytes[5];
	decoded.capability_flags = edge_read_u32(bytes + 8);
	decoded.profile_mask = edge_read_u32(bytes + 12);
	decoded.max_frame_bytes = edge_read_u32(bytes + 16);
	decoded.max_payload_bytes = edge_read_u32(bytes + 20);
	decoded.min_input_sample_rate_hz = edge_read_u32(bytes + 24);
	decoded.max_input_sample_rate_hz = edge_read_u32(bytes + 28);
	decoded.input_rate_granularity_hz = edge_read_u32(bytes + 32);
	decoded.frame_header_bytes = edge_read_u16(bytes + 36);
	decoded.config_bytes = edge_read_u16(bytes + 38);
	decoded.status_bytes = edge_read_u16(bytes + 40);
	decoded.event_bytes = edge_read_u16(bytes + 42);
	memcpy(decoded.build_id, bytes + 44, HACKRF_EDGE_BUILD_ID_BYTES);

	if ((decoded.capability_flags & ~EDGE_CAPABILITY_FLAGS_MASK) != 0u ||
	    (decoded.capability_flags & HACKRF_EDGE_CAP_CONTROL) == 0u ||
	    (decoded.profile_mask & ~EDGE_PROFILE_MASK) != 0u ||
	    (decoded.profile_mask & (UINT32_C(1) << HACKRF_EDGE_PROFILE_LEGACY_C8)) ==
		    0u ||
	    decoded.frame_header_bytes != EDGE_FRAME_HEADER_BYTES ||
	    decoded.config_bytes != EDGE_CONFIG_BYTES ||
	    decoded.status_bytes != EDGE_STATUS_BYTES ||
	    decoded.event_bytes != EDGE_EVENT_BYTES ||
	    (decoded.max_frame_bytes == 0u && decoded.max_payload_bytes != 0u) ||
	    (decoded.max_frame_bytes != 0u &&
	     (decoded.max_frame_bytes < EDGE_FRAME_HEADER_BYTES ||
	      decoded.max_payload_bytes >
		      decoded.max_frame_bytes - EDGE_FRAME_HEADER_BYTES)) ||
	    decoded.min_input_sample_rate_hz == 0u ||
	    decoded.min_input_sample_rate_hz > decoded.max_input_sample_rate_hz) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}

	framed_profiles =
		decoded.profile_mask & ~(UINT32_C(1) << HACKRF_EDGE_PROFILE_LEGACY_C8);
	c16_profiles = (UINT32_C(1) << HACKRF_EDGE_PROFILE_FRAMED_C16_D4) |
		(UINT32_C(1) << HACKRF_EDGE_PROFILE_FRAMED_C16_D8);
	if ((framed_profiles != 0u &&
	     ((decoded.capability_flags & HACKRF_EDGE_CAP_FRAMED_RX) == 0u ||
	      decoded.max_payload_bytes == 0u)) ||
	    ((decoded.profile_mask & c16_profiles) != 0u &&
	     (decoded.capability_flags & HACKRF_EDGE_CAP_C16) == 0u)) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}

	*capabilities = decoded;
	return HACKRF_SUCCESS;
}

static int edge_validate_config_basic(const hackrf_edge_config* config)
{
	uint8_t sample_bytes;
	uint16_t decimation;

	if (!edge_profile_shape(config->profile, &sample_bytes, &decimation) ||
	    (config->flags & ~EDGE_CONFIG_FLAGS_MASK) != 0u) {
		return 0;
	}
	if (config->profile != HACKRF_EDGE_PROFILE_LEGACY_C8 &&
	    (config->flags & HACKRF_EDGE_CONFIG_ONE_SHOT) == 0u) {
		return 0;
	}
	if (((config->flags & HACKRF_EDGE_CONFIG_SCHEDULED) == 0u &&
	     config->apply_at_input_tick != 0u) ||
	    ((config->flags & HACKRF_EDGE_CONFIG_SCHEDULED) != 0u &&
	     config->apply_at_input_tick == 0u)) {
		return 0;
	}
	if (config->profile == HACKRF_EDGE_PROFILE_LEGACY_C8 &&
	    (config->target_payload_bytes != 0u ||
	     (config->flags & HACKRF_EDGE_CONFIG_EMIT_EVENTS) != 0u)) {
		return 0;
	}
	if (config->target_payload_bytes != 0u &&
	    (config->target_payload_bytes % sample_bytes) != 0u) {
		return 0;
	}
	(void) decimation;
	return 1;
}

static int edge_decode_config(const unsigned char* bytes, hackrf_edge_config* config)
{
	hackrf_edge_config decoded;

	if (edge_read_u32(bytes) != EDGE_MAGIC_CONFIG ||
	    bytes[4] != HACKRF_EDGE_ABI_MAJOR || bytes[5] != HACKRF_EDGE_ABI_MINOR ||
	    bytes[6] != EDGE_CONFIG_BYTES ||
	    edge_read_u32(bytes + 28) != edge_crc32c(bytes, 28)) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}

	memset(&decoded, 0, sizeof(decoded));
	decoded.profile = bytes[7];
	decoded.flags = edge_read_u32(bytes + 8);
	decoded.input_sample_rate_hz = edge_read_u32(bytes + 12);
	decoded.target_payload_bytes = edge_read_u32(bytes + 16);
	decoded.apply_at_input_tick = edge_read_u64(bytes + 20);
	if (!edge_validate_config_basic(&decoded)) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}

	*config = decoded;
	return HACKRF_SUCCESS;
}

static int edge_decode_status(const unsigned char* bytes, hackrf_edge_status* status)
{
	hackrf_edge_status decoded;
	uint8_t expected_sample_bytes;
	uint16_t expected_decimation;

	if (edge_read_u32(bytes) != EDGE_MAGIC_STATUS ||
	    bytes[4] != HACKRF_EDGE_ABI_MAJOR || bytes[5] != HACKRF_EDGE_ABI_MINOR ||
	    bytes[6] != EDGE_STATUS_BYTES || bytes[55] != 0u ||
	    edge_read_u32(bytes + 124) != edge_crc32c(bytes, 124)) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}

	memset(&decoded, 0, sizeof(decoded));
	decoded.active_profile = bytes[7];
	decoded.snapshot_sequence = edge_read_u32(bytes + 8);
	decoded.flags = edge_read_u32(bytes + 12);
	decoded.last_error = edge_read_u32(bytes + 16);
	decoded.requested_profile = bytes[20];
	decoded.transceiver_mode = bytes[21];
	decoded.sample_bytes = bytes[22];
	decoded.rf_amp_enabled = bytes[23];
	decoded.stream_epoch = edge_read_u32(bytes + 24);
	decoded.config_epoch = edge_read_u32(bytes + 28);
	decoded.gain_epoch = edge_read_u32(bytes + 32);
	decoded.next_frame_sequence = edge_read_u32(bytes + 36);
	decoded.input_sample_rate_hz = edge_read_u32(bytes + 40);
	decoded.output_sample_rate_hz = edge_read_u32(bytes + 44);
	decoded.decimation = edge_read_u16(bytes + 48);
	decoded.group_delay_input_q8_8 = edge_read_u16(bytes + 50);
	decoded.lna_gain_db = bytes[52];
	decoded.vga_gain_db = bytes[53];
	decoded.txvga_gain_db = bytes[54];
	decoded.input_tick = edge_read_u64(bytes + 56);
	decoded.dropped_input_samples = edge_read_u64(bytes + 64);
	decoded.emitted_output_samples = edge_read_u64(bytes + 72);
	decoded.m0_count_bytes = edge_read_u64(bytes + 80);
	decoded.m4_count_bytes = edge_read_u64(bytes + 88);
	decoded.usb_completed_bytes = edge_read_u64(bytes + 96);
	decoded.shortfall_count = edge_read_u32(bytes + 104);
	decoded.longest_shortfall_bytes = edge_read_u32(bytes + 108);
	decoded.last_transition_input_tick = edge_read_u64(bytes + 112);
	decoded.event_loss_count = edge_read_u32(bytes + 120);

	if (!edge_profile_shape(
		    decoded.active_profile,
		    &expected_sample_bytes,
		    &expected_decimation) ||
	    decoded.requested_profile >= HACKRF_EDGE_PROFILE_COUNT ||
	    decoded.transceiver_mode > 5u || decoded.rf_amp_enabled > 1u ||
	    decoded.sample_bytes != expected_sample_bytes ||
	    decoded.decimation != expected_decimation ||
	    (decoded.flags & ~EDGE_STATUS_FLAGS_MASK) != 0u ||
	    decoded.last_error > HACKRF_EDGE_ERROR_INTERNAL) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}
	if ((decoded.input_sample_rate_hz == 0u && decoded.output_sample_rate_hz != 0u) ||
	    (decoded.input_sample_rate_hz != 0u &&
	     (decoded.input_sample_rate_hz % decoded.decimation != 0u ||
	      decoded.output_sample_rate_hz !=
		      decoded.input_sample_rate_hz / decoded.decimation))) {
		return HACKRF_ERROR_EDGE_MALFORMED;
	}

	*status = decoded;
	return HACKRF_SUCCESS;
}

int ADDCALL hackrf_edge_get_capabilities(
	hackrf_device* device,
	hackrf_edge_capabilities* capabilities)
{
	unsigned char bytes[EDGE_CAPABILITIES_BYTES];
	int result;

	if (device == NULL || capabilities == NULL) {
		return HACKRF_ERROR_INVALID_PARAM;
	}
	result = edge_read_control(
		device,
		EDGE_COMMAND_GET_CAPABILITIES,
		bytes,
		EDGE_CAPABILITIES_BYTES,
		1);
	if (result != HACKRF_SUCCESS) {
		return result;
	}
	return edge_decode_capabilities(bytes, capabilities);
}

int ADDCALL hackrf_edge_get_status(hackrf_device* device, hackrf_edge_status* status)
{
	unsigned char bytes[EDGE_STATUS_BYTES];
	int result;

	if (device == NULL || status == NULL) {
		return HACKRF_ERROR_INVALID_PARAM;
	}
	result = edge_read_control(
		device,
		EDGE_COMMAND_GET_STATUS,
		bytes,
		EDGE_STATUS_BYTES,
		0);
	if (result != HACKRF_SUCCESS) {
		return result;
	}
	return edge_decode_status(bytes, status);
}

int ADDCALL hackrf_edge_get_config(hackrf_device* device, hackrf_edge_config* config)
{
	unsigned char bytes[EDGE_CONFIG_BYTES];
	int result;

	if (device == NULL || config == NULL) {
		return HACKRF_ERROR_INVALID_PARAM;
	}
	result = edge_read_control(
		device,
		EDGE_COMMAND_GET_CONFIG,
		bytes,
		EDGE_CONFIG_BYTES,
		0);
	if (result != HACKRF_SUCCESS) {
		return result;
	}
	return edge_decode_config(bytes, config);
}

static int edge_validate_config_capabilities(
	const hackrf_edge_config* config,
	const hackrf_edge_capabilities* capabilities)
{
	uint32_t profile_bit;

	profile_bit = UINT32_C(1) << config->profile;
	if ((capabilities->profile_mask & profile_bit) == 0u) {
		return 0;
	}
	if (config->input_sample_rate_hz != 0u &&
	    (config->input_sample_rate_hz < capabilities->min_input_sample_rate_hz ||
	     config->input_sample_rate_hz > capabilities->max_input_sample_rate_hz ||
	     (capabilities->input_rate_granularity_hz != 0u &&
	      (config->input_sample_rate_hz - capabilities->min_input_sample_rate_hz) %
			      capabilities->input_rate_granularity_hz !=
		      0u))) {
		return 0;
	}
	if (config->target_payload_bytes > capabilities->max_payload_bytes) {
		return 0;
	}
	if ((config->flags & HACKRF_EDGE_CONFIG_EMIT_EVENTS) != 0u &&
	    (capabilities->capability_flags & HACKRF_EDGE_CAP_EMBEDDED_EVENTS) == 0u) {
		return 0;
	}
	if ((config->flags & HACKRF_EDGE_CONFIG_SCHEDULED) != 0u &&
	    (capabilities->capability_flags & HACKRF_EDGE_CAP_SCHEDULED_CONFIG) == 0u) {
		return 0;
	}
	return 1;
}

int ADDCALL hackrf_edge_set_config(hackrf_device* device, const hackrf_edge_config* config)
{
	hackrf_edge_capabilities capabilities;
	unsigned char bytes[EDGE_CONFIG_BYTES];
	int result;

	if (device == NULL || config == NULL || !edge_validate_config_basic(config)) {
		return HACKRF_ERROR_INVALID_PARAM;
	}

	result = hackrf_edge_get_capabilities(device, &capabilities);
	if (result != HACKRF_SUCCESS) {
		return result;
	}
	if (!edge_validate_config_capabilities(config, &capabilities)) {
		return HACKRF_ERROR_INVALID_PARAM;
	}

	memset(bytes, 0, sizeof(bytes));
	edge_write_u32(bytes, EDGE_MAGIC_CONFIG);
	bytes[4] = HACKRF_EDGE_ABI_MAJOR;
	bytes[5] = HACKRF_EDGE_ABI_MINOR;
	bytes[6] = EDGE_CONFIG_BYTES;
	bytes[7] = config->profile;
	edge_write_u32(bytes + 8, config->flags);
	edge_write_u32(bytes + 12, config->input_sample_rate_hz);
	edge_write_u32(bytes + 16, config->target_payload_bytes);
	edge_write_u64(bytes + 20, config->apply_at_input_tick);
	edge_write_u32(bytes + 28, edge_crc32c(bytes, 28));

	result = hackrf_edge_control_transfer(
		device,
		HACKRF_EDGE_CONTROL_OUT,
		HACKRF_EDGE_VENDOR_REQUEST,
		EDGE_ABI_WVALUE,
		EDGE_COMMAND_SET_CONFIG,
		bytes,
		EDGE_CONFIG_BYTES,
		HACKRF_EDGE_CONTROL_TIMEOUT_MS);
	if (result < 0 || result != EDGE_CONFIG_BYTES) {
		return HACKRF_ERROR_LIBUSB;
	}
	return HACKRF_SUCCESS;
}

int ADDCALL hackrf_edge_reset(hackrf_device* device)
{
	int result;

	if (device == NULL) {
		return HACKRF_ERROR_INVALID_PARAM;
	}
	result = hackrf_edge_control_transfer(
		device,
		HACKRF_EDGE_CONTROL_OUT,
		HACKRF_EDGE_VENDOR_REQUEST,
		EDGE_ABI_WVALUE,
		EDGE_COMMAND_RESET,
		NULL,
		0,
		HACKRF_EDGE_CONTROL_TIMEOUT_MS);
	if (result != 0) {
		return HACKRF_ERROR_LIBUSB;
	}
	return HACKRF_SUCCESS;
}
