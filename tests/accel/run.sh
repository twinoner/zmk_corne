#!/bin/sh
# Compile and run the zip_accel math tests on the host.
#
# SPDX-License-Identifier: MIT

set -e

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="${TMPDIR:-/tmp}/test_accel_math"

cc -std=c11 -Wall -Wextra -I "$here/../../src/pointing" -o "$out" "$here/test_accel_math.c"
"$out"
