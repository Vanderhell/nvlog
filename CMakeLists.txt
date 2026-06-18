cmake_minimum_required(VERSION 3.15)
project(nvlog C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

add_library(nvlog_hal_fram_example STATIC backends/hal_examples/nvlog_hal_fram.c)
target_include_directories(nvlog_hal_fram_example PUBLIC include backends/hal_examples)
target_link_libraries(nvlog_hal_fram_example nvlog)

add_library(nvlog_hal_eeprom_example STATIC backends/hal_examples/nvlog_hal_eeprom.c)
target_include_directories(nvlog_hal_eeprom_example PUBLIC include backends/hal_examples)
target_link_libraries(nvlog_hal_eeprom_example nvlog)

add_library(nvlog_hal_nor_spi_example STATIC backends/hal_examples/nvlog_hal_nor_spi.c)
target_include_directories(nvlog_hal_nor_spi_example PUBLIC include backends/hal_examples)
target_link_libraries(nvlog_hal_nor_spi_example nvlog)

add_library(nvlog_hal_stm32_f4_example STATIC backends/hal_examples/nvlog_hal_stm32_flash.c)
target_compile_definitions(nvlog_hal_stm32_f4_example PRIVATE NVLOG_STM32_FLASH_F4=1)
target_include_directories(nvlog_hal_stm32_f4_example PUBLIC include backends/hal_examples)
target_link_libraries(nvlog_hal_stm32_f4_example nvlog)

add_library(nvlog_hal_stm32_l4_example STATIC backends/hal_examples/nvlog_hal_stm32_flash.c)
target_compile_definitions(nvlog_hal_stm32_l4_example PRIVATE NVLOG_STM32_FLASH_L4=1)
target_include_directories(nvlog_hal_stm32_l4_example PUBLIC include backends/hal_examples)
target_link_libraries(nvlog_hal_stm32_l4_example nvlog)

add_library(nvlog_hal_stm32_h7_example STATIC backends/hal_examples/nvlog_hal_stm32_flash.c)
target_compile_definitions(nvlog_hal_stm32_h7_example PRIVATE NVLOG_STM32_FLASH_H7=1)
target_include_directories(nvlog_hal_stm32_h7_example PUBLIC include backends/hal_examples)
target_link_libraries(nvlog_hal_stm32_h7_example nvlog)

add_library(nvlog_espidf_partition_example STATIC backends/hal_examples/nvlog_espidf_partition.c)
target_include_directories(nvlog_espidf_partition_example PUBLIC include backends/hal_examples)
target_link_libraries(nvlog_espidf_partition_example nvlog)

# ── tests ─────────────────────────────────────────────────────────
add_executable(test_nvlog      tests/test_nvlog.c)
add_executable(test_powerloss  tests/test_powerloss.c)
add_executable(test_flash      tests/test_flash.c)
add_executable(test_ring       tests/test_ring.c)
add_executable(test_wire       tests/test_wire.c)
add_executable(compile_public_c tests/compile_public_c.c)
add_executable(compile_public_cpp tests/compile_public_cpp.cpp)
add_executable(test_model       tests/test_model.c)

foreach(t test_nvlog test_powerloss test_ring)
    target_include_directories(${t} PRIVATE include backends)
    target_link_libraries(${t} nvlog nvlog_posix)
endforeach()

target_include_directories(test_wire PRIVATE include)
target_link_libraries(test_wire nvlog)

target_include_directories(test_flash PRIVATE include backends)
target_link_libraries(test_flash nvlog nvlog_flash_sim)

target_include_directories(compile_public_c PRIVATE include backends)
target_link_libraries(compile_public_c nvlog)

target_include_directories(compile_public_cpp PRIVATE include backends)
target_link_libraries(compile_public_cpp nvlog)

target_include_directories(test_model PRIVATE include backends)
target_link_libraries(test_model nvlog nvlog_posix)

enable_testing()
add_test(NAME suite_v01 COMMAND test_nvlog)
add_test(NAME suite_v02 COMMAND test_powerloss)
add_test(NAME suite_v03 COMMAND test_flash)
add_test(NAME suite_v04 COMMAND test_ring)
add_test(NAME suite_v05 COMMAND test_wire)
add_test(NAME suite_v06 COMMAND compile_public_c)
add_test(NAME suite_v07 COMMAND compile_public_cpp)
add_test(NAME suite_v08 COMMAND test_model)

set_tests_properties(suite_v01 suite_v02 suite_v03 suite_v04 suite_v05 suite_v06 suite_v07 suite_v08
    PROPERTIES TIMEOUT 30)
