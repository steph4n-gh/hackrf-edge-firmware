/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdint.h>

struct m0_state {
	uint32_t requested_mode;
	uint32_t active_mode;
	uint32_t m0_count;
	uint32_t m4_count;
	uint32_t num_shortfalls;
	uint32_t longest_shortfall;
	uint32_t shortfall_limit;
	uint32_t threshold;
	uint32_t next_mode;
	uint32_t error;
	uint32_t total_shortfall;
	uint32_t current_shortfall;
	uint32_t total_shortfall_high;
	uint32_t current_shortfall_high;
};

enum m0_error {
	M0_ERROR_NONE = 0,
	M0_ERROR_RX_TIMEOUT = 1,
	M0_ERROR_TX_TIMEOUT = 2,
	M0_ERROR_MISSED_DEADLINE = 3,
};

static inline uint64_t m0_confirmed_shortfall_bytes(
	const volatile struct m0_state* state)
{
	uint32_t high_before;
	uint32_t high_after;
	uint32_t low;
	uint64_t total;
	uint64_t current;
	do {
		high_before = state->total_shortfall_high;
		low = state->total_shortfall;
		high_after = state->total_shortfall_high;
	} while (high_before != high_after);
	total = ((uint64_t) high_after << 32) | low;
	do {
		high_before = state->current_shortfall_high;
		low = state->current_shortfall;
		high_after = state->current_shortfall_high;
	} while (high_before != high_after);
	current = ((uint64_t) high_after << 32) | low;
	return total - current;
}

extern volatile struct m0_state m0_state;
