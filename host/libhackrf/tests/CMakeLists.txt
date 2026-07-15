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
endif()
