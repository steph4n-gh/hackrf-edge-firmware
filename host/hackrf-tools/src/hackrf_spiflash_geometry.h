/*
 * SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HACKRF_SPIFLASH_GEOMETRY_H
#define HACKRF_SPIFLASH_GEOMETRY_H

#include <hackrf.h>

#include <stdint.h>

#define HACKRF_SPIFLASH_STANDARD_LENGTH 0x100000U
#define HACKRF_SPIFLASH_PRALINE_LENGTH  0x400000U
#define HACKRF_SPIFLASH_MAX_LENGTH      HACKRF_SPIFLASH_PRALINE_LENGTH

/*
 * A pre-r9 HackRF One board ID does not identify its SPI flash capacity.
 * Default ambiguous and unknown hardware to the original 1 MiB geometry.
 * Larger non-Praline flashes remain unsupported until a versioned capacity
 * query can distinguish them from standard 1 MiB units.
 */
static uint32_t hackrf_spiflash_default_length(uint8_t board_id)
{
	if (board_id == BOARD_ID_PRALINE) {
		return HACKRF_SPIFLASH_PRALINE_LENGTH;
	}
	return HACKRF_SPIFLASH_STANDARD_LENGTH;
}

/* Subtraction keeps the range check safe when address + length would wrap. */
static int hackrf_spiflash_range_is_valid(
	uint32_t address,
	uint32_t length,
	uint32_t flash_length)
{
	return (length != 0U) && (address <= flash_length) &&
	       (length <= (flash_length - address));
}

#endif
