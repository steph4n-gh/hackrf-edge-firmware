/*
 * Copyright (c) 2026 HackRF Edge contributors
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "hackrf.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_DEVICE_COUNT 4
#define HARDWARE_SERIAL   "000000000000000087c867dc2985255f"
#define IAP_REQUEST       18

#define CHECK(condition)                                     \
	do {                                                 \
		if (!(condition)) {                          \
			fprintf(stderr,                      \
				"%s:%d: check failed: %s\n", \
				__FILE__,                    \
				__LINE__,                    \
				#condition);                 \
			exit(1);                             \
		}                                            \
	} while (0)

/* hackrf_open_usb is intentionally internal to libhackrf. */
extern libusb_device_handle* hackrf_open_usb(const char* desired_serial_number);

typedef struct {
	int descriptor_result;
	struct libusb_device_descriptor descriptor;
	int open_result;
	const char* descriptor_serial;
	int string_result;
	int iap_result;
	uint32_t serial_words[4];
	int descriptor_calls;
	int open_calls;
	int string_calls;
	int string_capacity;
	int iap_calls;
	int close_calls;
} mock_device_t;

static mock_device_t mock_devices[MOCK_DEVICE_COUNT];
static unsigned char mock_device_tokens[MOCK_DEVICE_COUNT];
static unsigned char mock_handle_tokens[MOCK_DEVICE_COUNT];
static libusb_device* mock_device_list[MOCK_DEVICE_COUNT + 1];
static ssize_t mock_list_result;
static int mock_list_is_nonnull;
static int mock_get_list_calls;
static int mock_free_list_calls;
static int mock_free_list_unref_devices;

static libusb_device* mock_device_pointer(int index)
{
	CHECK(index >= 0 && index < MOCK_DEVICE_COUNT);
	return (libusb_device*) &mock_device_tokens[index];
}

static libusb_device_handle* mock_handle_pointer(int index)
{
	CHECK(index >= 0 && index < MOCK_DEVICE_COUNT);
	return (libusb_device_handle*) &mock_handle_tokens[index];
}

static int mock_device_index(libusb_device* device)
{
	int i;

	for (i = 0; i < MOCK_DEVICE_COUNT; i++) {
		if (device == mock_device_pointer(i)) {
			return i;
		}
	}
	CHECK(0);
	return -1;
}

static int mock_handle_index(libusb_device_handle* handle)
{
	int i;

	for (i = 0; i < MOCK_DEVICE_COUNT; i++) {
		if (handle == mock_handle_pointer(i)) {
			return i;
		}
	}
	CHECK(0);
	return -1;
}

static void write_le32(unsigned char* output, uint32_t value)
{
	output[0] = (unsigned char) (value & 0xffu);
	output[1] = (unsigned char) ((value >> 8u) & 0xffu);
	output[2] = (unsigned char) ((value >> 16u) & 0xffu);
	output[3] = (unsigned char) ((value >> 24u) & 0xffu);
}

static void set_serial_words(mock_device_t* device, uint32_t final_word)
{
	device->serial_words[0] = 0x00000000u;
	device->serial_words[1] = 0x00000000u;
	device->serial_words[2] = 0x87c867dcu;
	device->serial_words[3] = final_word;
}

static void reset_mocks(int device_count)
{
	int i;

	CHECK(device_count >= 0 && device_count <= MOCK_DEVICE_COUNT);
	memset(mock_devices, 0, sizeof(mock_devices));
	memset(mock_device_list, 0, sizeof(mock_device_list));
	mock_list_result = device_count;
	mock_list_is_nonnull = 1;
	mock_get_list_calls = 0;
	mock_free_list_calls = 0;
	mock_free_list_unref_devices = -1;

	for (i = 0; i < device_count; i++) {
		mock_devices[i].descriptor_result = LIBUSB_SUCCESS;
		mock_devices[i].descriptor.idVendor = 0x1d50;
		mock_devices[i].descriptor.idProduct = 0x6089;
		mock_devices[i].descriptor.iSerialNumber = 1;
		mock_devices[i].open_result = LIBUSB_SUCCESS;
		mock_devices[i].descriptor_serial = "RunningFromRAM";
		mock_devices[i].string_result = 14;
		mock_devices[i].iap_result = (int) sizeof(read_partid_serialno_t);
		set_serial_words(&mock_devices[i], 0x2985255fu);
		mock_device_list[i] = mock_device_pointer(i);
	}
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context* context, libusb_device*** list)
{
	(void) context;
	CHECK(list != NULL);
	mock_get_list_calls++;
	*list = mock_list_is_nonnull ? mock_device_list : NULL;
	return mock_list_result;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device** list, int unref_devices)
{
	CHECK(list == mock_device_list);
	mock_free_list_calls++;
	mock_free_list_unref_devices = unref_devices;
}

int LIBUSB_CALL libusb_get_device_descriptor(
	libusb_device* device,
	struct libusb_device_descriptor* descriptor)
{
	const int index = mock_device_index(device);
	mock_device_t* mock = &mock_devices[index];

	mock->descriptor_calls++;
	if (mock->descriptor_result == LIBUSB_SUCCESS) {
		CHECK(descriptor != NULL);
		*descriptor = mock->descriptor;
	}
	return mock->descriptor_result;
}

int LIBUSB_CALL libusb_open(libusb_device* device, libusb_device_handle** device_handle)
{
	const int index = mock_device_index(device);
	mock_device_t* mock = &mock_devices[index];

	CHECK(device_handle != NULL);
	mock->open_calls++;
	if (mock->open_result == LIBUSB_SUCCESS) {
		*device_handle = mock_handle_pointer(index);
	}
	return mock->open_result;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* device_handle)
{
	const int index = mock_handle_index(device_handle);
	mock_devices[index].close_calls++;
}

int LIBUSB_CALL libusb_get_string_descriptor_ascii(
	libusb_device_handle* device_handle,
	uint8_t descriptor_index,
	unsigned char* data,
	int length)
{
	const int index = mock_handle_index(device_handle);
	mock_device_t* mock = &mock_devices[index];
	int copy_length;

	CHECK(descriptor_index == mock->descriptor.iSerialNumber);
	CHECK(data != NULL);
	mock->string_calls++;
	mock->string_capacity = length;
	if (mock->string_result < 0) {
		return mock->string_result;
	}

	copy_length = mock->string_result;
	if (copy_length > length) {
		copy_length = length;
	}
	if (copy_length > 0) {
		CHECK(mock->descriptor_serial != NULL);
		memcpy(data, mock->descriptor_serial, (size_t) copy_length);
	}
	return mock->string_result;
}

int LIBUSB_CALL libusb_control_transfer(
	libusb_device_handle* device_handle,
	uint8_t request_type,
	uint8_t request,
	uint16_t value,
	uint16_t index,
	unsigned char* data,
	uint16_t length,
	unsigned int timeout)
{
	const int device_index = mock_handle_index(device_handle);
	mock_device_t* mock = &mock_devices[device_index];
	unsigned char response[sizeof(read_partid_serialno_t)];
	int copy_length;
	int i;

	CHECK(request_type ==
	      (LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
	       LIBUSB_RECIPIENT_DEVICE));
	CHECK(request == IAP_REQUEST);
	CHECK(value == 0);
	CHECK(index == 0);
	CHECK(data != NULL);
	CHECK(length == sizeof(read_partid_serialno_t));
	CHECK(timeout == 100);
	mock->iap_calls++;
	if (mock->iap_result < 0) {
		return mock->iap_result;
	}

	memset(response, 0, sizeof(response));
	for (i = 0; i < 4; i++) {
		write_le32(response + 8 + (i * 4), mock->serial_words[i]);
	}
	copy_length = mock->iap_result;
	if (copy_length > (int) sizeof(response)) {
		copy_length = (int) sizeof(response);
	}
	if (copy_length > 0) {
		memcpy(data, response, (size_t) copy_length);
	}
	return mock->iap_result;
}

static void test_device_list_error_and_null_free(void)
{
	hackrf_device_list_t* list;

	reset_mocks(0);
	mock_list_result = LIBUSB_ERROR_IO;
	list = hackrf_device_list();
	CHECK(list == NULL);
	CHECK(mock_get_list_calls == 1);
	CHECK(mock_free_list_calls == 1);
	CHECK(mock_free_list_unref_devices == 1);

	hackrf_device_list_free(NULL);
	CHECK(mock_free_list_calls == 1);
}

static void test_zero_device_list_lifetime(void)
{
	hackrf_device_list_t* list;

	reset_mocks(0);
	list = hackrf_device_list();
	CHECK(list != NULL);
	CHECK(list->devicecount == 0);
	CHECK(list->usb_devicecount == 0);
	CHECK(list->serial_numbers == NULL);
	CHECK(list->usb_board_ids == NULL);
	CHECK(list->usb_device_index == NULL);
	CHECK(mock_free_list_calls == 0);
	hackrf_device_list_free(list);
	CHECK(mock_free_list_calls == 1);
	CHECK(mock_free_list_unref_devices == 1);
}

static void test_exact_serial_enumeration_uses_33_byte_capacity(void)
{
	hackrf_device_list_t* list;

	reset_mocks(1);
	mock_devices[0].descriptor_serial = HARDWARE_SERIAL;
	mock_devices[0].string_result = 32;
	list = hackrf_device_list();
	CHECK(list != NULL);
	CHECK(list->devicecount == 1);
	CHECK(list->usb_devicecount == 1);
	CHECK(list->serial_numbers[0] != NULL);
	CHECK(strcmp(list->serial_numbers[0], HARDWARE_SERIAL) == 0);
	CHECK(strlen(list->serial_numbers[0]) == 32u);
	CHECK(mock_devices[0].string_capacity == 33);
	CHECK(mock_devices[0].open_calls == 1);
	CHECK(mock_devices[0].close_calls == 1);
	hackrf_device_list_free(list);
	CHECK(mock_free_list_calls == 1);
}

static void test_device_list_skips_failed_descriptor(void)
{
	hackrf_device_list_t* list;

	reset_mocks(2);
	mock_devices[0].descriptor_result = LIBUSB_ERROR_IO;
	mock_devices[1].descriptor_serial = HARDWARE_SERIAL;
	mock_devices[1].string_result = 32;
	list = hackrf_device_list();
	CHECK(list != NULL);
	CHECK(list->devicecount == 1);
	CHECK(list->usb_device_index[0] == 1);
	CHECK(strcmp(list->serial_numbers[0], HARDWARE_SERIAL) == 0);
	CHECK(mock_devices[0].open_calls == 0);
	CHECK(mock_devices[1].open_calls == 1);
	CHECK(mock_devices[1].close_calls == 1);
	hackrf_device_list_free(list);
	CHECK(mock_free_list_calls == 1);
}

static void test_open_list_error_frees_returned_list(void)
{
	libusb_device_handle* handle;

	reset_mocks(0);
	mock_list_result = LIBUSB_ERROR_IO;
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == NULL);
	CHECK(mock_get_list_calls == 1);
	CHECK(mock_free_list_calls == 1);
	CHECK(mock_free_list_unref_devices == 1);
}

static void test_open_skips_failed_descriptor(void)
{
	libusb_device_handle* handle;

	reset_mocks(2);
	mock_devices[0].descriptor_result = LIBUSB_ERROR_IO;
	mock_devices[1].descriptor_serial = HARDWARE_SERIAL;
	mock_devices[1].string_result = 32;
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(1));
	CHECK(mock_devices[0].open_calls == 0);
	CHECK(mock_devices[1].open_calls == 1);
	CHECK(mock_devices[1].iap_calls == 0);
	CHECK(mock_devices[1].string_capacity == 33);
	CHECK(mock_free_list_calls == 1);
	libusb_close(handle);
	CHECK(mock_devices[1].close_calls == 1);
}

static void test_open_failure_continues_without_closing_invalid_handle(void)
{
	libusb_device_handle* handle;

	reset_mocks(2);
	mock_devices[0].open_result = LIBUSB_ERROR_ACCESS;
	mock_devices[1].descriptor_serial = HARDWARE_SERIAL;
	mock_devices[1].string_result = 32;
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(1));
	CHECK(mock_devices[0].open_calls == 1);
	CHECK(mock_devices[0].string_calls == 0);
	CHECK(mock_devices[0].iap_calls == 0);
	CHECK(mock_devices[0].close_calls == 0);
	CHECK(mock_devices[1].open_calls == 1);
	CHECK(mock_devices[1].close_calls == 0);
	CHECK(mock_free_list_calls == 1);
	libusb_close(handle);
	CHECK(mock_devices[1].close_calls == 1);
}

static void test_running_from_ram_falls_back_to_iap(void)
{
	libusb_device_handle* handle;

	reset_mocks(1);
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(0));
	CHECK(mock_devices[0].string_calls == 1);
	CHECK(mock_devices[0].string_capacity == 33);
	CHECK(mock_devices[0].iap_calls == 1);
	CHECK(mock_devices[0].close_calls == 0);
	CHECK(mock_free_list_calls == 1);
	libusb_close(handle);
	CHECK(mock_devices[0].close_calls == 1);
}

static void test_negative_descriptor_result_falls_back_safely(void)
{
	libusb_device_handle* handle;

	reset_mocks(1);
	mock_devices[0].string_result = LIBUSB_ERROR_IO;
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(0));
	CHECK(mock_devices[0].string_calls == 1);
	CHECK(mock_devices[0].iap_calls == 1);
	CHECK(mock_devices[0].close_calls == 0);
	libusb_close(handle);
}

static void test_zero_length_descriptor_falls_back_to_iap(void)
{
	libusb_device_handle* handle;

	reset_mocks(1);
	mock_devices[0].descriptor_serial = "";
	mock_devices[0].string_result = 0;
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(0));
	CHECK(mock_devices[0].string_calls == 1);
	CHECK(mock_devices[0].iap_calls == 1);
	CHECK(mock_devices[0].close_calls == 0);
	libusb_close(handle);
}

static void test_short_descriptor_and_nonmatching_iap_close_handle(void)
{
	libusb_device_handle* handle;

	reset_mocks(1);
	mock_devices[0].descriptor_serial = "RAM";
	mock_devices[0].string_result = 3;
	set_serial_words(&mock_devices[0], 0x11111111u);
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == NULL);
	CHECK(mock_devices[0].string_calls == 1);
	CHECK(mock_devices[0].iap_calls == 1);
	CHECK(mock_devices[0].close_calls == 1);
	CHECK(mock_free_list_calls == 1);
}

static void test_iap_transfer_failures_close_handle(void)
{
	static const int transfer_results[] = {LIBUSB_ERROR_PIPE, 0, 23};
	libusb_device_handle* handle;
	size_t i;

	for (i = 0u; i < sizeof(transfer_results) / sizeof(transfer_results[0]); i++) {
		reset_mocks(1);
		mock_devices[0].iap_result = transfer_results[i];
		handle = hackrf_open_usb(HARDWARE_SERIAL);
		CHECK(handle == NULL);
		CHECK(mock_devices[0].iap_calls == 1);
		CHECK(mock_devices[0].close_calls == 1);
		CHECK(mock_free_list_calls == 1);
	}
}

static void test_multi_device_closes_only_nonmatch(void)
{
	libusb_device_handle* handle;

	reset_mocks(2);
	set_serial_words(&mock_devices[0], 0x11111111u);
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(1));
	CHECK(mock_devices[0].open_calls == 1);
	CHECK(mock_devices[0].iap_calls == 1);
	CHECK(mock_devices[0].close_calls == 1);
	CHECK(mock_devices[1].open_calls == 1);
	CHECK(mock_devices[1].iap_calls == 1);
	CHECK(mock_devices[1].close_calls == 0);
	CHECK(mock_free_list_calls == 1);
	libusb_close(handle);
	CHECK(mock_devices[1].close_calls == 1);
}

static void test_descriptor_match_avoids_iap_transfer(void)
{
	libusb_device_handle* handle;

	reset_mocks(1);
	mock_devices[0].descriptor_serial = HARDWARE_SERIAL;
	mock_devices[0].string_result = 32;
	mock_devices[0].iap_result = LIBUSB_ERROR_PIPE;
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(0));
	CHECK(mock_devices[0].string_capacity == 33);
	CHECK(mock_devices[0].iap_calls == 0);
	CHECK(mock_devices[0].close_calls == 0);
	libusb_close(handle);
}

static void test_missing_descriptor_uses_iap(void)
{
	libusb_device_handle* handle;

	reset_mocks(1);
	mock_devices[0].descriptor.iSerialNumber = 0;
	handle = hackrf_open_usb(HARDWARE_SERIAL);
	CHECK(handle == mock_handle_pointer(0));
	CHECK(mock_devices[0].string_calls == 0);
	CHECK(mock_devices[0].iap_calls == 1);
	CHECK(mock_devices[0].close_calls == 0);
	libusb_close(handle);
}

int main(void)
{
	test_device_list_error_and_null_free();
	test_zero_device_list_lifetime();
	test_exact_serial_enumeration_uses_33_byte_capacity();
	test_device_list_skips_failed_descriptor();
	test_open_list_error_frees_returned_list();
	test_open_skips_failed_descriptor();
	test_open_failure_continues_without_closing_invalid_handle();
	test_running_from_ram_falls_back_to_iap();
	test_negative_descriptor_result_falls_back_safely();
	test_zero_length_descriptor_falls_back_to_iap();
	test_short_descriptor_and_nonmatching_iap_close_handle();
	test_iap_transfer_failures_close_handle();
	test_multi_device_closes_only_nonmatch();
	test_descriptor_match_avoids_iap_transfer();
	test_missing_descriptor_uses_iap();
	return 0;
}
