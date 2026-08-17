# SPDX-License-Identifier: MIT

# Input processors compile into ZMK's `app` target, not into a zephyr_library:
# <drivers/input_processor.h> lives in zmk/app/include, which app/CMakeLists.txt
# marks PRIVATE to that target. This is deliberately unlike drivers/input, which
# uses zephyr_library_amend() to extend Zephyr's own `input` library.
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_ACCEL app PRIVATE input_processor_accel.c)
