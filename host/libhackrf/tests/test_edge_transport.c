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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "edge_transport.h"

#define assert(condition)                                                    \
	do {                                                                  \
		if (!(condition)) {                                             \
			fprintf(stderr, "%s:%d: assertion failed: %s\n",         \
				__FILE__, __LINE__, #condition);                    \
			abort();                                                  \
		}                                                             \
	} while (0)

static uint8_t finish_dma(edge_transport_state_t* transport)
{
	uint8_t slot = EDGE_TRANSPORT_SLOT_NONE;
	assert(edge_transport_finish_dma(transport, &slot));
	return slot;
}

static uintptr_t make_usb_ready(
	edge_transport_state_t* transport,
	uint32_t activation_epoch)
{
	const uint8_t slot = edge_transport_dma_candidate(transport);
	assert(slot != EDGE_TRANSPORT_SLOT_NONE);
	assert(edge_transport_begin_dma(transport, slot, activation_epoch));
	assert(finish_dma(transport) == slot);
	assert(edge_transport_usb_candidate(transport) == slot);
	assert(edge_transport_begin_usb(transport, slot));
	return edge_transport_completion_token(transport, slot);
}

static void test_static_initializer_is_legacy_safe(void)
{
	edge_transport_state_t transport = EDGE_TRANSPORT_STATE_INITIALIZER;

	assert(edge_transport_invariant(&transport));
	assert(!edge_transport_dma_owned(&transport));
	assert(!edge_transport_dma_chunk_valid(&transport, 8192, 0));
}

typedef struct {
	edge_transport_state_t transport;
	uint64_t m4_count;
	uint32_t dma_pending;
	uint32_t dma_remaining;
	unsigned int continuations;
	unsigned int invalid_abandons;
} shared_dma_model_t;

static void modeled_shared_dma_isr(shared_dma_model_t* model)
{
	const uint32_t completed = model->dma_pending;
	model->m4_count += completed;
	model->dma_pending = 0;
	if (!edge_transport_dma_owned(&model->transport)) {
		if (model->dma_remaining != 0) {
			edge_transport_cancel_dma(&model->transport);
			model->dma_remaining = 0;
			model->invalid_abandons++;
		}
		return;
	}
	if (!edge_transport_dma_chunk_valid(
		    &model->transport, completed, model->dma_remaining)) {
		edge_transport_cancel_dma(&model->transport);
		model->dma_remaining = 0;
		model->invalid_abandons++;
		return;
	}
	model->dma_remaining -= completed;
	if (model->dma_remaining != 0) {
		model->dma_pending = model->dma_remaining;
		model->continuations++;
	} else {
		assert(edge_transport_finish_dma(&model->transport, NULL));
	}
}

static void run_legacy_first_dma_model(void)
{
	shared_dma_model_t model = {
		.transport = EDGE_TRANSPORT_STATE_INITIALIZER,
	};

	/* 64 ordinary 8 KiB completions cover sixteen 32 KiB sample-buffer wraps.
	 * This is the exact ISR entry shared by legacy RX and TX. */
	for (unsigned int transfer = 0; transfer < 64; ++transfer) {
		model.dma_pending = 8192;
		modeled_shared_dma_isr(&model);
		assert(model.m4_count == (uint64_t) (transfer + 1) * 8192u);
		assert(model.dma_pending == 0);
		assert(model.dma_remaining == 0);
		assert(model.continuations == 0);
		assert(model.invalid_abandons == 0);
		assert(!edge_transport_dma_owned(&model.transport));
		assert(edge_transport_invariant(&model.transport));
	}
}

static void test_legacy_first_rx_tx_shared_isr(void)
{
	run_legacy_first_dma_model();
	run_legacy_first_dma_model();
}

static void test_framed_dma_chunk_boundaries(void)
{
	const uint32_t sample_buffer_size = 32768;
	const uint32_t sample_buffer_mask = sample_buffer_size - 1;
	const uint32_t bulk_buffer_size = 32768;
	const uint32_t frame_size = 16384;
	const uint32_t header_size = 64;
	const uint32_t payload_size = 16320;
	const uint32_t dma_max = 16380;

	for (uint32_t slot = 0; slot < 2; ++slot) {
		for (uint32_t source = 0; source < sample_buffer_size; source += 32) {
			uint32_t source_offset = source;
			uint32_t destination_offset = slot * frame_size + header_size;
			uint32_t remaining = payload_size;
			uint32_t copied = 0;
			unsigned int chunks = 0;
			while (remaining != 0) {
				uint32_t chunk = sample_buffer_size - source_offset;
				if (chunk > remaining) {
					chunk = remaining;
				}
				assert(chunk != 0);
				assert((chunk & 3u) == 0);
				assert(chunk <= dma_max);
				assert(destination_offset < bulk_buffer_size);
				assert(chunk <= bulk_buffer_size - destination_offset);
				source_offset =
					(source_offset + chunk) & sample_buffer_mask;
				destination_offset += chunk;
				remaining -= chunk;
				copied += chunk;
				chunks++;
			}
			assert(copied == payload_size);
			assert(destination_offset == (slot + 1) * frame_size);
			assert(chunks >= 1 && chunks <= 2);
		}
	}
}

static void test_invalid_dma_completion_is_abandoned(void)
{
	for (unsigned int invalid_kind = 0; invalid_kind < 2; ++invalid_kind) {
		shared_dma_model_t model = {
			.transport = EDGE_TRANSPORT_STATE_INITIALIZER,
			.dma_pending = invalid_kind == 0 ? 0 : 16321,
			.dma_remaining = 16320,
		};
		edge_transport_reset(&model.transport, 6);
		assert(edge_transport_begin_dma(&model.transport, 0, 6));
		modeled_shared_dma_isr(&model);
		assert(model.invalid_abandons == 1);
		assert(model.dma_remaining == 0);
		assert(model.dma_pending == 0);
		assert(!edge_transport_dma_owned(&model.transport));
		assert(edge_transport_dma_candidate(&model.transport) == 0);
		assert(edge_transport_invariant(&model.transport));
	}
}

static void test_pipeline_and_queue_ownership(void)
{
	edge_transport_state_t transport = {0};
	uint8_t slot;
	uint32_t epoch;

	edge_transport_reset(&transport, 7);
	assert(edge_transport_invariant(&transport));
	assert(edge_transport_dma_candidate(&transport) == 0);
	assert(edge_transport_begin_dma(&transport, 0, 7));
	assert(edge_transport_dma_candidate(&transport) ==
	       EDGE_TRANSPORT_SLOT_NONE);
	assert(finish_dma(&transport) == 0);

	assert(edge_transport_dma_candidate(&transport) == 1);
	assert(edge_transport_begin_dma(&transport, 1, 7));
	assert(finish_dma(&transport) == 1);
	assert(edge_transport_invariant(&transport));

	assert(edge_transport_usb_candidate(&transport) == 0);
	assert(edge_transport_begin_usb(&transport, 0));
	assert(edge_transport_usb_candidate(&transport) ==
	       EDGE_TRANSPORT_SLOT_NONE);
	edge_transport_usb_schedule_result(&transport, 0, true);
	assert(edge_transport_complete_usb(
		&transport,
		edge_transport_completion_token(&transport, 0),
		&slot,
		&epoch));
	assert(slot == 0);
	assert(epoch == 7);
	assert(edge_transport_usb_candidate(&transport) == 1);
	assert(edge_transport_begin_usb(&transport, 1));
	edge_transport_usb_schedule_result(&transport, 1, true);
	assert(edge_transport_complete_usb(
		&transport,
		edge_transport_completion_token(&transport, 1),
		NULL,
		NULL));
	assert(edge_transport_invariant(&transport));
}

static void test_schedule_failure_is_retryable(void)
{
	edge_transport_state_t transport = {0};
	edge_transport_reset(&transport, 12);
	const uintptr_t token = make_usb_ready(&transport, 12);

	edge_transport_usb_schedule_result(&transport, 0, false);
	assert(transport.slots[0].state == EDGE_TRANSPORT_SLOT_READY);
	assert(transport.next_usb_slot == 0);
	assert(!edge_transport_complete_usb(
		&transport, token, NULL, NULL));
	assert(edge_transport_usb_candidate(&transport) == 0);
	assert(edge_transport_invariant(&transport));
}

static void test_stale_completion_cannot_release_reused_slot(void)
{
	edge_transport_state_t transport = {0};
	uint32_t completed_bytes = 0;
	edge_transport_reset(&transport, 21);
	const uintptr_t stale_token = make_usb_ready(&transport, 21);

	edge_transport_usb_schedule_result(&transport, 0, true);
	edge_transport_reset(&transport, 22);
	const uintptr_t current_token = make_usb_ready(&transport, 22);
	edge_transport_usb_schedule_result(&transport, 0, true);

	if (edge_transport_complete_usb(
		    &transport, stale_token, NULL, NULL)) {
		completed_bytes += 16384;
	}
	assert(completed_bytes == 0);
	assert(transport.slots[0].state == EDGE_TRANSPORT_SLOT_USB);
	assert(edge_transport_complete_usb(
		&transport, current_token, NULL, NULL));
	completed_bytes += 16384;
	assert(completed_bytes == 16384);
	assert(edge_transport_invariant(&transport));
}

static void test_stale_completion_cannot_release_same_epoch_slot_reuse(void)
{
	edge_transport_state_t transport = {0};
	uint32_t epoch;
	edge_transport_reset(&transport, 23);

	const uintptr_t stale_token = make_usb_ready(&transport, 23);
	edge_transport_usb_schedule_result(&transport, 0, true);
	assert(edge_transport_complete_usb(
		&transport, stale_token, NULL, &epoch));
	assert(epoch == 23);

	/* Cycle through slot 1 so the DMA and USB cursors return to slot 0
	 * without changing the activation epoch. */
	const uintptr_t slot_one_token = make_usb_ready(&transport, 23);
	edge_transport_usb_schedule_result(&transport, 1, true);
	assert(edge_transport_complete_usb(
		&transport, slot_one_token, NULL, NULL));

	const uintptr_t current_token = make_usb_ready(&transport, 23);
	edge_transport_usb_schedule_result(&transport, 0, true);
	assert(current_token != stale_token);
	assert(!edge_transport_complete_usb(
		&transport, stale_token, NULL, NULL));
	assert(transport.slots[0].state == EDGE_TRANSPORT_SLOT_USB);
	assert(edge_transport_complete_usb(
		&transport, current_token, NULL, NULL));
	assert(edge_transport_invariant(&transport));
}

static void test_reset_invalidates_usb_ownership_and_preserves_nonce(void)
{
	edge_transport_state_t transport = {0};
	edge_transport_reset(&transport, 24);
	const uintptr_t stale_token = make_usb_ready(&transport, 24);
	edge_transport_usb_schedule_result(&transport, 0, true);

	/* This is the software ownership boundary used after endpoint flush and
	 * DMA cancellation during stop/configuration loss. */
	edge_transport_reset(&transport, 24);
	assert(!edge_transport_complete_usb(
		&transport, stale_token, NULL, NULL));
	const uintptr_t current_token = make_usb_ready(&transport, 24);
	edge_transport_usb_schedule_result(&transport, 0, true);
	assert(current_token != stale_token);
	assert(edge_transport_complete_usb(
		&transport, current_token, NULL, NULL));
	assert(edge_transport_invariant(&transport));
}

static void test_dma_abandon_preserves_other_usb_slot(void)
{
	edge_transport_state_t transport = EDGE_TRANSPORT_STATE_INITIALIZER;
	uint8_t completed_slot = EDGE_TRANSPORT_SLOT_NONE;
	uint32_t completed_epoch = 0;

	edge_transport_reset(&transport, 26);
	const uintptr_t usb_token = make_usb_ready(&transport, 26);
	edge_transport_usb_schedule_result(&transport, 0, true);
	assert(transport.slots[0].state == EDGE_TRANSPORT_SLOT_USB);

	assert(edge_transport_dma_candidate(&transport) == 1);
	assert(edge_transport_begin_dma(&transport, 1, 26));
	assert(edge_transport_dma_owned(&transport));
	edge_transport_cancel_dma(&transport);

	assert(transport.slots[0].state == EDGE_TRANSPORT_SLOT_USB);
	assert(transport.slots[1].state == EDGE_TRANSPORT_SLOT_FREE);
	assert(!edge_transport_dma_owned(&transport));
	assert(edge_transport_dma_candidate(&transport) == 1);
	assert(edge_transport_complete_usb(
		&transport, usb_token, &completed_slot, &completed_epoch));
	assert(completed_slot == 0);
	assert(completed_epoch == 26);
	assert(edge_transport_dma_candidate(&transport) == 1);
	assert(edge_transport_begin_dma(&transport, 1, 26));
	assert(finish_dma(&transport) == 1);
	assert(edge_transport_usb_candidate(&transport) == 1);
	assert(edge_transport_invariant(&transport));
}

static void test_completion_serial_wrap_skips_null_token(void)
{
	edge_transport_state_t transport = {0};
	edge_transport_reset(&transport, 25);
	transport.next_completion_serial = EDGE_TRANSPORT_TOKEN_SERIAL_MASK - 1u;

	const uintptr_t final_token = make_usb_ready(&transport, 25);
	edge_transport_usb_schedule_result(&transport, 0, true);
	assert((final_token >> 1) == EDGE_TRANSPORT_TOKEN_SERIAL_MASK);
	assert(edge_transport_complete_usb(
		&transport, final_token, NULL, NULL));

	const uintptr_t wrapped_token = make_usb_ready(&transport, 25);
	edge_transport_usb_schedule_result(&transport, 1, true);
	assert((wrapped_token >> 1) == 1u);
	assert(wrapped_token != 0);
	assert(edge_transport_complete_usb(
		&transport, wrapped_token, NULL, NULL));
	assert(edge_transport_invariant(&transport));
}

static void test_pre_observe_snapshot_closes_gap_threshold_race(void)
{
	const uint32_t payload = 16320;
	const uint32_t pre_gap_snapshot = 16288;
	const uint32_t post_gap_live_count = pre_gap_snapshot + 32;

	/* The old observe-then-read order could reserve after this live count crossed
	 * the threshold, concatenating 16,288 pre-gap bytes with 32 post-gap bytes.
	 * Reservation now uses only the deliberately earlier snapshot. */
	assert(post_gap_live_count >= payload);
	assert(!edge_transport_snapshot_can_reserve(
		pre_gap_snapshot, 0, payload, false));

	/* On the next pass, M0's recovery publication is visible before the count
	 * containing post-gap bytes. Observation quarantines that snapshot. This
	 * must also short-circuit if discard advanced beyond the older snapshot. */
	assert(!edge_transport_snapshot_can_reserve(
		post_gap_live_count, 0, payload, true));
	assert(!edge_transport_snapshot_can_reserve(
		pre_gap_snapshot, post_gap_live_count, payload, true));

	/* A full payload accumulated after the discard boundary remains eligible. */
	assert(edge_transport_snapshot_can_reserve(
		post_gap_live_count + payload,
		post_gap_live_count,
		payload,
		false));
}

typedef struct {
	edge_transport_state_t transport;
	uint32_t dma_pending;
	uint32_t dma_remaining;
	bool irq_enabled;
	bool channel_enabled;
	bool peripheral_tc;
	bool nvic_pending;
	unsigned int chunks_started;
} modeled_dma_t;

static void modeled_dma_start(modeled_dma_t* model)
{
	*model = (modeled_dma_t){0};
	edge_transport_reset(&model->transport, 30);
	assert(edge_transport_begin_dma(&model->transport, 0, 30));
	model->dma_pending = 64;
	model->dma_remaining = 96;
	model->irq_enabled = true;
	model->channel_enabled = true;
	model->chunks_started = 1;
}

static void modeled_dma_service_irq(modeled_dma_t* model)
{
	if (!model->irq_enabled || !model->nvic_pending) {
		return;
	}
	model->channel_enabled = false;
	model->peripheral_tc = false;
	model->nvic_pending = false;
	const uint32_t completed = model->dma_pending;
	model->dma_pending = 0;
	if (!edge_transport_dma_owned(&model->transport)) {
		return;
	}
	assert(edge_transport_dma_chunk_valid(
		&model->transport, completed, model->dma_remaining));
	model->dma_remaining -= completed;
	if (model->dma_remaining != 0) {
		model->dma_pending = model->dma_remaining;
		model->channel_enabled = true;
		model->chunks_started++;
	} else {
		assert(edge_transport_finish_dma(&model->transport, NULL));
	}
}

static void modeled_dma_raise_tc(modeled_dma_t* model)
{
	model->peripheral_tc = true;
	model->nvic_pending = true;
	modeled_dma_service_irq(model);
}

static void modeled_dma_cancel(
	modeled_dma_t* model,
	unsigned int pending_tc_point)
{
	/* Mirror edge_cancel_dma() one operation at a time and inject a terminal
	 * count at each logical boundary after the interrupt is masked. */
	model->irq_enabled = false;
	if (pending_tc_point == 1) {
		modeled_dma_raise_tc(model);
	}
	model->channel_enabled = false;
	if (pending_tc_point == 2) {
		modeled_dma_raise_tc(model);
	}
	model->peripheral_tc = false;
	if (pending_tc_point == 3) {
		modeled_dma_raise_tc(model);
	}
	model->dma_pending = 0;
	if (pending_tc_point == 4) {
		modeled_dma_raise_tc(model);
	}
	edge_transport_cancel_dma(&model->transport);
	if (pending_tc_point == 5) {
		modeled_dma_raise_tc(model);
	}
	model->nvic_pending = false;
	if (pending_tc_point == 6) {
		modeled_dma_raise_tc(model);
	}
	model->irq_enabled = true;
	modeled_dma_service_irq(model);
}

static void test_stop_cancel_interrupt_ordering(void)
{
	modeled_dma_t model;

	/* A first, wrapping-chunk terminal count immediately before the mask may
	 * start the second chunk. Cancellation must stop that second chunk and
	 * release the still-DMA-owned slot without starting a third. */
	modeled_dma_start(&model);
	modeled_dma_raise_tc(&model);
	assert(model.chunks_started == 2);
	assert(model.dma_remaining == 32);
	modeled_dma_cancel(&model, 0);
	assert(model.chunks_started == 2);
	assert(model.transport.slots[0].state == EDGE_TRANSPORT_SLOT_FREE);
	assert(edge_transport_invariant(&model.transport));

	/* A final-chunk terminal count immediately before the mask owns completion
	 * and leaves READY. The stop ownership reset, after endpoint flush, then
	 * invalidates that unscheduled frame. */
	modeled_dma_start(&model);
	modeled_dma_raise_tc(&model);
	modeled_dma_raise_tc(&model);
	assert(model.transport.slots[0].state == EDGE_TRANSPORT_SLOT_READY);
	modeled_dma_cancel(&model, 0);
	assert(model.transport.slots[0].state == EDGE_TRANSPORT_SLOT_READY);
	edge_transport_reset(&model.transport, 30);
	assert(model.transport.slots[0].state == EDGE_TRANSPORT_SLOT_FREE);
	assert(edge_transport_invariant(&model.transport));

	/* A TC pending at every boundary after the mask cannot service the ISR or
	 * restart a wrapped chunk. The peripheral and NVIC clears drain it before
	 * re-enable; even an impossible late assertion sees no DMA ownership. */
	for (unsigned int interrupt_point = 1; interrupt_point <= 6;
	     ++interrupt_point) {
		modeled_dma_start(&model);
		modeled_dma_cancel(&model, interrupt_point);
		assert(model.chunks_started == 1);
		assert(model.transport.slots[0].state == EDGE_TRANSPORT_SLOT_FREE);
		assert(model.transport.dma_slot == EDGE_TRANSPORT_SLOT_NONE);
		assert(edge_transport_invariant(&model.transport));
	}
}

static void test_invalid_operations_preserve_invariants(void)
{
	edge_transport_state_t transport = {0};
	edge_transport_reset(&transport, 44);

	assert(!edge_transport_begin_dma(&transport, 1, 44));
	assert(!edge_transport_finish_dma(&transport, NULL));
	assert(!edge_transport_begin_usb(&transport, 0));
	assert(!edge_transport_complete_usb(&transport, 0, NULL, NULL));
	edge_transport_usb_schedule_result(&transport, 1, true);
	edge_transport_cancel_dma(&transport);
	assert(edge_transport_invariant(&transport));
}

int main(void)
{
	test_static_initializer_is_legacy_safe();
	test_legacy_first_rx_tx_shared_isr();
	test_framed_dma_chunk_boundaries();
	test_invalid_dma_completion_is_abandoned();
	test_pipeline_and_queue_ownership();
	test_schedule_failure_is_retryable();
	test_stale_completion_cannot_release_reused_slot();
	test_stale_completion_cannot_release_same_epoch_slot_reuse();
	test_reset_invalidates_usb_ownership_and_preserves_nonce();
	test_dma_abandon_preserves_other_usb_slot();
	test_completion_serial_wrap_skips_null_token();
	test_pre_observe_snapshot_closes_gap_threshold_race();
	test_stop_cancel_interrupt_ordering();
	test_invalid_operations_preserve_invariants();
	return 0;
}
