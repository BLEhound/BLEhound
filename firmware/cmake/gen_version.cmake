# Generates blehound_version.h: V<base>.<git commit count>. The file is only written when its
# content changes, so a rebuild is not forced every time (copy_if_different compares contents).
execute_process(
    COMMAND git -C ${SRC_DIR} rev-list --count HEAD
    OUTPUT_VARIABLE BUILD_NUM
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RC
)
if(NOT GIT_RC EQUAL 0 OR BUILD_NUM STREQUAL "")
    set(BUILD_NUM "0")
endif()
set(BLEHOUND_FW_VERSION "${BASE}.${BUILD_NUM}")
file(WRITE ${OUT}.in "#define BLEHOUND_FW_VERSION \"${BLEHOUND_FW_VERSION}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT}.in ${OUT})
