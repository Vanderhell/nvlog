# Hardware Validation

This repo currently ships compile-verified hardware example skeletons.

## Validation steps for a new board

- verify backend read/write alignment
- verify erase behavior
- verify 0 -> 1 protection if relevant
- verify mount after interrupted append
- verify ring overwrite behavior if supported
- verify program unit size
- verify timeout recovery

## STM32

Check the family-specific flash geometry:

- F4: sector erase, 32-bit program model
- L4: 8-byte program model
- H7: 32-byte flash-word model

## ESP32 / ESP-IDF

Validate the partition backend against the SDK storage API and its sync behavior.

