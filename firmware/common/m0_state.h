/*
 * Copyright 2025 Great Scott Gadgets <info@greatscottgadgets.com>
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

#pragma once

#include <stdint.h>

#define M0_REQUEST_FLAG (1 << 16)

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
	/* Exact cumulative bytes skipped by SGPIO shortfalls in this activation.
	 * The low/high members are intentionally interleaved with current loss;
	 * do not replace them with native uint64_t fields or change their order. */
	uint32_t total_shortfall;
	/* Bytes in the currently open shortfall; zero once loss is confirmed. */
	uint32_t current_shortfall;
	uint32_t total_shortfall_high;
	uint32_t current_shortfall_high;
};

/* The M0 publishes current before total while extending loss, then clears
 * current before publishing the first recovered sample. At a low-word wrap,
 * current publishes high then low while total publishes low then high. A
 * high/low/high stable read can therefore be exact or transiently behind, but
 * cannot fabricate forward confirmed loss. Callers must reject backward
 * modular snapshots. LPC43xx ordered AHB SRAM visibility is part of this
 * cross-core publication contract. */
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

enum m0_mode {
	M0_MODE_IDLE = 0,
	M0_MODE_WAIT = 1,
	M0_MODE_RX = 2,
	M0_MODE_TX_START = 3,
	M0_MODE_TX_RUN = 4,
};

enum m0_error {
	M0_ERROR_NONE = 0,
	M0_ERROR_RX_TIMEOUT = 1,
	M0_ERROR_TX_TIMEOUT = 2,
	M0_ERROR_MISSED_DEADLINE = 3,
};

/* Address of m0_state is set in ldscripts. If you change the name of this
 * variable, it won't be where it needs to be in the processor's address space,
 * unless you also adjust the ldscripts.
 */
extern volatile struct m0_state m0_state;

void m0_set_mode(enum m0_mode mode);
