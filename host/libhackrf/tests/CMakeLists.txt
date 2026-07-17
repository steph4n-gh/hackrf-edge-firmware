# SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(
  hackrf_edge_test
  test_hackrf_edge.c
  ../src/hackrf_edge.c)

target_compile_features(hackrf_edge_test PRIVATE c_std_90)
target_include_directories(hackrf_edge_test PRIVATE ../src)
if(MSVC)
  target_compile_options(hackrf_edge_test PRIVATE /W4 /WX)
else()
  target_compile_options(hackrf_edge_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME hackrf_edge_protocol COMMAND hackrf_edge_test)

add_executable(
  hackrf_serial_test
  test_serial_matching.c
  ../src/hackrf_serial.c)

target_compile_features(hackrf_serial_test PRIVATE c_std_90)
target_include_directories(hackrf_serial_test PRIVATE ../src)
if(MSVC)
  target_compile_options(hackrf_serial_test PRIVATE /W4 /WX)
else()
  target_compile_options(hackrf_serial_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME hackrf_serial_matching COMMAND hackrf_serial_test)

add_executable(hackrf_spiflash_geometry_test test_spiflash_geometry.c)
target_compile_features(hackrf_spiflash_geometry_test PRIVATE c_std_90)
target_include_directories(
  hackrf_spiflash_geometry_test
  PRIVATE ../src ../../hackrf-tools/src)
if(MSVC)
  target_compile_options(hackrf_spiflash_geometry_test PRIVATE /W4 /WX)
else()
  target_compile_options(
    hackrf_spiflash_geometry_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME hackrf_spiflash_geometry COMMAND hackrf_spiflash_geometry_test)

# Compile-time routing keeps mocked discovery independent of whether the
# selected libusb package is a shared or static library. Windows import
# declarations do not provide the same portable override seam.
if(NOT WIN32)
  add_executable(
    hackrf_usb_selection_test
    test_usb_selection.c
    ../src/hackrf.c
    ../src/hackrf_edge.c
    ../src/hackrf_serial.c)
  target_compile_features(hackrf_usb_selection_test PRIVATE c_std_90)
  target_compile_definitions(
    hackrf_usb_selection_test
    PRIVATE
      libusb_close=hackrf_test_libusb_close
      libusb_control_transfer=hackrf_test_libusb_control_transfer
      libusb_free_device_list=hackrf_test_libusb_free_device_list
      libusb_get_device_descriptor=hackrf_test_libusb_get_device_descriptor
      libusb_get_device_list=hackrf_test_libusb_get_device_list
      libusb_get_string_descriptor_ascii=hackrf_test_libusb_get_string_descriptor_ascii
      libusb_open=hackrf_test_libusb_open)
  target_include_directories(hackrf_usb_selection_test PRIVATE ../src)
  target_link_libraries(
    hackrf_usb_selection_test PRIVATE LIBUSB::LIBUSB)
  if(TARGET PThreads4W::PThreads4W)
    target_link_libraries(
      hackrf_usb_selection_test PRIVATE PThreads4W::PThreads4W)
  else()
    target_link_libraries(hackrf_usb_selection_test PRIVATE Threads::Threads)
  endif()
  set_source_files_properties(
    test_usb_selection.c
    PROPERTIES COMPILE_FLAGS "-Wall -Wextra -Werror")

  add_test(NAME hackrf_usb_selection COMMAND hackrf_usb_selection_test)
endif()

if(NOT MSVC)
  add_executable(
    hackrf_edge_firmware_test
    test_firmware_edge.c
    ../../../firmware/hackrf_usb/usb_api_edge.c)

  target_compile_features(hackrf_edge_firmware_test PRIVATE c_std_99)
  target_compile_definitions(
    hackrf_edge_firmware_test PRIVATE HACKRF_EDGE_HOST_TEST=1)
  target_include_directories(
    hackrf_edge_firmware_test
    PRIVATE firmware_edge_mocks ../../../firmware/hackrf_usb)
  target_compile_options(
    hackrf_edge_firmware_test PRIVATE -Wall -Wextra -Werror)

  add_test(NAME hackrf_edge_firmware_protocol COMMAND hackrf_edge_firmware_test)

  add_executable(
    hackrf_spiflash_firmware_test
    test_firmware_spiflash.c
    ../../../firmware/hackrf_usb/usb_api_spiflash.c)
  target_compile_features(hackrf_spiflash_firmware_test PRIVATE c_std_99)
  target_include_directories(
    hackrf_spiflash_firmware_test
    PRIVATE firmware_edge_mocks ../../../firmware/hackrf_usb)
  target_compile_options(
    hackrf_spiflash_firmware_test PRIVATE -Wall -Wextra -Werror)

  add_test(
    NAME hackrf_spiflash_firmware_bounds
    COMMAND hackrf_spiflash_firmware_test)

  add_executable(hackrf_edge_transport_test test_edge_transport.c)
  target_compile_features(hackrf_edge_transport_test PRIVATE c_std_99)
  target_include_directories(
    hackrf_edge_transport_test PRIVATE ../../../firmware/hackrf_usb)
  target_compile_options(
    hackrf_edge_transport_test PRIVATE -Wall -Wextra -Werror)

  add_test(NAME hackrf_edge_transport COMMAND hackrf_edge_transport_test)

  add_test(
    NAME hackrf_edge_m0_stop_boundary
    COMMAND
      python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_m0_stop_boundary.py
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/hackrf_usb/sgpio_m0.s)

  add_test(
    NAME hackrf_edge_configuration_loss_boundary
    COMMAND
      python3
      ${CMAKE_CURRENT_SOURCE_DIR}/test_configuration_loss_boundary.py
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/common/usb.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/hackrf_usb/hackrf_usb.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/hackrf_usb/usb_api_transceiver.c)

  add_test(
    NAME hackrf_edge_legacy_dma_ownership
    COMMAND
      python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_legacy_dma_ownership.py
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/hackrf_usb/usb_api_transceiver.c)

  add_test(
    NAME hackrf_portapack_lcd_init_contract
    COMMAND
      python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_portapack_lcd_init_contract.py
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/common/portapack.c)

  add_test(
    NAME hackrf_edge_cli_output_contract
    COMMAND
      python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_edge_cli_output_contract.py
      ${CMAKE_CURRENT_SOURCE_DIR}/../../hackrf-tools/src/hackrf_edge.c)
endif()
