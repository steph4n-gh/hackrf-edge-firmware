/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal firmware-info contract for the offline Edge firmware tests. */

#pragma once

#include <stdint.h>

struct firmware_info_t {
	char magic[8];
	uint16_t struct_version;
	uint16_t dfu_mode;
	uint32_t supported_platform;
	char version_string[32];
};

extern const struct firmware_info_t firmware_info;
