# SPDX-License-Identifier: Apache-2.0
#
# BLEhound Sniffer firmware top-level CMake
# Note: this is a Zephyr / nRF Connect SDK "freestanding" application.
#   - The build depends on an external NCS (provided via west); this repo does not contain the SDK itself.
#   - For local builds see tools/build.sh; for open-source reproduction see west.yml + README in the repo root.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf_ble_sniffer LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/cobs.c
    src/host_iface.c
    src/ble_csa.c
    src/ble_ctrl_pdu.c
    src/conn_follower.c
    src/board_role.c
    src/peer_msg.c
    src/peer_txq.c
    src/sync_line.c
    src/peer_link.c
    src/tri_coord.c
    src/fem_ctrl.c
    src/ble_aes128.c
)

# The Radio HAL implementation is selected per chip -- this is the whole point of the HAL abstraction:
# the upper layers (CSA / connection following / host protocol) stay completely unchanged, and switching
# chips only swaps this one file. To add a new chip, just add another branch here.
if(CONFIG_SOC_SERIES_NRF54L)
    target_sources(app PRIVATE src/radio_nrf54l.c)
else()
    target_sources(app PRIVATE src/radio_nrf52.c)   # nRF52 series (default)
endif()

target_include_directories(app PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/blehound_generated
)

# Firmware version: V<MAJOR>.<MINOR>.<PATCH>.<BUILD>, BUILD = git commit count of this repo,
# incremented with every commit. The generator runs on every build so a rebuild after a commit
# picks it up; outside a git repo (e.g. an exported source archive) it falls back to 0.
set(BLEHOUND_VERSION_BASE "V1.0.0")
add_custom_target(blehound_version
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/blehound_generated/blehound_version.h
    COMMAND ${CMAKE_COMMAND}
        -DBASE=${BLEHOUND_VERSION_BASE}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/blehound_generated/blehound_version.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_version.cmake
    COMMENT "Generating firmware version (V1.0.0.<git commit count>)"
    VERBATIM
)
add_dependencies(app blehound_version)

# nRF54L USB uses the new UDC/usbd stack, reusing Zephyr's official USB init helper
# (sample_usbd_init.c). common.cmake only adds that source file when CONFIG_USB_DEVICE_STACK_NEXT
# is set, so it is a no-op for nRF52 (legacy stack) builds.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
