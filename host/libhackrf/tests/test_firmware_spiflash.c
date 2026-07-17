/* SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_api_spiflash.h"

#include <usb_queue.h>
#include <w25q80bv.h>

#define CHECK(condition)                                                        \
	do {                                                                      \
		if (!(condition)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,       \
				__LINE__, #condition);                                  \
			exit(1);                                                      \
		}                                                                 \
	} while (0)

w25q80bv_driver_t spi_flash;

static usb_endpoint_t endpoint_in;
static usb_endpoint_t endpoint_out;
static usb_endpoint_t endpoint_control;
static const usb_endpoint_t* scheduled_endpoint;
static uint32_t scheduled_length;
static unsigned int ack_count;
static unsigned int read_count;
static unsigned int program_count;
static uint32_t last_address;
static uint32_t last_length;

int usb_transfer_schedule_block(
	const usb_endpoint_t* endpoint,
	void* data,
	uint32_t maximum_length,
	transfer_completion_cb completion_cb,
	void* user_data)
{
	(void) data;
	(void) completion_cb;
	(void) user_data;
	scheduled_endpoint = endpoint;
	scheduled_length = maximum_length;
	return 0;
}

int usb_transfer_schedule_ack(const usb_endpoint_t* endpoint)
{
	(void) endpoint;
	ack_count++;
	return 0;
}

unsigned int usb_transfer_completed_length(const usb_endpoint_t* endpoint)
{
	(void) endpoint;
	return 0;
}

void w25q80bv_setup(w25q80bv_driver_t* drv)
{
	(void) drv;
}

void w25q80bv_get_full_status(w25q80bv_driver_t* drv, uint8_t* data)
{
	(void) drv;
	data[0] = 0;
	data[1] = 0;
}

void w25q80bv_chip_erase(w25q80bv_driver_t* drv)
{
	(void) drv;
}

void w25q80bv_program(
	w25q80bv_driver_t* drv,
	uint32_t addr,
	uint32_t len,
	uint8_t* data)
{
	(void) drv;
	(void) data;
	program_count++;
	last_address = addr;
	last_length = len;
}

void w25q80bv_read(
	w25q80bv_driver_t* drv,
	uint32_t addr,
	uint32_t len,
	uint8_t* data)
{
	(void) drv;
	memset(data, 0xa5, len);
	read_count++;
	last_address = addr;
	last_length = len;
}

void w25q80bv_clear_status(w25q80bv_driver_t* drv)
{
	(void) drv;
}

static void reset_fixture(uint32_t address, uint16_t length)
{
	memset(&endpoint_in, 0, sizeof(endpoint_in));
	memset(&endpoint_out, 0, sizeof(endpoint_out));
	memset(&endpoint_control, 0, sizeof(endpoint_control));
	endpoint_control.in = &endpoint_in;
	endpoint_control.out = &endpoint_out;
	endpoint_control.setup.value = (uint16_t) (address >> 16);
	endpoint_control.setup.index = (uint16_t) address;
	endpoint_control.setup.length = length;
	spi_flash.page_len = 256U;
	spi_flash.num_bytes = 1048576U;
	scheduled_endpoint = NULL;
	scheduled_length = 0;
	ack_count = 0;
	read_count = 0;
	program_count = 0;
	last_address = 0;
	last_length = 0;
}

int main(void)
{
	reset_fixture(0x000fff00U, 256U);
	CHECK(usb_vendor_request_read_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_OK);
	CHECK(read_count == 1U);
	CHECK(last_address == 0x000fff00U && last_length == 256U);
	CHECK(scheduled_endpoint == &endpoint_in && scheduled_length == 256U);
	CHECK(ack_count == 1U);

	reset_fixture(0x00100000U, 1U);
	CHECK(usb_vendor_request_read_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(read_count == 0U && scheduled_endpoint == NULL && ack_count == 0U);

	reset_fixture(0x000fff01U, 256U);
	CHECK(usb_vendor_request_read_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(read_count == 0U);

	reset_fixture(0xfffffff0U, 32U);
	CHECK(usb_vendor_request_read_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(read_count == 0U);

	reset_fixture(0U, 0U);
	CHECK(usb_vendor_request_read_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(read_count == 0U);

	reset_fixture(0U, 257U);
	CHECK(usb_vendor_request_write_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_STALL);
	CHECK(program_count == 0U && scheduled_endpoint == NULL);

	reset_fixture(0x000fff00U, 256U);
	CHECK(usb_vendor_request_write_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_SETUP) ==
	      USB_REQUEST_STATUS_OK);
	CHECK(scheduled_endpoint == &endpoint_out && scheduled_length == 256U);
	CHECK(usb_vendor_request_write_spiflash(
		      &endpoint_control, USB_TRANSFER_STAGE_DATA) ==
	      USB_REQUEST_STATUS_OK);
	CHECK(program_count == 1U);
	CHECK(last_address == 0x000fff00U && last_length == 256U);

	return 0;
}
