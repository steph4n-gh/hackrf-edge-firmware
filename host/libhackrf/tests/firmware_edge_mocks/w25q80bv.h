/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
	size_t page_len;
	size_t num_bytes;
} w25q80bv_driver_t;

extern w25q80bv_driver_t spi_flash;

void w25q80bv_setup(w25q80bv_driver_t* drv);
void w25q80bv_get_full_status(w25q80bv_driver_t* drv, uint8_t* data);
void w25q80bv_chip_erase(w25q80bv_driver_t* drv);
void w25q80bv_program(
	w25q80bv_driver_t* drv,
	uint32_t addr,
	uint32_t len,
	uint8_t* data);
void w25q80bv_read(
	w25q80bv_driver_t* drv,
	uint32_t addr,
	uint32_t len,
	uint8_t* data);
void w25q80bv_clear_status(w25q80bv_driver_t* drv);
