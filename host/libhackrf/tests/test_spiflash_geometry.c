/*
 * SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hackrf_spiflash_geometry.h"

#include <stdint.h>

#define REQUIRE(condition) \
	do { \
		if (!(condition)) { \
			return __LINE__; \
		} \
	} while (0)

int main(void)
{
	REQUIRE(
		hackrf_spiflash_default_length(BOARD_ID_HACKRF1_OG) ==
		HACKRF_SPIFLASH_STANDARD_LENGTH);
	REQUIRE(
		hackrf_spiflash_default_length(BOARD_ID_HACKRF1_R9) ==
		HACKRF_SPIFLASH_STANDARD_LENGTH);
	REQUIRE(
		hackrf_spiflash_default_length(BOARD_ID_JAWBREAKER) ==
		HACKRF_SPIFLASH_STANDARD_LENGTH);
	REQUIRE(
		hackrf_spiflash_default_length(BOARD_ID_RAD1O) ==
		HACKRF_SPIFLASH_STANDARD_LENGTH);
	REQUIRE(
		hackrf_spiflash_default_length(BOARD_ID_UNRECOGNIZED) ==
		HACKRF_SPIFLASH_STANDARD_LENGTH);
	REQUIRE(
		hackrf_spiflash_default_length(BOARD_ID_PRALINE) ==
		HACKRF_SPIFLASH_PRALINE_LENGTH);

	REQUIRE(hackrf_spiflash_range_is_valid(0U, 0x100000U, 0x100000U));
	REQUIRE(hackrf_spiflash_range_is_valid(0x0fff00U, 0x100U, 0x100000U));
	REQUIRE(!hackrf_spiflash_range_is_valid(0U, 0U, 0x100000U));
	REQUIRE(!hackrf_spiflash_range_is_valid(0x100000U, 1U, 0x100000U));
	REQUIRE(!hackrf_spiflash_range_is_valid(1U, 0x100000U, 0x100000U));
	REQUIRE(!hackrf_spiflash_range_is_valid(
		UINT32_MAX - 15U,
		32U,
		0x100000U));

	return 0;
}
