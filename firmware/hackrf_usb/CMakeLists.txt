# Copyright 2012 Jared Boone <jared@sharebrained.com>
# Modified for HackRF Edge in 2026 by HackRF Edge contributors.
#
# This file is part of HackRF.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

cmake_minimum_required(VERSION 3.10.0)
set(CMAKE_TOOLCHAIN_FILE ../toolchain-arm-cortex-m.cmake)

project(hackrf_usb C)

include(../hackrf-common.cmake)

option(HACKRF_EDGE_LTO "Use link-time optimization for the M4 firmware" ON)
set(HACKRF_EDGE_M4_EXTRA_FLAGS "" CACHE STRING
	"Additional size-tuning flags for HackRF M4 compilation and linking")

# common/usb.c is also built by PortaPack and other firmware. Rename its ISR
# only in HackRF USB so the non-LTO vector wrapper can call the implementation
# without changing those other targets' public interrupt symbol.
set(CFLAGS_M4
	"${CFLAGS_M4} -DUSB0_ISR_NAME=usb0_isr_impl -DHACKRF_EXTERNAL_INTERRUPT_HANDLERS=1")
set(CFLAGS_M4_RAM
	"${CFLAGS_M4_RAM} -DUSB0_ISR_NAME=usb0_isr_impl -DHACKRF_EXTERNAL_INTERRUPT_HANDLERS=1")

if(HACKRF_EDGE_M4_EXTRA_FLAGS)
	set(CFLAGS_M4 "${CFLAGS_M4} ${HACKRF_EDGE_M4_EXTRA_FLAGS}")
	set(CFLAGS_M4_RAM "${CFLAGS_M4_RAM} ${HACKRF_EDGE_M4_EXTRA_FLAGS}")
	set(LDFLAGS_M4 "${LDFLAGS_M4} ${HACKRF_EDGE_M4_EXTRA_FLAGS}")
	set(LDFLAGS_M4_RAM "${LDFLAGS_M4_RAM} ${HACKRF_EDGE_M4_EXTRA_FLAGS}")
endif()

if(HACKRF_EDGE_LTO)
	# Keep the timing-audited M0 assembly untouched. Whole-program M4 LTO
	# recovers enough flash for Edge plus the stock one-megabyte PortaPack
	# image without deleting established applications or changing the ABI.
	# The interrupt vector table lives in non-LTO libopencm3. Explicit roots
	# keep GCC's LTO plugin from resolving its weak aliases before seeing the
	# strong application handlers. check_interrupt_handlers.py verifies both
	# the symbol bindings and the final vector entries after every link.
	set(CFLAGS_M4 "${CFLAGS_M4} -flto")
	set(CFLAGS_M4_RAM "${CFLAGS_M4_RAM} -flto")
	set(EDGE_ISR_ROOTS
		"-Wl,-u,dma_isr,-u,usb0_isr,-u,sys_tick_handler,-u,hard_fault_handler,-u,mem_manage_handler,-u,bus_fault_handler,-u,usage_fault_handler")
	set(LDFLAGS_M4 "${LDFLAGS_M4} -flto ${EDGE_ISR_ROOTS}")
	set(LDFLAGS_M4_RAM "${LDFLAGS_M4_RAM} -flto ${EDGE_ISR_ROOTS}")
endif()

add_custom_command(
	OUTPUT ${PATH_HACKRF_CPLD_DATA_C}
	COMMAND ${PATH_CPLD_BITSTREAM_TOOL} --xsvf ${PATH_HACKRF_CPLD_XSVF} --hackrf-data ${PATH_HACKRF_CPLD_DATA_C}
	DEPENDS ${PATH_CPLD_BITSTREAM_TOOL} ${PATH_HACKRF_CPLD_XSVF}
)

add_custom_command(
	OUTPUT ${PATH_PRALINE_FPGA_OBJ}
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMAND ${CMAKE_COMMAND} -E copy ${PATH_PRALINE_FPGA_BIN} "fpga.bin"
	COMMAND ${CMAKE_OBJCOPY}
		-I binary
		-O elf32-littlearm
		-B arm
		--rename-section .data=.rom_only
		"fpga.bin" ${PATH_PRALINE_FPGA_OBJ}
	DEPENDS ${PATH_PRALINE_FPGA_BIN}
)

set(SRC_M4
	hackrf_usb.c
	"${PATH_HACKRF_FIRMWARE_COMMON}/tuning.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/streaming.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/usb.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/usb_request.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/usb_standard_request.c"
	usb_descriptor.c
	usb_device.c
	usb_endpoint.c
	interrupt_handlers.c
	usb_api_edge.c
	usb_api_board_info.c
	usb_api_m0_state.c
	usb_api_register.c
	usb_api_spiflash.c
	usb_api_transceiver.c
	usb_api_operacake.c
	usb_api_sweep.c
	usb_api_selftest.c
	usb_api_ui.c
	usb_api_adc.c
	"${PATH_HACKRF_FIRMWARE_COMMON}/usb_queue.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/fault_handler.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/crc.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/rom_iap.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/operacake.c"
	"${PATH_HACKRF_FIRMWARE_COMMON}/operacake_sctimer.c"
)

if(HACKRF_EDGE_LTO)
	# libopencm3's vector table refers to weak aliases from a non-LTO archive.
	# Keep tiny strong wrappers outside the LTO partition so GNU ld resolves
	# those aliases correctly; the wrappers root their LTO implementation calls.
	set_source_files_properties(
		interrupt_handlers.c PROPERTIES COMPILE_FLAGS "-fno-lto")
endif()

set(SRC_M0 sgpio_m0.s)

if(BOARD STREQUAL "UNIVERSAL")
	SET(SRC_M4
		${SRC_M4}
		usb_api_cpld.c
		"${PATH_HACKRF_FIRMWARE_COMMON}/cpld_xc2c.c"
		"${PATH_HACKRF_CPLD_DATA_C}"
		"${PATH_HACKRF_FIRMWARE_COMMON}/xapp058/lenval.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/xapp058/micro.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/xapp058/ports.c"

		"${PATH_HACKRF_FIRMWARE_COMMON}/lz4_blk.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/fpga_image.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/fpga_selftest.c"
		usb_api_praline.c
	)
	SET(OBJ_M4
		${PATH_PRALINE_FPGA_OBJ}
	)
endif()

if(BOARD STREQUAL "PRALINE")
	SET(SRC_M4
		${SRC_M4}
		"${PATH_HACKRF_FIRMWARE_COMMON}/lz4_blk.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/fpga_image.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/fpga_selftest.c"
		usb_api_praline.c
	)
	SET(OBJ_M4
		${PATH_PRALINE_FPGA_OBJ}
	)
else()
	SET(SRC_M4
		${SRC_M4}
		usb_api_cpld.c
		"${PATH_HACKRF_FIRMWARE_COMMON}/cpld_xc2c.c"
		"${PATH_HACKRF_CPLD_DATA_C}"
		"${PATH_HACKRF_FIRMWARE_COMMON}/xapp058/lenval.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/xapp058/micro.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/xapp058/ports.c"
	)
endif()

if(BOARD STREQUAL "PRALINE" OR BOARD STREQUAL "HACKRF_ONE" OR BOARD STREQUAL "UNIVERSAL")
	SET(SRC_M4
		${SRC_M4}
		"${PATH_HACKRF_FIRMWARE_COMMON}/portapack.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/ui_portapack.c"
	)
endif()

if(BOARD STREQUAL "RAD1O")
	SET(SRC_M4
		${SRC_M4}
		"${PATH_HACKRF_FIRMWARE_COMMON}/rad1o/display.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/rad1o/print.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/rad1o/render.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/rad1o/decoder.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/rad1o/smallfonts.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/rad1o/draw.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/rad1o/ubuntu18.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/picoprintf.c"
		"${PATH_HACKRF_FIRMWARE_COMMON}/ui_rad1o.c"
	)
endif()

DeclareTargets()

# A successful link is not sufficient under LTO: without explicit rooting,
# GNU ld can silently leave weak blocking/null handlers in the vector table.
# Validate all three HackRF M4 variants, including their actual vector words.
foreach(variant "" "_ram" "_dfu")
	add_custom_command(
		TARGET hackrf_usb${variant}.elf
		POST_BUILD
		COMMAND python3
			${CMAKE_CURRENT_SOURCE_DIR}/check_interrupt_handlers.py
			--objcopy ${CMAKE_OBJCOPY}
			--elf $<TARGET_FILE:hackrf_usb${variant}.elf>
		VERBATIM)
endforeach()
