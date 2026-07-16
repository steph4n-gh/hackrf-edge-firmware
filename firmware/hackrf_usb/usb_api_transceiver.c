/*
 * Copyright 2012-2022 Great Scott Gadgets <info@greatscottgadgets.com>
 * Copyright 2012 Jared Boone
 * Copyright 2013 Benjamin Vernoux
 * Copyright 2024 Bernd Herzog
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

#include "usb_api_transceiver.h"

#include <stdbool.h>
#include <stddef.h>

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/lpc43xx/gpdma.h>
#include <libopencm3/lpc43xx/usb.h>

#include <clock_gen.h>
#include <fixed_point.h>
#include <gpdma.h>
#include <hackrf_ui.h>
#include <leds.h>
#include <m0_state.h>
#include <operacake_sctimer.h>
#include <platform_detect.h>
#include <radio.h>
#include <sgpio.h>
#include <streaming.h>
#include <transceiver_mode.h>
#include <usb.h>
#include <usb_queue.h>
#include <usb_request.h>
#include <usb_type.h>

#include "usb_buffer.h"
#include "usb_endpoint.h"
#include "usb_api_edge.h"
#include "edge_transport.h"

#define USB_TRANSFER_SIZE 0x4000
#define DMA_TRANSFER_SIZE 0x2000
#define DMA_MAX_TRANSFER_SIZE 0x3ffc

#define BUF_HALF_MASK (USB_SAMP_BUFFER_SIZE >> 1)

// Unless we know the host knows our buffer size, we'll avoid leaving TX
// until we've transmitted all bytes sent by the host. This flag is cleared
// when the host requests our buffer size.
bool auto_tx_flush = true;

volatile uint32_t dma_started, dma_pending, usb_started, usb_completed;

static edge_transport_state_t edge_transport =
	EDGE_TRANSPORT_STATE_INITIALIZER;
static uint32_t edge_dma_source_offset;
static uint32_t edge_dma_destination_offset;
static uint32_t edge_dma_remaining;
static uint64_t edge_previous_total_shortfall;
static uint32_t edge_previous_shortfall_count;
static bool edge_quarantine_pending;
static uint32_t edge_activation_epoch;

static void edge_clear_dma_progress(void)
{
	edge_dma_source_offset = 0;
	edge_dma_destination_offset = 0;
	edge_dma_remaining = 0;
}

static void edge_reset_dma_ownership(uint32_t activation_epoch)
{
	edge_transport_reset(&edge_transport, activation_epoch);
	edge_clear_dma_progress();
}

static void edge_abandon_dma_ownership(void)
{
	edge_transport_cancel_dma(&edge_transport);
	edge_clear_dma_progress();
}

typedef struct {
	uint32_t freq_mhz;
	uint32_t freq_hz;
} set_freq_params_t;

set_freq_params_t set_freq_params;

struct set_freq_explicit_params {
	uint64_t if_freq_hz; /* intermediate frequency */
	uint64_t lo_freq_hz; /* front-end local oscillator frequency */
	uint8_t path;        /* image rejection filter path */
};

struct set_freq_explicit_params explicit_params;

typedef struct {
	uint32_t freq_hz;
	uint32_t divider;
} set_sample_r_params_t;

set_sample_r_params_t set_sample_r_params;

void transceiver_dma_setup(void);

usb_request_status_t usb_vendor_request_set_baseband_filter_bandwidth(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		const uint32_t bandwidth =
			(endpoint->setup.index << 16) | endpoint->setup.value;
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_BB_BANDWIDTH_TX,
			bandwidth);
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_BB_BANDWIDTH_RX,
			bandwidth);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_freq(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		usb_transfer_schedule_block(
			endpoint->out,
			&set_freq_params,
			sizeof(set_freq_params_t),
			NULL,
			NULL);
	} else if (stage == USB_TRANSFER_STAGE_DATA) {
		const uint64_t freq =
			set_freq_params.freq_mhz * 1000000ULL + set_freq_params.freq_hz;
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_FREQUENCY_RF,
			freq * FP_ONE_HZ);
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_FREQUENCY_IF,
			RADIO_UNSET);
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_FREQUENCY_LO,
			RADIO_UNSET);
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_IMAGE_REJECT,
			RADIO_UNSET);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_freq_explicit(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		usb_transfer_schedule_block(
			endpoint->out,
			&explicit_params,
			sizeof(struct set_freq_explicit_params),
			NULL,
			NULL);
	} else if (stage == USB_TRANSFER_STAGE_DATA) {
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_FREQUENCY_IF,
			explicit_params.if_freq_hz * FP_ONE_HZ);
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_FREQUENCY_LO,
			explicit_params.lo_freq_hz * FP_ONE_HZ);
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_IMAGE_REJECT,
			explicit_params.path);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

/*
 * Convert fractional sample rate to units of 1/(2**36) Hz.
 */
static inline fp_28_36_t round_sample_rate(uint64_t num, uint32_t denom)
{
	uint64_t q1, r1, q2, r2, q3;

	if (denom == 0) {
		denom = 1;
	}

	q1 = num / denom;
	r1 = num % denom;

	q2 = (r1 << 32) / denom;
	r2 = (r1 << 32) % denom;

	q3 = ((r2 << 4) + (denom >> 1)) / denom;

	return (q1 << 36) + (q2 << 4) + q3;
}

usb_request_status_t usb_vendor_request_set_sample_rate_frac(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		usb_transfer_schedule_block(
			endpoint->out,
			&set_sample_r_params,
			sizeof(set_sample_r_params_t),
			NULL,
			NULL);
	} else if (stage == USB_TRANSFER_STAGE_DATA) {
		uint32_t numerator = set_sample_r_params.freq_hz;
		uint32_t denominator = set_sample_r_params.divider;
		uint64_t value = round_sample_rate(numerator, denominator);
		radio_reg_write(&radio, RADIO_BANK_ACTIVE, RADIO_SAMPLE_RATE, value);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_amp_enable(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_GAIN_TX_RF,
			endpoint->setup.value);
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_GAIN_RX_RF,
			endpoint->setup.value);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_lna_gain(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		uint8_t gain = endpoint->setup.index;
		radio_reg_write(&radio, RADIO_BANK_ACTIVE, RADIO_GAIN_RX_IF, gain);
		endpoint->buffer[0] = RADIO_OK;
		usb_transfer_schedule_block(
			endpoint->in,
			&endpoint->buffer,
			1,
			NULL,
			NULL);
		usb_transfer_schedule_ack(endpoint->out);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_vga_gain(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		uint8_t gain = endpoint->setup.index;
		radio_reg_write(&radio, RADIO_BANK_ACTIVE, RADIO_GAIN_RX_BB, gain);
		endpoint->buffer[0] = RADIO_OK;
		usb_transfer_schedule_block(
			endpoint->in,
			&endpoint->buffer,
			1,
			NULL,
			NULL);
		usb_transfer_schedule_ack(endpoint->out);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_txvga_gain(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		uint8_t gain = endpoint->setup.index;
		radio_reg_write(&radio, RADIO_BANK_ACTIVE, RADIO_GAIN_TX_IF, gain);
		endpoint->buffer[0] = RADIO_OK;
		usb_transfer_schedule_block(
			endpoint->in,
			&endpoint->buffer,
			1,
			NULL,
			NULL);
		usb_transfer_schedule_ack(endpoint->out);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_antenna_enable(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	switch (detected_platform()) {
	case BOARD_ID_HACKRF1_OG:
	case BOARD_ID_HACKRF1_R9:
	case BOARD_ID_PRALINE:
		// supported
		break;
	default:
		return USB_REQUEST_STATUS_STALL;
	}

	if (stage == USB_TRANSFER_STAGE_SETUP) {
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_BIAS_TEE,
			endpoint->setup.value);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

static volatile uint32_t _tx_underrun_limit;
static volatile uint32_t _rx_overrun_limit;

volatile transceiver_request_t transceiver_request = {
	.mode = TRANSCEIVER_MODE_OFF,
	.seq = 0,
};

void transceiver_usb_setup_complete(usb_endpoint_t* const endpoint)
{
	if (transceiver_request.mode == TRANSCEIVER_MODE_TX &&
	    endpoint->setup.request == 1 && auto_tx_flush) {
		// This is a request to leave TX mode. Do so but NAK for now.
		request_transceiver_mode(endpoint->setup.value);
	} else {
		usb_setup_complete(endpoint);
	}
}

// Must be called from an atomic context (normally USB ISR)
void request_transceiver_mode(transceiver_mode_t mode)
{
	usb_endpoint_flush(&usb_endpoint_bulk_in);
	usb_endpoint_flush(&usb_endpoint_bulk_out);

	edge_transceiver_request_superseded(mode);
	transceiver_request.mode = mode;
	transceiver_request.seq++;
}

void transceiver_shutdown(void)
{
	baseband_streaming_disable(&sgpio_config);
	operacake_sctimer_reset_state();

	usb_endpoint_flush(&usb_endpoint_bulk_in);
	usb_endpoint_flush(&usb_endpoint_bulk_out);

	led_off(LED2);
	led_off(LED3);
	radio_switch_opmode(&radio, TRANSCEIVER_MODE_OFF);
	m0_set_mode(M0_MODE_IDLE);
}

void transceiver_startup(const transceiver_mode_t mode)
{
	dma_started = 0;
	dma_pending = 0;
	usb_started = 0;
	usb_completed = 0;
	edge_reset_dma_ownership(edge_activation_epoch);

	transceiver_dma_setup();

	radio_switch_opmode(&radio, mode);

	switch (mode) {
	case TRANSCEIVER_MODE_RX_SWEEP:
	case TRANSCEIVER_MODE_RX:
		led_off(LED3);
		led_on(LED2);
		m0_set_mode(M0_MODE_RX);
		m0_state.shortfall_limit = _rx_overrun_limit;
		break;
	case TRANSCEIVER_MODE_TX:
		led_off(LED2);
		led_on(LED3);
		m0_set_mode(M0_MODE_TX_START);
		m0_state.shortfall_limit = _tx_underrun_limit;
		break;
	default:
		break;
	}

	activate_best_clock_source();
}

usb_request_status_t usb_vendor_request_set_transceiver_mode(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		switch (endpoint->setup.value) {
		case TRANSCEIVER_MODE_OFF:
		case TRANSCEIVER_MODE_RX:
		case TRANSCEIVER_MODE_TX:
		case TRANSCEIVER_MODE_RX_SWEEP:
		case TRANSCEIVER_MODE_CPLD_UPDATE:
			request_transceiver_mode(endpoint->setup.value);
			usb_transfer_schedule_ack(endpoint->in);
			return USB_REQUEST_STATUS_OK;
		default:
			return USB_REQUEST_STATUS_STALL;
		}
	} else {
		return USB_REQUEST_STATUS_OK;
	}
}

usb_request_status_t usb_vendor_request_set_hw_sync_mode(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		radio_reg_write(
			&radio,
			RADIO_BANK_ACTIVE,
			RADIO_TRIGGER,
			endpoint->setup.value);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_tx_underrun_limit(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		uint32_t value = (endpoint->setup.index << 16) + endpoint->setup.value;
		_tx_underrun_limit = transceiver_clamp_shortfall_limit(value);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_set_rx_overrun_limit(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		uint32_t value = (endpoint->setup.index << 16) + endpoint->setup.value;
		_rx_overrun_limit = transceiver_clamp_shortfall_limit(value);
		usb_transfer_schedule_ack(endpoint->in);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_get_buffer_size(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		uint32_t value = USB_SAMP_BUFFER_SIZE + USB_BULK_BUFFER_SIZE;
		endpoint->buffer[0] = value & 0xff;
		endpoint->buffer[1] = (value & 0xff00) >> 8;
		endpoint->buffer[2] = (value & 0xff0000) >> 16;
		endpoint->buffer[3] = (value & 0xff000000) >> 24;
		usb_transfer_schedule_block(
			endpoint->in,
			&endpoint->buffer,
			4,
			NULL,
			NULL);
		usb_transfer_schedule_ack(endpoint->out);

		// We now know the host is aware of our buffer size, so it
		// can make its own decisions about flushing the buffer.
		auto_tx_flush = false;

		return USB_REQUEST_STATUS_OK;
	}
	return USB_REQUEST_STATUS_OK;
}

/* clang-format off */

// Which GPDMA channel to use.
const uint32_t DMA_CHANNEL = 1;

// GPDMA CCONFIG register setting.
const uint32_t DMA_CONFIG =
	  GPDMA_CCONFIG_FLOWCNTRL(0) // memory-to-memory
	| GPDMA_CCONFIG_IE(0)        // no error interrupt
	| GPDMA_CCONFIG_ITC(1)       // terminal count interrupt
	| GPDMA_CCONFIG_L(0)         // do not lock
	| GPDMA_CCONFIG_H(0);        // do not halt

// GPDMA CCONTROL register setting (excluding TRANSFERSIZE field).
const uint32_t DMA_CONTROL =
	  GPDMA_CCONTROL_SBSIZE(7) // 256-transfer src bursts
	| GPDMA_CCONTROL_DBSIZE(7) // 256-transfer dst bursts
	| GPDMA_CCONTROL_SWIDTH(2) // 32-bit src transfers
	| GPDMA_CCONTROL_DWIDTH(2) // 32-bit dst transfers
	| GPDMA_CCONTROL_S(0)      // AHB Master 0
	| GPDMA_CCONTROL_D(1)      // AHB Master 1
	| GPDMA_CCONTROL_SI(1)     // increment source
	| GPDMA_CCONTROL_DI(1)     // increment destination
	| GPDMA_CCONTROL_PROT1(0)  // user mode
	| GPDMA_CCONTROL_PROT2(0)  // not bufferable
	| GPDMA_CCONTROL_PROT3(0)  // not cacheable
	| GPDMA_CCONTROL_I(1);     // interrupt enabled

/* clang-format on */

// Called before any sequence of DMA transfers.
void transceiver_dma_setup(void)
{
	gpdma_controller_enable();
	GPDMA_CCONFIG(DMA_CHANNEL) = DMA_CONFIG;
	GPDMA_CCONTROL(DMA_CHANNEL) = DMA_CONTROL;
	GPDMA_CLLI(DMA_CHANNEL) = 0;
	GPDMA_INTTCCLEAR = (1 << DMA_CHANNEL);
	nvic_enable_irq(NVIC_DMA_IRQ);
}

// Called to start each DMA transfer.
void transceiver_start_dma(void* src, void* dest, size_t size)
{
	uint32_t num_transfers = size >> 2;
	GPDMA_CCONTROL(DMA_CHANNEL) = DMA_CONTROL | num_transfers;
	GPDMA_CSRCADDR(DMA_CHANNEL) = (uint32_t) src;
	GPDMA_CDESTADDR(DMA_CHANNEL) = (uint32_t) dest;
	dma_pending = size;
	gpdma_channel_enable(DMA_CHANNEL);
}

static void edge_stream_reset(uint32_t activation_epoch)
{
	edge_reset_dma_ownership(activation_epoch);
	edge_previous_total_shortfall =
		m0_confirmed_shortfall_bytes(&m0_state);
	edge_previous_shortfall_count = m0_state.num_shortfalls;
	edge_quarantine_pending = false;
}

static uint32_t current_sample_rate_hz(void)
{
	const uint64_t rate =
		radio_reg_read(&radio, RADIO_BANK_APPLIED, RADIO_SAMPLE_RATE);
	if (rate == RADIO_UNSET) {
		return 0;
	}
	return (uint32_t) ((rate + (SR_FP_ONE_HZ / 2u)) / SR_FP_ONE_HZ);
}

static bool edge_start_next_dma_chunk(void)
{
	if (edge_dma_remaining == 0 ||
	    edge_dma_source_offset >= USB_SAMP_BUFFER_SIZE ||
	    edge_dma_destination_offset >= USB_BULK_BUFFER_SIZE) {
		return false;
	}
	uint32_t chunk = USB_SAMP_BUFFER_SIZE - edge_dma_source_offset;
	if (chunk > edge_dma_remaining) {
		chunk = edge_dma_remaining;
	}
	if (chunk == 0 || chunk > DMA_MAX_TRANSFER_SIZE || (chunk & 3u) != 0 ||
	    chunk > USB_BULK_BUFFER_SIZE - edge_dma_destination_offset) {
		return false;
	}
	transceiver_start_dma(
		&usb_samp_buffer[edge_dma_source_offset],
		&usb_bulk_buffer[edge_dma_destination_offset],
		chunk);
	return true;
}

// Called when a DMA transfer completes.
void dma_isr_impl(void)
{
	gpdma_channel_disable(DMA_CHANNEL);
	GPDMA_INTTCCLEAR = (1 << DMA_CHANNEL);

	const uint32_t completed = dma_pending;
	m0_state.m4_count += completed;
	dma_pending = 0;

	if (!edge_transport_dma_owned(&edge_transport)) {
		if (edge_dma_remaining != 0) {
			edge_abandon_dma_ownership();
			edge_note_loss_uncertain(edge_activation_epoch);
		}
		return;
	}
	if (!edge_transport_dma_chunk_valid(
		    &edge_transport, completed, edge_dma_remaining)) {
		edge_abandon_dma_ownership();
		edge_note_loss_uncertain(edge_activation_epoch);
		return;
	}

	edge_dma_source_offset =
		(edge_dma_source_offset + completed) & USB_SAMP_BUFFER_MASK;
	edge_dma_destination_offset += completed;
	edge_dma_remaining -= completed;
	if (edge_dma_remaining != 0) {
		if (!edge_start_next_dma_chunk()) {
			edge_abandon_dma_ownership();
			edge_note_loss_uncertain(edge_activation_epoch);
		}
	} else if (!edge_transport_finish_dma(&edge_transport, NULL)) {
		edge_abandon_dma_ownership();
		edge_note_loss_uncertain(edge_activation_epoch);
	}
}

void transceiver_bulk_transfer_complete(void* user_data, unsigned int bytes_transferred)
{
	(void) user_data;
	usb_completed += bytes_transferred;
}

static void edge_bulk_transfer_complete(void* user_data, unsigned int bytes_transferred)
{
	uint32_t activation_epoch;
	if (!edge_transport_complete_usb(
		    &edge_transport,
		    (uintptr_t) user_data,
		    NULL,
		    &activation_epoch)) {
		return;
	}
	usb_completed += bytes_transferred;
	if (bytes_transferred == EDGE_FRAME_BYTES) {
		edge_frame_completed(activation_epoch);
	} else {
		edge_note_usb_short_transfer(activation_epoch);
	}
}

typedef enum {
	DIRECTION_RX,
	DIRECTION_TX,
} direction_t;

void start_dma_if_possible(direction_t direction, size_t size)
{
	if (dma_pending) {
		return;
	}

	uint32_t sampling_completed = m0_state.m0_count;
	uint32_t dma_completed = m0_state.m4_count;
	uint32_t samp_offset = dma_started & USB_SAMP_BUFFER_MASK;
	uint32_t bulk_offset = dma_started & USB_BULK_BUFFER_MASK;
	uint32_t data_available, space_in_use, space_available, samp_buf_margin;
	uint8_t *dest, *src;

	if (direction == DIRECTION_RX) {
		data_available = sampling_completed - dma_started;
		space_in_use = usb_completed - dma_completed;
		space_available = USB_BULK_BUFFER_SIZE - space_in_use;
		samp_buf_margin = USB_SAMP_BUFFER_SIZE - data_available;
		src = &usb_samp_buffer[samp_offset];
		dest = &usb_bulk_buffer[bulk_offset];
	} else {
		data_available = usb_completed - dma_started;
		space_in_use = dma_completed - sampling_completed;
		space_available = USB_SAMP_BUFFER_SIZE - space_in_use;
		samp_buf_margin = space_in_use;
		src = &usb_bulk_buffer[bulk_offset];
		dest = &usb_samp_buffer[samp_offset];
	}

	if (data_available < size || size > space_available) {
		return;
	}

	uint32_t m0_buf_half = sampling_completed & BUF_HALF_MASK;
	uint32_t dma_buf_half = dma_started & BUF_HALF_MASK;
	bool same_buf_half = m0_buf_half == dma_buf_half;

	if (same_buf_half && samp_buf_margin >= (USB_SAMP_BUFFER_SIZE / 2)) {
		return;
	}

	transceiver_start_dma(src, dest, size);

	dma_started += size;
}

void start_usb_if_possible(direction_t direction)
{
	uint32_t bulk_offset = usb_started & USB_BULK_BUFFER_MASK;
	uint32_t dma_completed = m0_state.m4_count;
	uint32_t bytes_available;
	usb_endpoint_t* usb_endpoint;

	if (direction == DIRECTION_RX) {
		bytes_available = dma_completed - usb_started;
		usb_endpoint = &usb_endpoint_bulk_in;
	} else {
		uint32_t space_used = usb_started - dma_completed;
		bytes_available = USB_BULK_BUFFER_SIZE - space_used;
		usb_endpoint = &usb_endpoint_bulk_out;
	}

	if (bytes_available < USB_TRANSFER_SIZE) {
		return;
	}

	usb_transfer_schedule_block(
		usb_endpoint,
		&usb_bulk_buffer[bulk_offset],
		USB_TRANSFER_SIZE,
		transceiver_bulk_transfer_complete,
		NULL);

	usb_started += USB_TRANSFER_SIZE;
}

static bool edge_observe_loss_and_transitions(void)
{
	if (!edge_rx_is_framed(edge_activation_epoch)) {
		return true;
	}
	const uint64_t total_shortfall =
		m0_confirmed_shortfall_bytes(&m0_state);
	const uint32_t shortfall_count = m0_state.num_shortfalls;
	const int64_t lost_bytes =
		(int64_t) (total_shortfall - edge_previous_total_shortfall);
	if (lost_bytes > 0) {
		edge_note_hardware_loss_bytes(
			edge_activation_epoch,
			(uint64_t) lost_bytes);
		edge_previous_total_shortfall = total_shortfall;
	}
	if (m0_state.error == M0_ERROR_MISSED_DEADLINE) {
		edge_note_loss_uncertain(edge_activation_epoch);
		edge_quarantine_pending = true;
	}
	if (shortfall_count != edge_previous_shortfall_count) {
		edge_previous_shortfall_count = shortfall_count;
		edge_quarantine_pending = true;
	}
	if (edge_take_quarantine_request(edge_activation_epoch)) {
		edge_quarantine_pending = true;
	}

	/* A copied frame predates a newly observed transition and remains safe.
	 * Once its DMA is complete, drop every uncommitted byte in the SGPIO ring.
	 * The next frame therefore cannot concatenate samples across a loss,
	 * retune, rate change, or gain transition. */
	const bool no_reserve = edge_quarantine_pending;
	if (no_reserve && dma_pending == 0) {
		const uint32_t sampling_completed = m0_state.m0_count;
		const uint32_t discarded = sampling_completed - dma_started;
		dma_started = sampling_completed;
		m0_state.m4_count = sampling_completed;
		edge_note_dropped_bytes(edge_activation_epoch, discarded);
		edge_quarantine_pending = false;
	}
	return no_reserve;
}

static void edge_reconcile_final_shortfall_total(uint32_t activation_epoch)
{
	/* OFF can change the request sequence immediately after the M0 detects a
	 * missed deadline, before the main loop gets another observation pass. */
	if (m0_state.error == M0_ERROR_MISSED_DEADLINE) {
		edge_note_loss_uncertain(activation_epoch);
	}
	const uint64_t final_total =
		m0_confirmed_shortfall_bytes(&m0_state);
	const int64_t delta =
		(int64_t) (final_total - edge_previous_total_shortfall);
	if (delta > 0) {
		edge_note_hardware_loss_bytes(activation_epoch, (uint64_t) delta);
		edge_previous_total_shortfall = final_total;
	}
}

static void edge_start_dma_if_possible(
	uint32_t sampling_completed,
	bool no_reserve)
{
	if (!edge_rx_is_framed(edge_activation_epoch) || dma_pending) {
		return;
	}

	const uint8_t slot = edge_transport_dma_candidate(&edge_transport);
	if (slot == EDGE_TRANSPORT_SLOT_NONE) {
		return;
	}

	if (!edge_transport_snapshot_can_reserve(
		    sampling_completed,
		    dma_started,
		    EDGE_FRAME_PAYLOAD_BYTES,
		    no_reserve)) {
		return;
	}
	const uint32_t data_available = sampling_completed - dma_started;

	const uint32_t samp_buf_margin = USB_SAMP_BUFFER_SIZE - data_available;
	const uint32_t m0_buf_half = sampling_completed & BUF_HALF_MASK;
	const uint32_t dma_buf_half = dma_started & BUF_HALF_MASK;
	if (m0_buf_half == dma_buf_half &&
	    samp_buf_margin >= (USB_SAMP_BUFFER_SIZE / 2)) {
		return;
	}

	const uint32_t slot_offset = slot * EDGE_FRAME_BYTES;
	if (!edge_build_frame_header(
		    &usb_bulk_buffer[slot_offset],
		    current_sample_rate_hz(),
		    edge_activation_epoch)) {
		return;
	}
	if (!edge_transport_begin_dma(
		    &edge_transport, slot, edge_activation_epoch)) {
		return;
	}
	edge_dma_source_offset = dma_started & USB_SAMP_BUFFER_MASK;
	edge_dma_destination_offset = slot_offset + EDGE_FRAME_HEADER_BYTES;
	edge_dma_remaining = EDGE_FRAME_PAYLOAD_BYTES;
	dma_started += EDGE_FRAME_PAYLOAD_BYTES;
	if (!edge_start_next_dma_chunk()) {
		edge_abandon_dma_ownership();
		edge_note_loss_uncertain(edge_activation_epoch);
	}
}

static void edge_start_usb_if_possible(uint32_t request_sequence)
{
	/* OFF and Edge RESET both run in the USB ISR. Keep that ISR masked from
	 * the final ownership check through queue insertion: otherwise OFF could
	 * flush the endpoint, then this old activation could enqueue one late
	 * framed transfer before RESET. The queue's own global critical section
	 * does not re-enable this individually masked IRQ. */
	nvic_disable_irq(NVIC_USB0_IRQ);
	if (transceiver_request.seq != request_sequence ||
	    transceiver_request.mode != TRANSCEIVER_MODE_RX ||
	    !edge_rx_is_framed(edge_activation_epoch)) {
		nvic_enable_irq(NVIC_USB0_IRQ);
		return;
	}
	const uint8_t slot = edge_transport_usb_candidate(&edge_transport);
	if (slot == EDGE_TRANSPORT_SLOT_NONE) {
		nvic_enable_irq(NVIC_USB0_IRQ);
		return;
	}
	(void) edge_patch_frame_for_usb_short(
		&usb_bulk_buffer[slot * EDGE_FRAME_BYTES]);
	if (!edge_transport_begin_usb(&edge_transport, slot)) {
		nvic_enable_irq(NVIC_USB0_IRQ);
		return;
	}

	const uintptr_t completion_token =
		edge_transport_completion_token(&edge_transport, slot);
	const int scheduled = usb_transfer_schedule(
		&usb_endpoint_bulk_in,
		&usb_bulk_buffer[slot * EDGE_FRAME_BYTES],
		EDGE_FRAME_BYTES,
		edge_bulk_transfer_complete,
		(void*) completion_token);
	edge_transport_usb_schedule_result(
		&edge_transport, slot, scheduled == 0);
	if (scheduled == 0) {
		usb_started += EDGE_FRAME_BYTES;
	}
	nvic_enable_irq(NVIC_USB0_IRQ);
}

static void edge_cancel_dma(void)
{
	/* Mask first. A terminal-count ISR that ran before this point owns and
	 * completes the current chunk. Once masked, clearing both the peripheral
	 * and NVIC pending state prevents a delayed ISR from restarting a wrapped
	 * chunk after the slot has been released. */
	nvic_disable_irq(NVIC_DMA_IRQ);
	gpdma_channel_disable(DMA_CHANNEL);
	GPDMA_INTTCCLEAR = (1 << DMA_CHANNEL);
	dma_pending = 0;
	edge_transport_cancel_dma(&edge_transport);
	edge_dma_source_offset = 0;
	edge_dma_destination_offset = 0;
	edge_dma_remaining = 0;
	nvic_clear_pending_irq(NVIC_DMA_IRQ);
	nvic_enable_irq(NVIC_DMA_IRQ);
}

static int8_t saturation_buffer;
static uint64_t saturation_buffer_time;
static uint64_t runtime_counter_time;
volatile uint64_t systick_counter = 0;

static void update_rx_saturation_ui(void)
{
	const int8_t sample_value = *(
		int8_t*) &usb_samp_buffer[m0_state.m0_count & USB_SAMP_BUFFER_MASK];

	if (sample_value > saturation_buffer) {
		saturation_buffer = sample_value;
	}
	if (-sample_value > saturation_buffer) {
		saturation_buffer = -sample_value;
	}
	if (saturation_buffer_time + 4 < systick_counter) {
		saturation_buffer_time = systick_counter;
		hackrf_ui()->set_saturation(saturation_buffer);
		saturation_buffer = 0;
	}
}

void sys_tick_handler_impl(void)
{
	systick_counter++;
}

void rx_mode(uint32_t seq)
{
	if (transceiver_request.seq != seq ||
	    transceiver_request.mode != TRANSCEIVER_MODE_RX) {
		return;
	}
	transceiver_startup(TRANSCEIVER_MODE_RX);
	bool framed = false;
	if (!edge_rx_start(
		    current_sample_rate_hz(),
		    seq,
		    &edge_activation_epoch,
		    &framed)) {
		transceiver_shutdown();
		return;
	}
	runtime_counter_time = systick_counter;
	if (framed) {
		edge_stream_reset(edge_activation_epoch);
	}

	baseband_streaming_enable(&sgpio_config);

	while (transceiver_request.seq == seq) {
		if (framed) {
			/* M0 recovery publishes its shortfall state before advancing m0_count.
			 * Snapshot first: a gap racing after observation cannot contribute its
			 * post-gap bytes to this reservation, while a snapshot that already
			 * includes them is paired with the published quarantine observation. */
			const uint32_t sampling_completed = m0_state.m0_count;
			const bool no_reserve = edge_observe_loss_and_transitions();
			edge_start_dma_if_possible(sampling_completed, no_reserve);
			edge_start_usb_if_possible(seq);
		} else {
			start_dma_if_possible(DIRECTION_RX, DMA_TRANSFER_SIZE);
			start_usb_if_possible(DIRECTION_RX);
		}

		if (runtime_counter_time != systick_counter) {
			runtime_counter_time = systick_counter;
			edge_update_runtime_counters(
				edge_activation_epoch,
				m0_state.m0_count,
				m0_state.m4_count,
				usb_completed,
				m0_state.num_shortfalls,
				m0_state.longest_shortfall);
			if (!framed) {
				edge_update_legacy_timeline(
					edge_activation_epoch,
					m0_confirmed_shortfall_bytes(&m0_state));
			}
		}
		update_rx_saturation_ui();
		radio_update(&radio);
	}

	if (framed) {
		edge_cancel_dma();
	}
	/* Mark Edge inactive before the OFF radio configuration is applied, so
	 * shutdown-only gain/rate changes cannot become stream transitions. */
	edge_rx_stop(edge_activation_epoch);
	transceiver_shutdown();
	if (framed) {
		/* request_transceiver_mode() flushes the endpoint before changing the
		 * sequence, and transceiver_shutdown() repeats that flush after the DMA
		 * channel has been cancelled. This is the safe software-ownership reset
		 * boundary for ordinary OFF and usb_configuration_changed() (including a
		 * bus reset): no old USB callback or DMA ISR may retain a frame slot. The
		 * per-submission completion serial deliberately survives this reset. */
		edge_transport_reset(&edge_transport, edge_activation_epoch);
		edge_reconcile_final_shortfall_total(edge_activation_epoch);
	}
	edge_update_runtime_counters(
		edge_activation_epoch,
		m0_state.m0_count,
		m0_state.m4_count,
		usb_completed,
		m0_state.num_shortfalls,
		m0_state.longest_shortfall);
	if (!framed) {
		edge_update_legacy_timeline(
			edge_activation_epoch,
			m0_confirmed_shortfall_bytes(&m0_state));
	}
}

void tx_mode(uint32_t seq)
{
	transceiver_startup(TRANSCEIVER_MODE_TX);

	// First, make transfers directly into the sample buffer to fill it.
	for (int i = 0; i < (USB_SAMP_BUFFER_SIZE / USB_TRANSFER_SIZE); i++) {
		// Set up transfer.
		usb_transfer_schedule_block(
			&usb_endpoint_bulk_out,
			&usb_samp_buffer[usb_started],
			USB_TRANSFER_SIZE,
			transceiver_bulk_transfer_complete,
			NULL);
		usb_started += USB_TRANSFER_SIZE;

		// Wait for the transfer to complete.
		while (usb_completed < usb_started) {
			// Handle the host switching modes before filling the buffer.
			if (transceiver_request.seq != seq) {
				transceiver_shutdown();
				return;
			}

			radio_update(&radio);
		}
	}

	// Sample buffer is now full. Update DMA counters accordingly.
	dma_started = USB_SAMP_BUFFER_SIZE;
	m0_state.m4_count = USB_SAMP_BUFFER_SIZE;

	// Start transmitting samples.
	baseband_streaming_enable(&sgpio_config);

	// Continue feeding samples to the sample buffer.
	while (transceiver_request.seq == seq) {
		start_dma_if_possible(DIRECTION_TX, DMA_TRANSFER_SIZE);
		start_usb_if_possible(DIRECTION_TX);
		radio_update(&radio);
	}

	// Host has now requested to stop TX. If we're not auto-flushing, we
	// should now stop TX immediately.

	if (!auto_tx_flush) {
		transceiver_shutdown();
		return;
	}

	// Otherwise, we should now ensure all bytes sent by the host are
	// transmitted before we leave TX. First, we should make sure all data
	// currently in the USB bulk buffer reaches the sample buffer.

	if ((usb_started - usb_completed) > 0) {
		// We were part way through a 16KB firmware-side transfer when
		// the transceiver mode change request to stop TX was received.
		//
		// We want to include the contents of that partial transfer in
		// the data we move to the sample buffer.
		//
		// The transfer was already stopped by usb_endpoint_flush(),
		// which was called from request_transceiver_mode().
		//
		// We will not have had a callback, and the transfer descriptor
		// (dTD) will not have been updated, since the transfer did not
		// complete.
		//
		// However, as long as we haven't started a new transfer, we
		// can retrieve the partial byte count from the transfer
		// overlay in the endpoint queue head (dQH) (UM10503 25.9.1).

		usb_queue_head_t* const qh =
			usb_queue_head(usb_endpoint_bulk_out.address);
		unsigned int bytes_remaining =
			(qh->total_bytes & USB_TD_DTD_TOKEN_TOTAL_BYTES_MASK) >>
			USB_TD_DTD_TOKEN_TOTAL_BYTES_SHIFT;
		unsigned int bytes_transferred = USB_TRANSFER_SIZE - bytes_remaining;
		usb_completed += bytes_transferred;
	}

	// Feed the remaining data from the bulk buffer to the sample buffer.
	// At this point, we also need to handle the case where there is less data
	// to be transferred to the sample buffer than a full-sized DMA transfer.

	// Any remainder of less than 4 bytes will be ignored; this is the chunk
	// size of our DMA transfers.
	while ((usb_completed - m0_state.m4_count) >= 4) {
		uint32_t data_available = usb_completed - dma_started;
		if (data_available > DMA_TRANSFER_SIZE) {
			start_dma_if_possible(DIRECTION_TX, DMA_TRANSFER_SIZE);
		} else {
			start_dma_if_possible(DIRECTION_TX, data_available);
		}
		radio_update(&radio);
	}

	// Wait for the data in the sample buffer to be transmitted.

	// Any remainder of less than 32 bytes will be ignored; this is
	// the chunk size used by the M0 core to transfer samples to SGPIO.
	while ((m0_state.m4_count - m0_state.m0_count) >= 32) {
		radio_update(&radio);
	}

	// All data received from the host has now been transmitted.
	// Now we can ACK the control request that took us out of TX mode.
	usb_transfer_schedule_ack(usb_endpoint_control_in.in);

	transceiver_shutdown();
}

void off_mode(uint32_t seq)
{
	while (transceiver_request.seq == seq) {
		radio_update(&radio);
	}
}
