# SPDX-License-Identifier: MIT

# Input processors compile into ZMK's `app` target, not into a zephyr_library:
# <drivers/input_processor.h> lives in zmk/app/include, which app/CMakeLists.txt
# marks PRIVATE to that target. This is deliberately unlike drivers/input, which
# uses zephyr_library_amend() to extend Zephyr's own `input` library.
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_ACCEL app PRIVATE input_processor_accel.c)

# Both halves of click lock build together under one option: they share the
# mode flag defined in the processor, so the behavior alone would not link.
target_sources_ifdef(CONFIG_ZMK_CLICK_LOCK app PRIVATE input_processor_click_lock.c)
target_sources_ifdef(CONFIG_ZMK_CLICK_LOCK app PRIVATE behavior_click_lock.c)
