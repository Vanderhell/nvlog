cmake_minimum_required(VERSION 3.15)
project(nvlog C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ── core library ──────────────────────────────────────────────────
add_library(nvlog STATIC
    src/nvlog.c
    src/nvlog_hal_flash.c
)
target_include_directories(nvlog PUBLIC include)

# ── POSIX backend (host/test) ─────────────────────────────────────
add_library(nvlog_posix STATIC backends/nvlog_posix.c)
target_include_directories(nvlog_posix PUBLIC include backends)
target_link_libraries(nvlog_posix nvlog)

# ── NOR flash simulator (host/test) ───────────────────────────────
add_library(nvlog_flash_sim STATIC backends/nvlog_flash_sim.c)
target_include_directories(nvlog_flash_sim PUBLIC include backends)
target_link_libraries(nvlog_flash_sim nvlog)

# ── tests ─────────────────────────────────────────────────────────
add_executable(test_nvlog      tests/test_nvlog.c)
add_executable(test_powerloss  tests/test_powerloss.c)
add_executable(test_flash      tests/test_flash.c)
add_executable(test_ring       tests/test_ring.c)
add_executable(test_wire       tests/test_wire.c)

foreach(t test_nvlog test_powerloss test_ring)
    target_include_directories(${t} PRIVATE include backends)
    target_link_libraries(${t} nvlog nvlog_posix)
endforeach()

target_include_directories(test_wire PRIVATE include)

target_include_directories(test_flash PRIVATE include backends)
target_link_libraries(test_flash nvlog nvlog_flash_sim)

enable_testing()
add_test(NAME suite_v01 COMMAND test_nvlog)
add_test(NAME suite_v02 COMMAND test_powerloss)
add_test(NAME suite_v03 COMMAND test_flash)
add_test(NAME suite_v04 COMMAND test_ring)
add_test(NAME suite_v05 COMMAND test_wire)

set_tests_properties(suite_v01 suite_v02 suite_v03 suite_v04 suite_v05
    PROPERTIES TIMEOUT 30)
