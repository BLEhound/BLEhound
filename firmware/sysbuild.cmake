# Extra sysbuild-stage configuration (goes with sysbuild.conf).
#
# The version in the MCUboot image header (imgtool --version, format major.minor.patch+build) follows the
# same rule as the V1.0.0.<git commit count> the firmware reports: base from BLEHOUND_VERSION_BASE in
# CMakeLists.txt, build = commit count of this repo. Under sysbuild a per-image CONFIG_* override can only
# be injected from here (the sysbuild cache); setting a cache variable in the app's own CMakeLists has no
# effect. The commit count is read once at configure time; to refresh the version after committing, CMake
# has to re-run (automatic when a CMake file changed, otherwise west build --cmake).
cmake_minimum_required(VERSION 3.20.0)

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt _blehound_base_line
     REGEX "^set\\(BLEHOUND_VERSION_BASE \"V[0-9]+\\.[0-9]+\\.[0-9]+\"\\)")
string(REGEX REPLACE ".*\"V([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1" _blehound_base "${_blehound_base_line}")
if(NOT _blehound_base MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "sysbuild.cmake: cannot read BLEHOUND_VERSION_BASE from CMakeLists.txt")
endif()

execute_process(
  COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} rev-list --count HEAD
  OUTPUT_VARIABLE _blehound_build_num
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE _blehound_git_rc
)
if(NOT _blehound_git_rc EQUAL 0 OR _blehound_build_num STREQUAL "")
  set(_blehound_build_num "0")
endif()

set(BLEHOUND_IMAGE_VERSION "${_blehound_base}+${_blehound_build_num}")
message(STATUS "BLEhound: MCUboot image version ${BLEHOUND_IMAGE_VERSION}")
set_config_string(${DEFAULT_IMAGE} CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION "${BLEHOUND_IMAGE_VERSION}")

# ---- Fixed-name output directory build_dongle/blehound/ ----
# The three images all produce zephyr.* in their own subdirectories; copy them under blehound/ with stable
# names for distribution and for the scripts:
#   blehound_mcu_boot.{bin,hex,elf,map}  MCUboot
#   blehound_loader.{bin,hex,elf,map}    firmware loader (bin/hex are the signed image)
#   blehound_app.{bin,hex,elf,map}       application (bin/hex are the signed image, programmed at 0x23000)
#   blehound_ota.bin / blehound_ota.zip  signed app image for OTA/DFU upload (= blehound_app.bin) and the DFU package
#   blehound_merged.hex                  whole-chip image (MCUboot + loader + app) for J-Link programming
string(REGEX REPLACE "^/" "" _blehound_quals "${BOARD_QUALIFIERS}")
set(_blehound_bt "${BOARD}")
if(_blehound_quals)
  string(APPEND _blehound_bt "/${_blehound_quals}")
endif()
string(REPLACE "/" "_" _blehound_bt "${_blehound_bt}")
string(REPLACE "." "_" _blehound_bt "${_blehound_bt}")
set(_blehound_out ${CMAKE_BINARY_DIR}/blehound)
set(_blehound_merged ${CMAKE_BINARY_DIR}/merged_${_blehound_bt}.hex)
set(_blehound_loader ${SB_CONFIG_FIRMWARE_LOADER_IMAGE_NAME})
set(_blehound_copies)
foreach(pair
    "mcuboot/zephyr/zephyr.bin|blehound_mcu_boot.bin"
    "mcuboot/zephyr/zephyr.hex|blehound_mcu_boot.hex"
    "mcuboot/zephyr/zephyr.elf|blehound_mcu_boot.elf"
    "mcuboot/zephyr/zephyr.map|blehound_mcu_boot.map"
    "${_blehound_loader}/zephyr/zephyr.signed.bin|blehound_loader.bin"
    "${_blehound_loader}/zephyr/zephyr.signed.hex|blehound_loader.hex"
    "${_blehound_loader}/zephyr/zephyr.elf|blehound_loader.elf"
    "${_blehound_loader}/zephyr/zephyr.map|blehound_loader.map"
    "${DEFAULT_IMAGE}/zephyr/zephyr.signed.bin|blehound_app.bin"
    "${DEFAULT_IMAGE}/zephyr/zephyr.signed.hex|blehound_app.hex"
    "${DEFAULT_IMAGE}/zephyr/zephyr.elf|blehound_app.elf"
    "${DEFAULT_IMAGE}/zephyr/zephyr.map|blehound_app.map"
    "${DEFAULT_IMAGE}/zephyr/zephyr.signed.bin|blehound_ota.bin"
    "dfu_application.zip|blehound_ota.zip"
    "merged_${_blehound_bt}.hex|blehound_merged.hex")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 _src)
  list(GET pair 1 _dst)
  list(APPEND _blehound_copies COMMAND ${CMAKE_COMMAND} -E copy_if_different
       ${CMAKE_BINARY_DIR}/${_src} ${_blehound_out}/${_dst})
endforeach()
add_custom_target(blehound_outputs ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_blehound_out}
  ${_blehound_copies}
  DEPENDS ${_blehound_merged} ${CMAKE_BINARY_DIR}/dfu_application.zip
  COMMENT "Collecting BLEhound outputs into ${_blehound_out}"
)
add_dependencies(blehound_outputs mcuboot ${_blehound_loader} ${DEFAULT_IMAGE})
