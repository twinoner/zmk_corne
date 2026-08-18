#!/bin/sh
# Compile and run the click-lock state machine tests on the host.
#
# SPDX-License-Identifier: MIT

set -e

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="${TMPDIR:-/tmp}/test_click_lock"

cc -std=c11 -Wall -Wextra -I "$here/../../src/pointing" -o "$out" "$here/test_click_lock.c"
"$out"
