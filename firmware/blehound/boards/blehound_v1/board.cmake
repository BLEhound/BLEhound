# BLEhound board definition, derived from the Zephyr nRF54LM20 DK board
# (boards/nordic/nrf54lm20dk, Copyright (c) 2025 Nordic Semiconductor ASA).
# SPDX-License-Identifier: Apache-2.0

board_runner_args(jlink "--device=nRF54LM20A_M33" "--speed=4000")

# J-Link only: nrfutil-device does not recognise the nRF54LM20A engineering samples
# ("Unknown part 0x33"), and the project does not depend on nrfutil anywhere else.
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
