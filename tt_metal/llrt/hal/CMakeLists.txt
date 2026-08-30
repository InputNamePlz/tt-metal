set(DEV_MSGS_CODEGEN_PY ${CMAKE_CURRENT_SOURCE_DIR}/codegen/codegen.py)
set(DEV_MSGS_CODEGEN_SH ${CMAKE_CURRENT_SOURCE_DIR}/codegen/codegen.sh)
set(HAL_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)

# On Windows hosts a shell script is not directly executable; run it through bash
# (e.g. the one shipped with Git for Windows).
set(DEV_MSGS_CODEGEN_COMMAND ${DEV_MSGS_CODEGEN_SH})
if(CMAKE_HOST_WIN32)
    find_program(BASH_EXECUTABLE bash REQUIRED)
    # PYTHONUTF8: codegen.py reads UTF-8 sources; don't let a non-UTF-8 ANSI
    # codepage (e.g. cp949) break it.
    set(DEV_MSGS_CODEGEN_COMMAND
        ${CMAKE_COMMAND}
        -E
        env
        PYTHONUTF8=1
        ${BASH_EXECUTABLE}
        ${DEV_MSGS_CODEGEN_SH}
    )
endif()

# ── Per-file codegen commands ──────────────────────────────────────────────────
# Split into separate add_custom_command calls so that a change to dev_msgs.h
# only regenerates dev_msgs.hpp/dev_msgs_impl.hpp, not all six generated files.
# This prevents ~4000 ccache misses per unrelated header edit.

# 1) dev_msgs
add_custom_command(
    OUTPUT
        ${HAL_GENERATED_DIR}/dev_msgs.hpp
        ${HAL_GENERATED_DIR}/dev_msgs_impl.hpp
    COMMAND
        ${DEV_MSGS_CODEGEN_COMMAND} ${PROJECT_SOURCE_DIR} ${HAL_GENERATED_DIR} 0
    DEPENDS
        ${DEV_MSGS_CODEGEN_PY}
        ${DEV_MSGS_CODEGEN_SH}
        ${PROJECT_SOURCE_DIR}/tt_metal/hw/inc/hostdev/dev_msgs.h
)

# 2) fabric_telemetry
add_custom_command(
    OUTPUT
        ${HAL_GENERATED_DIR}/fabric_telemetry.hpp
        ${HAL_GENERATED_DIR}/fabric_telemetry_impl.hpp
    COMMAND
        ${DEV_MSGS_CODEGEN_COMMAND} ${PROJECT_SOURCE_DIR} ${HAL_GENERATED_DIR} 1
    DEPENDS
        ${DEV_MSGS_CODEGEN_PY}
        ${DEV_MSGS_CODEGEN_SH}
        ${PROJECT_SOURCE_DIR}/tt_metal/hw/inc/hostdev/fabric_telemetry_msgs.h
)

# 3) realtime_profiler_msgs
add_custom_command(
    OUTPUT
        ${HAL_GENERATED_DIR}/realtime_profiler_msgs.hpp
        ${HAL_GENERATED_DIR}/realtime_profiler_msgs_impl.hpp
    COMMAND
        ${DEV_MSGS_CODEGEN_COMMAND} ${PROJECT_SOURCE_DIR} ${HAL_GENERATED_DIR} 2
    DEPENDS
        ${DEV_MSGS_CODEGEN_PY}
        ${DEV_MSGS_CODEGEN_SH}
        ${PROJECT_SOURCE_DIR}/tt_metal/hw/inc/hostdev/realtime_profiler_msgs.h
)

set(HAL_GENERATED_STRUCT_HEADERS
    ${HAL_GENERATED_DIR}/dev_msgs.hpp
    ${HAL_GENERATED_DIR}/dev_msgs_impl.hpp
    ${HAL_GENERATED_DIR}/fabric_telemetry.hpp
    ${HAL_GENERATED_DIR}/fabric_telemetry_impl.hpp
    ${HAL_GENERATED_DIR}/realtime_profiler_msgs.hpp
    ${HAL_GENERATED_DIR}/realtime_profiler_msgs_impl.hpp
)

add_custom_target(hal_generate_dev_msgs_header DEPENDS ${HAL_GENERATED_STRUCT_HEADERS})
if(TARGET all_generated_files)
    add_dependencies(all_generated_files hal_generate_dev_msgs_header)
endif()

add_subdirectory(tt-1xx)
add_subdirectory(tt-2xx)
