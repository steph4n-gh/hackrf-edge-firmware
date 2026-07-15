/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Read and explicitly configure the experimental HackRF Edge v1 protocol.
 */

#include <hackrf.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char* program)
{
	fprintf(stderr,
		"usage: %s [--serial SERIAL] [--reset-edge | --set-profile PROFILE [options]]\n"
		"\n"
		"With no mutation option, only capabilities, status, and config are read.\n"
		"\n"
		"Mutation options (must be explicit):\n"
		"  --reset-edge                 reset Edge state to legacy C8\n"
		"  --set-profile PROFILE        legacy-c8, framed-c8, c16-d4, or c16-d8\n"
		"  --rate HZ                    requested raw-input rate (0 = current)\n"
		"  --payload BYTES              target payload bytes (0 = default)\n"
		"  --emit-events                request embedded event records\n"
		"  --reset-counters             reset stream-local counters on apply\n"
		"  --strict-rate                reject instead of selecting a nearby rate\n"
		"  --apply-at INPUT_TICK        schedule the configuration at a future tick\n"
		"\n"
		"Other options:\n"
		"  --serial SERIAL              open a specific HackRF\n"
		"  -h, --help                   show this help\n",
		program);
}

static int parse_u64(const char* text, uint64_t* value)
{
	char* end;
	uintmax_t parsed;

	errno = 0;
	end = NULL;
	if (text[0] == '-') {
		return 0;
	}
	parsed = strtoumax(text, &end, 0);
	if (errno != 0 || end == text || *end != '\0' || parsed > UINT64_MAX) {
		return 0;
	}
	*value = (uint64_t) parsed;
	return 1;
}

static int parse_u32(const char* text, uint32_t* value)
{
	uint64_t parsed;

	if (!parse_u64(text, &parsed) || parsed > UINT32_MAX) {
		return 0;
	}
	*value = (uint32_t) parsed;
	return 1;
}

static int parse_profile(const char* text, uint8_t* profile)
{
	if (strcmp(text, "legacy-c8") == 0) {
		*profile = HACKRF_EDGE_PROFILE_LEGACY_C8;
	} else if (strcmp(text, "framed-c8") == 0) {
		*profile = HACKRF_EDGE_PROFILE_FRAMED_C8;
	} else if (strcmp(text, "c16-d4") == 0) {
		*profile = HACKRF_EDGE_PROFILE_FRAMED_C16_D4;
	} else if (strcmp(text, "c16-d8") == 0) {
		*profile = HACKRF_EDGE_PROFILE_FRAMED_C16_D8;
	} else {
		return 0;
	}
	return 1;
}

static const char* profile_name(uint8_t profile)
{
	switch (profile) {
	case HACKRF_EDGE_PROFILE_LEGACY_C8:
		return "legacy-c8";
	case HACKRF_EDGE_PROFILE_FRAMED_C8:
		return "framed-c8";
	case HACKRF_EDGE_PROFILE_FRAMED_C16_D4:
		return "c16-d4";
	case HACKRF_EDGE_PROFILE_FRAMED_C16_D8:
		return "c16-d8";
	default:
		return "unknown";
	}
}

static void print_capabilities(const hackrf_edge_capabilities* capabilities)
{
	size_t i;

	printf("Edge ABI: %u.%u\n", capabilities->abi_major, capabilities->abi_minor);
	printf("Capability flags: 0x%08" PRIx32 "\n", capabilities->capability_flags);
	printf("Profile mask: 0x%08" PRIx32 "\n", capabilities->profile_mask);
	printf("Frame/payload limit: %" PRIu32 "/%" PRIu32 " bytes\n",
	       capabilities->max_frame_bytes,
	       capabilities->max_payload_bytes);
	printf("Input rate: %" PRIu32 "..%" PRIu32 " Hz (granularity %" PRIu32 ")\n",
	       capabilities->min_input_sample_rate_hz,
	       capabilities->max_input_sample_rate_hz,
	       capabilities->input_rate_granularity_hz);
	printf("Wire sizes header/config/status/event: %u/%u/%u/%u bytes\n",
	       capabilities->frame_header_bytes,
	       capabilities->config_bytes,
	       capabilities->status_bytes,
	       capabilities->event_bytes);
	printf("Build ID: ");
	for (i = 0; i < HACKRF_EDGE_BUILD_ID_BYTES; i++) {
		printf("%02x", capabilities->build_id[i]);
	}
	printf("\n");
}

static void print_config(const hackrf_edge_config* config)
{
	printf("Requested profile: %s (%u)\n",
	       profile_name(config->profile),
	       config->profile);
	printf("Config flags: 0x%08" PRIx32 "\n", config->flags);
	printf("Requested input rate: %" PRIu32 " Hz\n", config->input_sample_rate_hz);
	printf("Target payload: %" PRIu32 " bytes\n", config->target_payload_bytes);
	printf("Apply-at input tick: %" PRIu64 "\n", config->apply_at_input_tick);
}

static void print_status(const hackrf_edge_status* status)
{
	printf("Active profile: %s (%u)\n",
	       profile_name(status->active_profile),
	       status->active_profile);
	printf("Requested profile: %s (%u)\n",
	       profile_name(status->requested_profile),
	       status->requested_profile);
	printf("Snapshot sequence: %" PRIu32 "\n", status->snapshot_sequence);
	printf("Status flags: 0x%08" PRIx32 "\n", status->flags);
	printf("Last Edge error: %" PRIu32 "\n", status->last_error);
	printf("Transceiver/sample bytes/RF amp: %u/%u/%u\n",
	       status->transceiver_mode,
	       status->sample_bytes,
	       status->rf_amp_enabled);
	printf("Epochs stream/config/gain: %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
	       status->stream_epoch,
	       status->config_epoch,
	       status->gain_epoch);
	printf("Next frame sequence: %" PRIu32 "\n", status->next_frame_sequence);
	printf("Rates input/output: %" PRIu32 "/%" PRIu32 " Hz; decimation %u\n",
	       status->input_sample_rate_hz,
	       status->output_sample_rate_hz,
	       status->decimation);
	printf("Group delay Q8.8: %u\n", status->group_delay_input_q8_8);
	printf("Gains LNA/VGA/TX VGA: %u/%u/%u dB\n",
	       status->lna_gain_db,
	       status->vga_gain_db,
	       status->txvga_gain_db);
	printf("Input tick: %" PRIu64 "\n", status->input_tick);
	printf("Dropped input samples: %" PRIu64 "\n", status->dropped_input_samples);
	printf("Emitted output samples: %" PRIu64 "\n", status->emitted_output_samples);
	printf("Byte counters M0/M4/USB: %" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
	       status->m0_count_bytes,
	       status->m4_count_bytes,
	       status->usb_completed_bytes);
	printf("Shortfalls count/longest bytes: %" PRIu32 "/%" PRIu32 "\n",
	       status->shortfall_count,
	       status->longest_shortfall_bytes);
	printf("Last transition input tick: %" PRIu64 "\n",
	       status->last_transition_input_tick);
	printf("Lost events: %" PRIu32 "\n", status->event_loss_count);
}

int main(int argc, char** argv)
{
	const char* serial = NULL;
	hackrf_device* device = NULL;
	hackrf_edge_capabilities capabilities;
	hackrf_edge_config config;
	hackrf_edge_status status;
	int reset_edge = 0;
	int set_config = 0;
	int config_modifier = 0;
	int initialized = 0;
	int opened = 0;
	int result;
	int exit_code = EXIT_FAILURE;
	int i;

	memset(&config, 0, sizeof(config));
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		} else if (strcmp(argv[i], "--serial") == 0) {
			if (++i >= argc) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			serial = argv[i];
		} else if (strcmp(argv[i], "--reset-edge") == 0) {
			reset_edge = 1;
		} else if (strcmp(argv[i], "--set-profile") == 0) {
			if (++i >= argc || !parse_profile(argv[i], &config.profile)) {
				fprintf(stderr, "invalid or missing Edge profile\n");
				return EXIT_FAILURE;
			}
			set_config = 1;
		} else if (strcmp(argv[i], "--rate") == 0) {
			if (++i >= argc ||
			    !parse_u32(argv[i], &config.input_sample_rate_hz)) {
				fprintf(stderr, "invalid input rate\n");
				return EXIT_FAILURE;
			}
			config_modifier = 1;
		} else if (strcmp(argv[i], "--payload") == 0) {
			if (++i >= argc ||
			    !parse_u32(argv[i], &config.target_payload_bytes)) {
				fprintf(stderr, "invalid payload size\n");
				return EXIT_FAILURE;
			}
			config_modifier = 1;
		} else if (strcmp(argv[i], "--emit-events") == 0) {
			config.flags |= HACKRF_EDGE_CONFIG_EMIT_EVENTS;
			config_modifier = 1;
		} else if (strcmp(argv[i], "--reset-counters") == 0) {
			config.flags |= HACKRF_EDGE_CONFIG_RESET_COUNTERS;
			config_modifier = 1;
		} else if (strcmp(argv[i], "--strict-rate") == 0) {
			config.flags |= HACKRF_EDGE_CONFIG_STRICT_RATE;
			config_modifier = 1;
		} else if (strcmp(argv[i], "--apply-at") == 0) {
			if (++i >= argc ||
			    !parse_u64(argv[i], &config.apply_at_input_tick) ||
			    config.apply_at_input_tick == 0u) {
				fprintf(stderr, "invalid apply-at tick\n");
				return EXIT_FAILURE;
			}
			config.flags |= HACKRF_EDGE_CONFIG_SCHEDULED;
			config_modifier = 1;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if ((reset_edge && set_config) || (config_modifier && !set_config)) {
		fprintf(stderr,
			"configuration flags require --set-profile, which cannot be combined with --reset-edge\n");
		return EXIT_FAILURE;
	}
	if (set_config && config.profile != HACKRF_EDGE_PROFILE_LEGACY_C8) {
		config.flags |= HACKRF_EDGE_CONFIG_ONE_SHOT;
	}

	result = hackrf_init();
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"hackrf_init failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		goto done;
	}
	initialized = 1;
	result = serial == NULL ? hackrf_open(&device) :
				  hackrf_open_by_serial(serial, &device);
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"HackRF open failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		goto done;
	}
	opened = 1;

	result = hackrf_edge_get_capabilities(device, &capabilities);
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"Edge capability discovery failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		goto done;
	}
	print_capabilities(&capabilities);

	if (reset_edge) {
		result = hackrf_edge_reset(device);
		if (result != HACKRF_SUCCESS) {
			fprintf(stderr,
				"Edge reset failed: %s (%d)\n",
				hackrf_error_name(result),
				result);
			goto done;
		}
		printf("Edge state reset to legacy C8.\n");
	} else if (set_config) {
		result = hackrf_edge_set_config(device, &config);
		if (result != HACKRF_SUCCESS) {
			fprintf(stderr,
				"Edge SET_CONFIG failed: %s (%d)\n",
				hackrf_error_name(result),
				result);
			goto done;
		}
		printf("Edge configuration accepted.\n");
	}

	result = hackrf_edge_get_status(device, &status);
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"Edge GET_STATUS failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		goto done;
	}
	print_status(&status);

	result = hackrf_edge_get_config(device, &config);
	if (result != HACKRF_SUCCESS) {
		fprintf(stderr,
			"Edge GET_CONFIG failed: %s (%d)\n",
			hackrf_error_name(result),
			result);
		goto done;
	}
	print_config(&config);
	exit_code = EXIT_SUCCESS;

done:
	if (opened) {
		result = hackrf_close(device);
		if (result != HACKRF_SUCCESS && exit_code == EXIT_SUCCESS) {
			fprintf(stderr,
				"hackrf_close failed: %s (%d)\n",
				hackrf_error_name(result),
				result);
			exit_code = EXIT_FAILURE;
		}
	}
	if (initialized) {
		result = hackrf_exit();
		if (result != HACKRF_SUCCESS && exit_code == EXIT_SUCCESS) {
			fprintf(stderr,
				"hackrf_exit failed: %s (%d)\n",
				hackrf_error_name(result),
				result);
			exit_code = EXIT_FAILURE;
		}
	}
	return exit_code;
}
