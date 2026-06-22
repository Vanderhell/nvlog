#pragma once

#include <Arduino.h>
#include <stddef.h>

// This is the only layer that should know the API of the library under test.
bool project_test_setup(char *detail, size_t detail_size);
void project_test_tick();
bool project_test_run_once(char *detail, size_t detail_size);
bool project_test_handle_command(const char *command, Print &output);
