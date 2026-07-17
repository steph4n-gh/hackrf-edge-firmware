/*
 * Copyright 2026 HackRF Edge contributors
 *
 * This file is part of HackRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_TRANSPORT_SLOT_COUNT 2u
#define EDGE_TRANSPORT_SLOT_NONE UINT8_MAX
#define EDGE_TRANSPORT_TOKEN_SERIAL_MASK UINT32_C(0x7fffffff)

typedef enum {
	EDGE_TRANSPORT_SLOT_FREE = 0,
	EDGE_TRANSPORT_SLOT_DMA,
	EDGE_TRANSPORT_SLOT_READY,
	EDGE_TRANSPORT_SLOT_USB,
} edge_transport_slot_state_t;

typedef struct {
	volatile edge_transport_slot_state_t state;
	uint32_t activation_epoch;
} edge_transport_slot_t;

typedef struct {
	edge_transport_slot_t slots[EDGE_TRANSPORT_SLOT_COUNT];
	volatile uint8_t dma_slot;
	uint8_t next_dma_slot;
	uint8_t next_usb_slot;
	/* At most one slot may be USB-owned, so one persistent serial identifies
	 * the current submission without a duplicate serial in every slot. This
	 * counter intentionally survives edge_transport_reset(): each slot is reused
	 * many times during a continuous stream. The completion serial and all
	 * otherwise-unspecified initializer fields must start at zero; dma_slot must
	 * use EDGE_TRANSPORT_STATE_INITIALIZER's explicit NONE sentinel. */
	uint32_t next_completion_serial;
} edge_transport_state_t;

/* A zero-filled transport has dma_slot == 0, which is a real slot.  Use this
 * initializer for static storage so legacy DMA cannot be mistaken for a
 * framed transfer before the first edge_transport_reset(). */
#define EDGE_TRANSPORT_STATE_INITIALIZER \
	{ .dma_slot = EDGE_TRANSPORT_SLOT_NONE }

static inline void edge_transport_reset(
	edge_transport_state_t* transport,
	uint32_t activation_epoch)
{
	uint8_t slot;
	for (slot = 0; slot < EDGE_TRANSPORT_SLOT_COUNT; ++slot) {
		transport->slots[slot].state = EDGE_TRANSPORT_SLOT_FREE;
		transport->slots[slot].activation_epoch = activation_epoch;
	}
	transport->dma_slot = EDGE_TRANSPORT_SLOT_NONE;
	transport->next_dma_slot = 0;
	transport->next_usb_slot = 0;
}

static inline bool edge_transport_dma_owned(
	const edge_transport_state_t* transport)
{
	const uint8_t slot = transport->dma_slot;
	return slot < EDGE_TRANSPORT_SLOT_COUNT &&
		transport->slots[slot].state == EDGE_TRANSPORT_SLOT_DMA;
}

static inline bool edge_transport_dma_chunk_valid(
	const edge_transport_state_t* transport,
	uint32_t completed,
	uint32_t remaining)
{
	return edge_transport_dma_owned(transport) && completed != 0 &&
		completed <= remaining;
}

static inline bool edge_transport_snapshot_can_reserve(
	uint32_t sampling_completed,
	uint32_t reserved,
	uint32_t required,
	bool no_reserve)
{
	/* Short-circuit before subtracting when observation advanced the discard
	 * boundary beyond this deliberately older sampling snapshot. */
	return !no_reserve && (uint32_t) (sampling_completed - reserved) >= required;
}

static inline uint8_t edge_transport_dma_candidate(
	const edge_transport_state_t* transport)
{
	const uint8_t slot = transport->next_dma_slot;
	if (transport->dma_slot != EDGE_TRANSPORT_SLOT_NONE ||
	    slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    transport->slots[slot].state != EDGE_TRANSPORT_SLOT_FREE) {
		return EDGE_TRANSPORT_SLOT_NONE;
	}
	return slot;
}

static inline bool edge_transport_begin_dma(
	edge_transport_state_t* transport,
	uint8_t slot,
	uint32_t activation_epoch)
{
	if (slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    slot != edge_transport_dma_candidate(transport)) {
		return false;
	}
	transport->slots[slot].activation_epoch = activation_epoch;
	transport->slots[slot].state = EDGE_TRANSPORT_SLOT_DMA;
	transport->dma_slot = slot;
	transport->next_dma_slot ^= 1u;
	return true;
}

static inline bool edge_transport_finish_dma(
	edge_transport_state_t* transport,
	uint8_t* completed_slot)
{
	const uint8_t slot = transport->dma_slot;
	if (slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    transport->slots[slot].state != EDGE_TRANSPORT_SLOT_DMA) {
		return false;
	}
	transport->slots[slot].state = EDGE_TRANSPORT_SLOT_READY;
	transport->dma_slot = EDGE_TRANSPORT_SLOT_NONE;
	if (completed_slot != NULL) {
		*completed_slot = slot;
	}
	return true;
}

static inline void edge_transport_cancel_dma(edge_transport_state_t* transport)
{
	const uint8_t slot = transport->dma_slot;
	if (slot < EDGE_TRANSPORT_SLOT_COUNT &&
	    transport->slots[slot].state == EDGE_TRANSPORT_SLOT_DMA) {
		transport->slots[slot].state = EDGE_TRANSPORT_SLOT_FREE;
		/* Refill the abandoned slot first.  The USB cursor is waiting for
		 * this slot, so advancing from the other slot would reorder frames. */
		transport->next_dma_slot = slot;
	}
	transport->dma_slot = EDGE_TRANSPORT_SLOT_NONE;
}

static inline uint8_t edge_transport_usb_candidate(
	const edge_transport_state_t* transport)
{
	uint8_t scan;
	const uint8_t slot = transport->next_usb_slot;
	if (slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    transport->slots[slot].state != EDGE_TRANSPORT_SLOT_READY) {
		return EDGE_TRANSPORT_SLOT_NONE;
	}
	for (scan = 0; scan < EDGE_TRANSPORT_SLOT_COUNT; ++scan) {
		if (transport->slots[scan].state == EDGE_TRANSPORT_SLOT_USB) {
			return EDGE_TRANSPORT_SLOT_NONE;
		}
	}
	return slot;
}

static inline bool edge_transport_begin_usb(
	edge_transport_state_t* transport,
	uint8_t slot)
{
	if (slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    slot != edge_transport_usb_candidate(transport)) {
		return false;
	}
	uint32_t serial =
		(transport->next_completion_serial + 1u) &
		EDGE_TRANSPORT_TOKEN_SERIAL_MASK;
	/* Avoid a null user_data token and make a zero serial unambiguously
	 * invalid when checking transport invariants. */
	if (serial == 0) {
		serial = 1;
	}
	transport->next_completion_serial = serial;
	transport->slots[slot].state = EDGE_TRANSPORT_SLOT_USB;
	return true;
}

static inline void edge_transport_usb_schedule_result(
	edge_transport_state_t* transport,
	uint8_t slot,
	bool scheduled)
{
	if (slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    transport->slots[slot].state != EDGE_TRANSPORT_SLOT_USB) {
		return;
	}
	if (scheduled) {
		transport->next_usb_slot ^= 1u;
	} else {
		transport->slots[slot].state = EDGE_TRANSPORT_SLOT_READY;
	}
}

static inline uintptr_t edge_transport_completion_token(
	const edge_transport_state_t* transport,
	uint8_t slot)
{
	return ((uintptr_t) (transport->next_completion_serial &
					     EDGE_TRANSPORT_TOKEN_SERIAL_MASK)
			<< 1) |
			slot;
}

static inline bool edge_transport_complete_usb(
	edge_transport_state_t* transport,
	uintptr_t token,
	uint8_t* completed_slot,
	uint32_t* activation_epoch)
{
	const uint8_t slot = (uint8_t) (token & 1u);
	const uintptr_t token_serial = token >> 1;
	if (token_serial == 0 ||
	    token_serial > EDGE_TRANSPORT_TOKEN_SERIAL_MASK ||
	    slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    transport->slots[slot].state != EDGE_TRANSPORT_SLOT_USB ||
	    transport->next_completion_serial != (uint32_t) token_serial) {
		return false;
	}
	if (completed_slot != NULL) {
		*completed_slot = slot;
	}
	if (activation_epoch != NULL) {
		*activation_epoch = transport->slots[slot].activation_epoch;
	}
	transport->slots[slot].state = EDGE_TRANSPORT_SLOT_FREE;
	return true;
}

static inline bool edge_transport_invariant(
	const edge_transport_state_t* transport)
{
	uint8_t slot;
	uint8_t dma_count = 0;
	uint8_t usb_count = 0;
	if (transport->next_dma_slot >= EDGE_TRANSPORT_SLOT_COUNT ||
	    transport->next_usb_slot >= EDGE_TRANSPORT_SLOT_COUNT) {
		return false;
	}
	for (slot = 0; slot < EDGE_TRANSPORT_SLOT_COUNT; ++slot) {
		dma_count += transport->slots[slot].state == EDGE_TRANSPORT_SLOT_DMA;
		usb_count += transport->slots[slot].state == EDGE_TRANSPORT_SLOT_USB;
	}
	if (dma_count > 1 || usb_count > 1) {
		return false;
	}
	if (transport->dma_slot == EDGE_TRANSPORT_SLOT_NONE) {
		return dma_count == 0;
	}
	return transport->dma_slot < EDGE_TRANSPORT_SLOT_COUNT && dma_count == 1 &&
		transport->slots[transport->dma_slot].state == EDGE_TRANSPORT_SLOT_DMA;
}
