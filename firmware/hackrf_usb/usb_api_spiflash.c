/*
 * Copyright 2012-2022 Great Scott Gadgets <info@greatscottgadgets.com>
 * Copyright 2012 Jared Boone
 * Copyright 2013 Benjamin Vernoux
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

#include "usb_api_spiflash.h"

#include <stddef.h>
#include <stdint.h>

#include <usb_queue.h>
#include <usb_request.h>
#include <usb_type.h>
#include <w25q80bv.h>

/* Buffer size == spi_flash.page_len */
uint8_t spiflash_buffer[256U];

static uint32_t spiflash_request_address(const usb_endpoint_t* const endpoint)
{
	return ((uint32_t) endpoint->setup.value << 16) |
	       (uint32_t) endpoint->setup.index;
}

static int spiflash_request_is_valid(uint32_t addr, uint16_t len)
{
	const size_t address = (size_t) addr;
	const size_t length = (size_t) len;
	return (length != 0U) && (length <= spi_flash.page_len) &&
	       (address <= spi_flash.num_bytes) &&
	       (length <= (spi_flash.num_bytes - address));
}

usb_request_status_t usb_vendor_request_erase_spiflash(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		w25q80bv_setup(&spi_flash);
		/* only chip erase is implemented */
		w25q80bv_chip_erase(&spi_flash);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_write_spiflash(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	uint32_t addr = 0;
	uint16_t len = 0;

	if (stage == USB_TRANSFER_STAGE_SETUP) {
		addr = spiflash_request_address(endpoint);
		len = endpoint->setup.length;
		if (!spiflash_request_is_valid(addr, len)) {
			return USB_REQUEST_STATUS_STALL;
		} else {
			usb_transfer_schedule_block(
				endpoint->out,
				&spiflash_buffer[0],
				len,
				NULL,
				NULL);
			w25q80bv_setup(&spi_flash);
			return USB_REQUEST_STATUS_OK;
		}
	} else if (stage == USB_TRANSFER_STAGE_DATA) {
		addr = spiflash_request_address(endpoint);
		len = endpoint->setup.length;
		/* This check is redundant but makes me feel better. */
		if (!spiflash_request_is_valid(addr, len)) {
			return USB_REQUEST_STATUS_STALL;
		} else {
			w25q80bv_program(&spi_flash, addr, len, &spiflash_buffer[0]);
			usb_transfer_schedule_ack(endpoint->in);
			return USB_REQUEST_STATUS_OK;
		}
	} else {
		return USB_REQUEST_STATUS_OK;
	}
}

usb_request_status_t usb_vendor_request_read_spiflash(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	uint32_t addr;
	uint16_t len;

	if (stage == USB_TRANSFER_STAGE_SETUP) {
		addr = spiflash_request_address(endpoint);
		len = endpoint->setup.length;
		if (!spiflash_request_is_valid(addr, len)) {
			return USB_REQUEST_STATUS_STALL;
		} else {
			w25q80bv_read(&spi_flash, addr, len, &spiflash_buffer[0]);
			usb_transfer_schedule_block(
				endpoint->in,
				&spiflash_buffer[0],
				len,
				NULL,
				NULL);
			usb_transfer_schedule_ack(endpoint->out);
			return USB_REQUEST_STATUS_OK;
		}
	} else if (stage == USB_TRANSFER_STAGE_DATA) {
		addr = spiflash_request_address(endpoint);
		len = endpoint->setup.length;
		/* This check is redundant but makes me feel better. */
		if (!spiflash_request_is_valid(addr, len)) {
			return USB_REQUEST_STATUS_STALL;
		} else {
			return USB_REQUEST_STATUS_OK;
		}
	} else {
		return USB_REQUEST_STATUS_OK;
	}
}

usb_request_status_t usb_vendor_request_spiflash_status(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		w25q80bv_get_full_status(&spi_flash, endpoint->buffer);
		usb_transfer_schedule_block(
			endpoint->in,
			&endpoint->buffer,
			2,
			NULL,
			NULL);
		usb_transfer_schedule_ack(endpoint->out);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_spiflash_clear_status(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		w25q80bv_clear_status(&spi_flash);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}
