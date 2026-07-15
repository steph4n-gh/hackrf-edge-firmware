/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdint.h>

typedef struct {
	uint32_t unused;
} radio_t;

typedef uint8_t radio_register_bank_t;
typedef uint8_t radio_register_t;

enum {
	RADIO_BANK_APPLIED = 0,
	RADIO_BANK_ACTIVE = 1,
	RADIO_OPMODE = 0,
	RADIO_SAMPLE_RATE = 1,
	RADIO_GAIN_RX_RF = 2,
	RADIO_GAIN_RX_IF = 3,
	RADIO_GAIN_RX_BB = 4,
	RADIO_GAIN_TX_IF = 5,
};

#define RADIO_UNSET UINT64_MAX
#define RADIO_REG_GROUP_FREQ (UINT32_C(1) << 0)
#define RADIO_REG_GROUP_RATE (UINT32_C(1) << 1)
#define RADIO_REG_GROUP_GAIN (UINT32_C(1) << 2)

extern radio_t radio;

uint64_t radio_reg_read(
	radio_t* radio,
	radio_register_bank_t bank,
	radio_register_t reg);
