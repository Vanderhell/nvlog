function(add_nvlog_example target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends
        ${CMAKE_CURRENT_SOURCE_DIR}/common
    )
    target_link_libraries(${target} nvlog)
endfunction()

add_nvlog_example(example_minimal_linear minimal_linear/main.c)
add_nvlog_example(example_minimal_ring minimal_ring/main.c)

add_executable(example_posix_file posix_file/main.c)
target_include_directories(example_posix_file PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends
)
target_link_libraries(example_posix_file nvlog nvlog_posix)

add_nvlog_example(example_fram fram/main.c)
add_nvlog_example(example_eeprom eeprom/main.c)
add_nvlog_example(example_spi_nor spi_nor/main.c)
add_nvlog_example(example_stm32 stm32/main.c)
add_nvlog_example(example_esp_idf esp_idf/main.c)
add_nvlog_example(example_freertos freertos/main.c)
add_nvlog_example(example_zephyr zephyr/main.c)

