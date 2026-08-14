# ZMK v0.3.0 Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `zmk_corne` off the `daniel2887/zmk@ds87-mouse-pim447` fork onto upstream `zmkfirmware/zmk@v0.3.0`, replacing the fork's PIM447 support with a new in-repo Zephyr `input` driver, so that `nice-view-gem` can finally build.

**Architecture:** The repo becomes a full Zephyr module (it already declares `zephyr/module.yml`) hosting a ~150-line polled I²C input driver. The driver emits `INPUT_REL_X/Y` and `INPUT_BTN_0`; a `zmk,input-listener` node turns those into HID mouse events, with a child node that remaps X/Y to scroll while a `SCROLL` layer is active. All pointer tuning lives in devicetree input processors rather than driver code.

**Tech Stack:** ZMK v0.3.0, Zephyr 3.5 (`v3.5.0+zmk-fixes`), LVGL 8, nRF52840 (nice!nano v2), Docker (`zmkfirmware/zmk-build-arm:stable`, native arm64), devicetree + Kconfig.

**Spec:** `docs/superpowers/specs/2026-08-14-zmk-v030-migration-design.md`

## Global Constraints

- ZMK pin is `zmkfirmware/zmk` revision **`v0.3.0`**. Never `main` — `@main` is Zephyr 4.1 and its "explicit ZMK compat" CI gate fails for hwmv1 boards.
- `.github/workflows/build.yml` stays pinned to `build-user-config.yml@v0.3.0`. Do not change it.
- Board is plain **`nice_nano_v2`** (v0.3.0 is hwmv1). Not `nice_nano_v2/nrf52840/zmk`.
- `config/west.yml` must contain **exactly one** project named `zmk`. A duplicate name makes `west update` fail (this broke CI run 262).
- The trackball must stay on **`i2c1`**. `nice_view_adapter` reassigns `spi0` and sets `&pro_micro_i2c { status = "disabled"; }` because SPIM0 and TWIM0 are the same nRF52840 hardware block.
- The right half is **central** (`CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y` in `corne_right.conf`) because the trackball lives there. No task changes this.
- Shared headers included from `.keymap` / `.overlay` files must use the **quoted** form `#include "layers.h"`. The config directory is not on the DTS preprocessor's include path; quoted includes resolve relative to the including file.
- New source files carry `SPDX-License-Identifier: MIT` headers, matching the rest of the repo.
- `nice-view-gem` is pinned to **`v0.3.0`**, matching the ZMK pin. Its `main` branch requires Zephyr 4.1.

## Verification model

This repo has no unit tests, and ZMK's `native_posix` suite does not cover out-of-tree input drivers. The test cycle for every task is:

1. **Compile** locally in Docker via `scripts/zmk-build.sh` (Task 1 builds this). ~1–2 min.
2. **Inspect build output** — `build-out/<slug>/.config`, `zephyr.dts`, `build.log`.
3. **Push** and confirm CI is green.
4. **Flash and check on hardware** where the task changes runtime behavior.

Steps 1–3 are automatable. Step 4 is a human gate; the plan marks those steps explicitly.

**Checking CI without `gh`:** `gh` is not installed. Use the public API (60 req/hr unauthenticated):

```bash
curl -s "https://api.github.com/repos/twinoner/zmk_corne/actions/runs?branch=$(git rev-parse --abbrev-ref HEAD)&per_page=3" \
  | python3 -c "import json,sys; [print(r['run_number'], r['status'], r['conclusion'], r['head_sha'][:7], r['display_title']) for r in json.load(sys.stdin)['workflow_runs']]"
```

Per-step failure detail (job logs themselves are 403 without admin auth):

```bash
curl -s "https://api.github.com/repos/twinoner/zmk_corne/actions/runs/<RUN_ID>/jobs?per_page=30" \
  | python3 -c "
import json,sys
for j in json.load(sys.stdin).get('jobs',[]):
    print(j['conclusion'],'|',j['name'])
    for s in j.get('steps',[]):
        if s['conclusion'] not in ('success','skipped',None): print('   FAILED:', s['number'], s['name'])"
```

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `scripts/zmk-build.sh` | Create (T1) | Local Docker build mirroring CI; the test runner for every later task |
| `.gitignore` | Modify (T1) | Append `build-out/` — the file already exists and ignores `.superpowers/` |
| `config/west.yml` | Modify (T2, T5) | West manifest — ZMK pin, later nice-view-gem |
| `config/corne.dtsi` | **Delete** (T2) | Stale copy of the pre-v0.3.0 corne shield |
| `config/corne_right.overlay` | Delete (T2), Create (T4) | T2 removes the stale copy; T4 reintroduces it with only trackball additions |
| `config/layers.h` | Create (T2) | Single definition of layer numbering, shared by keymap and overlay |
| `config/corne.keymap` | Modify (T2) | Keymap — drop pim447 behaviors, add SCROLL layer |
| `config/corne.conf` | Modify (T2, T5) | Shared Kconfig — pointing, later custom status screen |
| `config/corne_right.conf` | Modify (T2, T3) | Central-half Kconfig — drop fork symbols, later enable I²C + driver |
| `zephyr/module.yml` | Modify (T3) | Promote board-root-only module to a full cmake/kconfig/dts module |
| `CMakeLists.txt` | Create (T3) | Module root — descends into `drivers/` |
| `Kconfig` | Create (T3) | Module root Kconfig — sources `drivers/Kconfig` |
| `drivers/CMakeLists.txt` | Create (T3) | Descends into `drivers/input/` |
| `drivers/Kconfig` | Create (T3) | Sources `drivers/input/Kconfig` |
| `drivers/input/CMakeLists.txt` | Create (T3) | Amends Zephyr's input library with our source |
| `drivers/input/Kconfig` | Create (T3) | `INPUT_PIM447`, auto-enabled by devicetree |
| `drivers/input/input_pim447.c` | Create (T3), Modify (T4) | T3: device registration + I²C readiness. T4: polling + input reports |
| `dts/bindings/input/pimoroni,pim447.yaml` | Create (T3) | Devicetree binding for the trackball node |
| `build.yaml` | Modify (T5) | CI matrix — swap `nice_view` for `nice_view_gem` |

---

## Task 1: Local Docker build loop

The remaining tasks each need a compile signal. Without this, every one costs a ~10-minute CI round trip. The image has a native `linux/arm64` variant, so it runs at full speed on Apple Silicon.

This task deliberately runs against the **current, unmigrated** HEAD (still on the ds87 fork). CI run 261 proves that combination builds green in this exact image, so a successful local build confirms the harness is faithful before anything changes.

**Files:**
- Create: `scripts/zmk-build.sh`
- Create: `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: `scripts/zmk-build.sh "<shield string>"`, honouring `BOARD` (default `nice_nano_v2`). Writes `build-out/<slug>/{zmk.uf2,.config,zephyr.dts,build.log}` where `<slug>` is the shield string with spaces replaced by `-`. Exits non-zero on build failure.

- [ ] **Step 1: Create the build script**

Create `scripts/zmk-build.sh`:

```bash
#!/usr/bin/env bash
#
# Build this ZMK config locally in Docker, mirroring what
# .github/workflows/build.yml does in CI.
#
# Usage:
#   scripts/zmk-build.sh "corne_right nice_view_adapter nice_view"
#   scripts/zmk-build.sh "settings_reset"
#   BOARD=nice_nano_v2 scripts/zmk-build.sh "corne_left nice_view_adapter nice_view"
#
# Artifacts land in build-out/<slug>/ : zmk.uf2, .config, zephyr.dts, build.log
#
# The west workspace lives in a persistent Docker volume so only the first
# run pays the download cost. To start clean: docker volume rm zmk-corne-west
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="zmkfirmware/zmk-build-arm:stable"
VOLUME="zmk-corne-west"

BOARD="${BOARD:-nice_nano_v2}"
SHIELD="${1:-}"
SLUG="$(echo "${SHIELD:-$BOARD}" | tr ' ' '-')"
OUT="${REPO_ROOT}/build-out/${SLUG}"

rm -rf "$OUT"
mkdir -p "$OUT"

# `set -e` would abort on a failed build before PIPESTATUS could be read,
# so the pipeline runs with errexit off and the status is captured by hand.
set +e
docker run --rm \
  -v "${REPO_ROOT}:/repo:ro" \
  -v "${VOLUME}:/ws" \
  -v "${OUT}:/out" \
  -e BOARD="$BOARD" \
  -e SHIELD="$SHIELD" \
  "$IMAGE" \
  bash -euo pipefail -c '
    rm -rf /ws/config
    mkdir -p /ws/config
    cp -R /repo/config/. /ws/config/

    cd /ws
    [ -d .west ] || west init -l config
    west update --fetch-opt=--filter=tree:0
    west zephyr-export

    rm -rf /ws/build
    EXTRA=()
    if [ -n "${SHIELD}" ]; then EXTRA+=(-DSHIELD="${SHIELD}"); fi

    west build -s zmk/app -d /ws/build -b "${BOARD}" -- \
      -DZMK_CONFIG=/ws/config \
      -DZMK_EXTRA_MODULES=/repo \
      ${EXTRA[@]+"${EXTRA[@]}"}
  ' 2>&1 | tee "${OUT}/build.log"

# tee swallows the exit status; recover it from PIPESTATUS before re-arming errexit.
status="${PIPESTATUS[0]}"
set -e

# Copy artifacts out even on failure — .config and zephyr.dts are often the
# most useful things to inspect when a build breaks.
docker run --rm -v "${VOLUME}:/ws" -v "${OUT}:/out" "$IMAGE" bash -c '
  cp /ws/build/zephyr/zmk.uf2    /out/ 2>/dev/null || true
  cp /ws/build/zephyr/.config    /out/ 2>/dev/null || true
  cp /ws/build/zephyr/zephyr.dts /out/ 2>/dev/null || true
' || true

echo
echo "Artifacts in build-out/${SLUG}/:"
ls -la "$OUT"
exit "$status"
```

- [ ] **Step 2: Make it executable and add .gitignore**

```bash
chmod +x scripts/zmk-build.sh
printf 'build-out/\n' >> .gitignore
```

`.gitignore` already exists and already ignores `.superpowers/`. **Append, do not overwrite** — `>` here would delete that entry.

- [ ] **Step 3: Run it against current HEAD — expect success**

```bash
./scripts/zmk-build.sh "corne_right nice_view_adapter nice_view"
```

Expected: exit 0, and `build-out/corne_right-nice_view_adapter-nice_view/zmk.uf2` exists. The first run downloads the west workspace (several minutes); later runs take ~1–2 minutes.

This mirrors CI run 261, which was green on this commit. If it fails, the harness is wrong — fix the script before proceeding, do not change any config.

- [ ] **Step 4: Verify the artifacts are usable for later inspection**

```bash
ls -la build-out/corne_right-nice_view_adapter-nice_view/
grep -c . build-out/corne_right-nice_view_adapter-nice_view/.config
grep -c . build-out/corne_right-nice_view_adapter-nice_view/zephyr.dts
```

Expected: `zmk.uf2`, `.config`, `zephyr.dts` and `build.log` all present and non-empty. Later tasks grep these files.

- [ ] **Step 5: Commit**

```bash
git add scripts/zmk-build.sh .gitignore
git commit -m "build: add local Docker build script mirroring CI

Gives every migration step a ~1-2 minute compile loop instead of a
10-minute CI round trip. Verified against current HEAD, which matches
green CI run 261."
```

---

## Task 2: Stage 1 — migrate to upstream ZMK v0.3.0

No trackball, no gem. This isolates the ZMK version bump from every other change.

This is a single task rather than several because a half-migrated tree does not compile: switching `west.yml` breaks the keymap's `pim447_*` references, and rewriting the keymap to use `pointing.h` breaks against the fork. There is no intermediate state that carries its own test cycle.

**Files:**
- Modify: `config/west.yml`
- Delete: `config/corne.dtsi`
- Delete: `config/corne_right.overlay`
- Create: `config/layers.h`
- Modify: `config/corne.keymap`
- Modify: `config/corne.conf`
- Modify: `config/corne_right.conf`

**Interfaces:**
- Consumes: `scripts/zmk-build.sh` from Task 1.
- Produces: `config/layers.h` defining `DEFAULT 0`, `LOWER 1`, `RAISE 2`, `ADJUST 3`, `SCROLL 4`. Task 4's overlay includes this header and references `SCROLL`.

- [ ] **Step 1: Point west.yml at upstream v0.3.0**

Replace the whole of `config/west.yml` with:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: v0.3.0
      import: app/west.yml
  self:
    path: config
```

The `zmk_ds87` remote and every commented-out block go away. Exactly one project named `zmk`.

- [ ] **Step 2: Delete the stale shield copies**

```bash
git rm config/corne.dtsi config/corne_right.overlay
```

Upstream's v0.3.0 `corne` shield already provides `kscan0`, `default_transform`, `five_column_transform`, `col-offset`, `col-gpios` and the `oled` node — plus `foostan_corne_5col/6col` physical layouts that the local copies would fight.

- [ ] **Step 3: Create the shared layer header**

Create `config/layers.h`:

```c
/*
 * Layer numbering, shared between corne.keymap and corne_right.overlay.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#define DEFAULT 0
#define LOWER   1
#define RAISE   2
#define ADJUST  3
#define SCROLL  4
```

- [ ] **Step 4: Update the keymap header block**

In `config/corne.keymap`, replace lines 7–28 (the include block, the layer `#define`s, and the `PIM447_SCROLL_MOVE` conditional) with:

```c
#include <behaviors.dtsi>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/pointing.h>
#include <dt-bindings/zmk/bt.h>
#include <dt-bindings/zmk/outputs.h>
#include <dt-bindings/zmk/ext_power.h>

#include "layers.h"
```

Changes: `dt-bindings/zmk/mouse.h` → `dt-bindings/zmk/pointing.h`; the `trackball_pim447.h` include is gone; the inline layer defines are replaced by `#include "layers.h"`; the `#ifdef CONFIG_ZMK_TRACKBALL_PIM447` block is gone.

**Drop the `ZMK_POINTING_DEFAULT_MOVE_VAL` / `ZMK_POINTING_DEFAULT_SCRL_VAL` defines entirely** — do not carry them forward. They are dead code: nothing in the keymap binds `&mmv` or `&msc`, and Tasks 3–4 scale the pointer through `zip_xy_scaler` / `zip_scroll_scaler` instead. They also cannot work where they sat: `dt-bindings/zmk/pointing.h:26-32` defines both under `#ifndef`, so any redefinition *after* that include is a plain macro-redefinition warning and the guard never fires. If mouse-move key bindings are ever added, restore these defines **above** the `pointing.h` include, not below it.

- [ ] **Step 5: Replace the pim447 bindings in the lower layer**

In `config/corne.keymap`, in `lower_layer`, change the third row from:

```
&trans &trans &trans &trans &pim447_scroll_move &pim447_toggle      &mkp MB1 &mkp MB3 &mkp MB2 &trans &trans &kp PIPE
```

to:

```
&trans &trans &trans &trans &mo SCROLL &tog SCROLL      &mkp MB1 &mkp MB3 &mkp MB2 &trans &trans &kp PIPE
```

`&mkp MB1/MB3/MB2` are unchanged — those identifiers exist in v0.3.0's `pointing.h`.

- [ ] **Step 6: Add the scroll layer**

In `config/corne.keymap`, after the `media_layer` block and before the closing `};` of `keymap`, add:

```c
		scroll_layer {
// The trackball listener remaps X/Y to scroll while this layer is active.
// All keys stay transparent so the underlying layer still types.
			label = "Scroll";
			bindings = <
&trans &trans &trans &trans &trans &trans		&trans &trans &trans &trans &trans &trans
&trans &trans &trans &trans &trans &trans		&trans &trans &trans &trans &trans &trans
&trans &trans &trans &trans &trans &trans		&trans &trans &trans &trans &trans &trans
					&trans &trans &trans		&trans &trans &trans
			>;
		};
```

The row widths must be 12/12/12/6 to match `default_transform`. A mismatch is a build error, not a silent bug.

- [ ] **Step 7: Switch the pointing Kconfig symbol**

In `config/corne.conf`, replace:

```
# mouse config
# CONFIG_ZMK_POINTING=y
CONFIG_ZMK_MOUSE=y
```

with:

```
# mouse config
CONFIG_ZMK_POINTING=y
```

- [ ] **Step 8: Strip the fork's symbols from the central-half config**

In `config/corne_right.conf`, remove these four lines:

```
CONFIG_SENSOR=y
CONFIG_ZMK_MOUSE=y
CONFIG_ZMK_TRACKBALL_PIM447=y
# CONFIG_ZMK_TRACKBALL_PIM447_LED_ON=y
```

Keep `CONFIG_ZMK_KEYBOARD_NAME`, `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`, the commented logging lines, and `CONFIG_I2C=y`. Leave `# CONFIG_INPUT=y` commented — Task 3 enables it.

- [ ] **Step 9: Build all three CI targets**

```bash
./scripts/zmk-build.sh "corne_left nice_view_adapter nice_view"
./scripts/zmk-build.sh "corne_right nice_view_adapter nice_view"
./scripts/zmk-build.sh "settings_reset"
```

Expected: all three exit 0.

If the build fails on a missing `pim447_scroll_move` or `pim447_toggle`, a keymap reference was missed. If it fails on a duplicate node label, a stale file from Step 2 survived.

- [ ] **Step 10: Audit for Kconfig symbols that no longer exist**

Zephyr warns rather than fails when a `.conf` sets an unknown symbol, so stale symbols become missing behavior, not build errors. Surface them:

```bash
grep -iE "undefined symbol|unknown symbol|no such config" build-out/*/build.log || echo "No undefined Kconfig symbols"
```

For every symbol reported, find the current name in ZMK v0.3.0 and update the `.conf` file, or delete the line if the feature is gone. Then re-run Step 9 until this grep is clean.

Also confirm the display widgets actually took effect:

```bash
grep -E "CONFIG_ZMK_POINTING=|CONFIG_ZMK_WIDGET_|CONFIG_ZMK_KEYBOARD_NAME=" \
  build-out/corne_right-nice_view_adapter-nice_view/.config
```

Expected: `CONFIG_ZMK_POINTING=y` is present. Any `CONFIG_ZMK_WIDGET_*` line you set in `corne.conf` that is absent here did not apply — investigate before continuing.

- [ ] **Step 11: Commit and push**

```bash
git add -A config/
git commit -m "feat: migrate to upstream ZMK v0.3.0

Drops the daniel2887/zmk@ds87-mouse-pim447 fork for zmkfirmware/zmk@v0.3.0.
Removes the stale local copies of the corne shield, which upstream now
provides along with physical layouts. Replaces the fork's pim447 behaviors
with a SCROLL layer driven by &mo/&tog; the trackball itself is inert until
the new input driver lands."
git push
```

- [ ] **Step 12: Confirm CI is green**

Run the CI-status command from the "Verification model" section. Expected: the newest run on this branch reaches `completed` / `success` with all three build jobs passing.

- [ ] **Step 13: HUMAN GATE — flash and verify**

Flash both halves with the `zmk.uf2` files from `build-out/`. Confirm: both halves pair; all four original layers type correctly; the conditional ADJUST layer (LOWER+RAISE) works; `&mkp MB1/MB2/MB3` click on the lower layer.

The trackball is expected to be completely dead at this point. Do not proceed until typing is confirmed working.

---

## Task 3: Stage 2a — module scaffolding and driver registration

Splitting the driver in two de-risks the module wiring, devicetree binding, `i2c1` pinctrl and I²C bus bring-up separately from any pointer logic. At the end of this task the device registers and logs, but reports nothing.

**Files:**
- Modify: `zephyr/module.yml`
- Create: `CMakeLists.txt`
- Create: `Kconfig`
- Create: `drivers/CMakeLists.txt`
- Create: `drivers/Kconfig`
- Create: `drivers/input/CMakeLists.txt`
- Create: `drivers/input/Kconfig`
- Create: `drivers/input/input_pim447.c`
- Create: `dts/bindings/input/pimoroni,pim447.yaml`
- Create: `config/corne_right.overlay`
- Modify: `config/corne_right.conf`

**Interfaces:**
- Consumes: `scripts/zmk-build.sh` (T1); `config/layers.h` (T2).
- Produces: devicetree compatible `"pimoroni,pim447"` with property `poll-interval` (int, ms, default 50); Kconfig symbol `INPUT_PIM447`; a devicetree node labelled `trackball` that Task 4's input-listener references via `<&trackball>`; C function `pim447_init(const struct device *dev)` returning `int`.

- [ ] **Step 1: Promote the module manifest**

Replace `zephyr/module.yml` with:

```yaml
name: zmk_corne
build:
  cmake: .
  kconfig: Kconfig
  settings:
    board_root: .
    dts_root: .
```

This mirrors ZMK's own `app/module/zephyr/module.yml`. `board_root` is preserved from the existing file. CI already passes `-DZMK_EXTRA_MODULES=<workspace>` whenever `zephyr/module.yml` exists, so no workflow change is needed.

- [ ] **Step 2: Create the module root CMake and Kconfig**

Create `CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: MIT

add_subdirectory(drivers)
```

Create `Kconfig`:

```
# SPDX-License-Identifier: MIT

rsource "drivers/Kconfig"
```

Create `drivers/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: MIT

add_subdirectory(input)
```

Create `drivers/Kconfig`:

```
# SPDX-License-Identifier: MIT

rsource "input/Kconfig"
```

- [ ] **Step 3: Create the driver's CMake and Kconfig**

Create `drivers/input/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: MIT

zephyr_library_amend()

zephyr_library_sources_ifdef(CONFIG_INPUT_PIM447 input_pim447.c)
```

`zephyr_library_amend()` appends to Zephyr's existing `drivers__input` library rather than declaring a new one — the same idiom ZMK uses in `app/module/drivers/input/CMakeLists.txt`.

Create `drivers/input/Kconfig`:

```
# SPDX-License-Identifier: MIT

if INPUT

config INPUT_PIM447
    bool "Pimoroni PIM447 trackball"
    default y
    depends on I2C
    depends on DT_HAS_PIMORONI_PIM447_ENABLED
    help
      I2C-polled input driver for the Pimoroni PIM447 trackball.
      Reports INPUT_REL_X / INPUT_REL_Y and INPUT_BTN_0.

endif # INPUT
```

`default y` plus `depends on DT_HAS_PIMORONI_PIM447_ENABLED` means the driver enables itself when the devicetree node exists — no explicit `CONFIG_INPUT_PIM447=y` needed in any `.conf`.

- [ ] **Step 4: Create the devicetree binding**

Create `dts/bindings/input/pimoroni,pim447.yaml`:

```yaml
# SPDX-License-Identifier: MIT

description: Pimoroni PIM447 trackball

compatible: "pimoroni,pim447"

include: [i2c-device.yaml]

properties:
  poll-interval:
    type: int
    default: 50
    description: |
      Interval in milliseconds at which the trackball registers are polled.
```

- [ ] **Step 5: Write the registration-only driver**

Create `drivers/input/input_pim447.c`:

```c
/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT pimoroni_pim447

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pim447, CONFIG_INPUT_LOG_LEVEL);

struct pim447_config {
    struct i2c_dt_spec i2c;
    uint32_t poll_interval_ms;
};

struct pim447_data {
    const struct device *dev;
};

static int pim447_init(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;

    if (!i2c_is_ready_dt(&cfg->i2c)) {
        LOG_ERR("I2C bus %s is not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    data->dev = dev;

    LOG_INF("PIM447 registered on %s at 0x%02x, poll interval %u ms", cfg->i2c.bus->name,
            cfg->i2c.addr, cfg->poll_interval_ms);

    return 0;
}

#define PIM447_INST(n)                                                                             \
    static struct pim447_data pim447_data_##n = {};                                                \
    static const struct pim447_config pim447_config_##n = {                                        \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                            \
        .poll_interval_ms = DT_INST_PROP(n, poll_interval),                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, pim447_init, NULL, &pim447_data_##n, &pim447_config_##n, POST_KERNEL, \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PIM447_INST)
```

- [ ] **Step 6: Create the overlay with pinctrl and the trackball node**

Create `config/corne_right.overlay`:

```c
/*
 * Additions on top of the upstream corne_right shield.
 *
 * SPDX-License-Identifier: MIT
 */

&pinctrl {
	i2c1_default: i2c1_default {
		group1 {
			psels = <NRF_PSEL(TWIM_SDA, 1, 4)>,
				<NRF_PSEL(TWIM_SCL, 1, 6)>;
		};
	};

	i2c1_sleep: i2c1_sleep {
		group1 {
			psels = <NRF_PSEL(TWIM_SDA, 1, 4)>,
				<NRF_PSEL(TWIM_SCL, 1, 6)>;
			low-power-enable;
		};
	};
};

&i2c1 {
	status = "okay";
	pinctrl-0 = <&i2c1_default>;
	pinctrl-1 = <&i2c1_sleep>;
	pinctrl-names = "default", "sleep";

	trackball: trackball@a {
		compatible = "pimoroni,pim447";
		reg = <0xa>;
		poll-interval = <20>;
	};
};
```

`NRF_PSEL(TWIM_SDA, 1, 4)` and `(TWIM_SCL, 1, 6)` reproduce the old overlay's `sda-pin = <36>` / `scl-pin = <38>` (36 = P1.04, 38 = P1.06). Zephyr 3.5 removed the bare `sda-pin`/`scl-pin` properties; pinctrl is mandatory.

This file does **not** include `layers.h` yet — nothing here references a layer. Task 4 adds the include together with the listener node that needs it.

- [ ] **Step 7: Enable I²C and the input subsystem on the central half**

In `config/corne_right.conf`, ensure these are present and uncommented:

```
CONFIG_I2C=y
CONFIG_INPUT=y
```

Also temporarily uncomment USB logging so Step 10 can observe driver init:

```
CONFIG_ZMK_USB_LOGGING=y
```

Task 4 removes the logging line again.

- [ ] **Step 8: Build the central half**

```bash
./scripts/zmk-build.sh "corne_right nice_view_adapter nice_view"
```

Expected: exit 0.

If it fails with "no such binding for pimoroni,pim447", `dts_root` in Step 1 is wrong or the binding path is not `dts/bindings/input/`.

- [ ] **Step 9: Verify the node and symbol actually landed**

```bash
OUT=build-out/corne_right-nice_view_adapter-nice_view
grep -n "CONFIG_INPUT_PIM447\|CONFIG_INPUT=" "$OUT/.config"
grep -n -A6 "trackball@a" "$OUT/zephyr.dts"
```

Expected: `.config` contains `CONFIG_INPUT=y` and `CONFIG_INPUT_PIM447=y`; `zephyr.dts` contains a `trackball@a` node with `status = "okay"`, `reg = <0x0a>` and `poll-interval = <0x14>`.

If `CONFIG_INPUT_PIM447` is missing while the node is present, the `DT_HAS_PIMORONI_PIM447_ENABLED` dependency name does not match the compatible — it is the compatible upper-cased with non-alphanumerics as underscores.

- [ ] **Step 10: Build the other two targets and commit**

```bash
./scripts/zmk-build.sh "corne_left nice_view_adapter nice_view"
./scripts/zmk-build.sh "settings_reset"
git add -A
git commit -m "feat: add PIM447 input driver scaffolding

Promotes the repo to a full Zephyr module and registers a pimoroni,pim447
input device on i2c1 with Zephyr 3.5 pinctrl. Registration and I2C readiness
only; no polling or reporting yet."
git push
```

Then confirm CI is green using the command from the "Verification model" section.

- [ ] **Step 11: HUMAN GATE — flash and confirm the driver initialises**

Flash the right half. Connect over USB serial and confirm the log line:

```
PIM447 registered on I2C_1 at 0x0a, poll interval 20 ms
```

If instead you see `I2C bus ... is not ready`, the `i2c1` pinctrl or bus configuration is wrong — that is exactly the risk this task exists to isolate. Also re-confirm typing still works on both halves.

---

## Task 4: Stage 2b — polling, input reports and the listener

**Files:**
- Modify: `drivers/input/input_pim447.c`
- Modify: `config/corne_right.overlay`
- Modify: `config/corne_right.conf`

**Interfaces:**
- Consumes: `pim447_init`, `struct pim447_config`, `struct pim447_data` and the `trackball` node from Task 3; `SCROLL` from `config/layers.h` (T2).
- Produces: the running driver reports `INPUT_REL_X`, `INPUT_REL_Y` and `INPUT_BTN_0`. Nothing later depends on it.

- [ ] **Step 1: Add the register map and polling state**

In `drivers/input/input_pim447.c`, after the `LOG_MODULE_REGISTER` line, add:

```c
/* Register map. Delta counters are cleared by the device on read. */
#define PIM447_REG_LEFT   0x04
#define PIM447_REG_RIGHT  0x05
#define PIM447_REG_UP     0x06
#define PIM447_REG_DOWN   0x07
#define PIM447_REG_SWITCH 0x08

#define PIM447_REG_COUNT (PIM447_REG_SWITCH - PIM447_REG_LEFT + 1)

/* Bit 7 of the switch register is the current button state; the low bits
 * are a press counter we do not use. */
#define PIM447_SWITCH_PRESSED BIT(7)
```

Then extend `struct pim447_data` to:

```c
struct pim447_data {
    const struct device *dev;
    struct k_work_delayable work;
    bool btn_pressed;
};
```

- [ ] **Step 2: Add the poll handler**

Insert this function immediately above `pim447_init`:

```c
static void pim447_poll(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct pim447_data *data = CONTAINER_OF(dwork, struct pim447_data, work);
    const struct device *dev = data->dev;
    const struct pim447_config *cfg = dev->config;
    uint8_t buf[PIM447_REG_COUNT];

    int ret = i2c_burst_read_dt(&cfg->i2c, PIM447_REG_LEFT, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Failed to read PIM447 registers: %d", ret);
        goto reschedule;
    }

    int16_t dx = (int16_t)buf[1] - (int16_t)buf[0]; /* RIGHT - LEFT */
    int16_t dy = (int16_t)buf[3] - (int16_t)buf[2]; /* DOWN  - UP   */
    bool pressed = (buf[4] & PIM447_SWITCH_PRESSED) != 0;
    bool btn_changed = pressed != data->btn_pressed;

    /* Stay silent when nothing happened so the listener is not woken on
     * every poll. */
    if (dx != 0 || dy != 0) {
        input_report_rel(dev, INPUT_REL_X, dx, false, K_NO_WAIT);
        input_report_rel(dev, INPUT_REL_Y, dy, !btn_changed, K_NO_WAIT);
    }

    if (btn_changed) {
        data->btn_pressed = pressed;
        input_report_key(dev, INPUT_BTN_0, pressed, true, K_NO_WAIT);
    }

reschedule:
    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
}
```

The `sync` flag is set on the last event of each batch: on `INPUT_REL_Y` when the button did not change, otherwise on the button event.

- [ ] **Step 3: Start polling from init**

In `pim447_init`, replace the `LOG_INF(...); return 0;` tail with:

```c
    k_work_init_delayable(&data->work, pim447_poll);
    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));

    LOG_INF("PIM447 polling on %s at 0x%02x every %u ms", cfg->i2c.bus->name, cfg->i2c.addr,
            cfg->poll_interval_ms);

    return 0;
```

`data->dev = dev;` must stay above this — the work handler reaches the device through it.

- [ ] **Step 4: Add the input listener to the overlay**

In `config/corne_right.overlay`, add the layer header include directly below the file's comment block, above `&pinctrl`:

```c
#include "layers.h"
```

The quoted form is required — the config directory is not on the DTS preprocessor's include path, so resolution relies on `layers.h` sitting beside this file.

Then append to the same file:

```c
/ {
	trackball_listener {
		compatible = "zmk,input-listener";
		device = <&trackball>;
		input-processors = <&zip_xy_scaler 11 10>;

		scroll {
			layers = <SCROLL>;
			input-processors = <&zip_xy_to_scroll_mapper>, <&zip_scroll_scaler 1 3>;
		};
	};
};
```

`zip_xy_scaler 11 10` is a 1.1× multiplier, approximating the fork's `move-factor-* = 110`. `zip_scroll_scaler 1 3` divides by 3, approximating `scroll-divisor-* = 3`. Both are starting points for tuning, not faithful reproductions — the fork's inertia and acceleration are deliberately not ported.

- [ ] **Step 5: Turn USB logging back off**

In `config/corne_right.conf`, re-comment the line added in Task 3:

```
# CONFIG_ZMK_USB_LOGGING=y
```

- [ ] **Step 6: Build and verify the listener node resolved**

```bash
./scripts/zmk-build.sh "corne_right nice_view_adapter nice_view"
OUT=build-out/corne_right-nice_view_adapter-nice_view
grep -n -A8 "trackball_listener" "$OUT/zephyr.dts"
```

Expected: exit 0, and `zephyr.dts` shows `trackball_listener` with a `scroll` child whose `layers = <0x04>` — the numeric value of `SCROLL`. If `layers` is missing or zero, `#include "layers.h"` did not resolve; confirm it uses the quoted form and that `config/layers.h` exists.

- [ ] **Step 7: Build the remaining targets and commit**

```bash
./scripts/zmk-build.sh "corne_left nice_view_adapter nice_view"
./scripts/zmk-build.sh "settings_reset"
git add -A
git commit -m "feat: poll the PIM447 and report pointer input

Adds the polling work handler emitting INPUT_REL_X/Y and INPUT_BTN_0, plus
the zmk,input-listener that turns them into HID mouse events and remaps X/Y
to scroll on the SCROLL layer."
git push
```

Then confirm CI is green.

- [ ] **Step 8: HUMAN GATE — flash and verify pointer behavior**

Flash the right half. Confirm, in order:

1. Rolling the ball moves the cursor, in the correct direction on both axes.
2. Pressing the ball performs a left click.
3. Holding the `&mo SCROLL` key (lower layer, position 5) makes the ball scroll instead of move.
4. Tapping `&tog SCROLL` (lower layer, position 6) latches scroll mode; tapping again releases it.
5. Typing on both halves still works.

If an axis is inverted, add `<&zip_xy_transform>` with the appropriate flags to `input-processors` rather than negating values in the driver. If the pointer is too slow or too fast, adjust the `zip_xy_scaler` ratio. Both are overlay-only changes; rebuild and reflash.

---

## Task 5: Stage 3 — enable nice-view-gem

The change the whole migration was for, and the smallest.

**Files:**
- Modify: `config/west.yml`
- Modify: `build.yaml`
- Modify: `config/corne.conf`

**Interfaces:**
- Consumes: the v0.3.0 base from Task 2.
- Produces: nothing downstream.

- [ ] **Step 1: Add nice-view-gem to the manifest**

Replace `config/west.yml` with:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: m165437
      url-base: https://github.com/M165437
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: v0.3.0
      import: app/west.yml
    - name: nice-view-gem
      remote: m165437
      revision: v0.3.0
  self:
    path: config
```

Still exactly one project named `zmk`. `nice-view-gem` is pinned to `v0.3.0` to match — its `main` branch requires Zephyr 4.1.

- [ ] **Step 2: Swap the shield in the CI matrix**

In `build.yaml`, change the two active shield lines from `nice_view` to `nice_view_gem`:

```yaml
include:
  - board: nice_nano_v2
    shield: corne_left nice_view_adapter nice_view_gem
  - board: nice_nano_v2
    shield: corne_right nice_view_adapter nice_view_gem
  - board: nice_nano_v2
    shield: settings_reset
```

Keep the existing commented alternatives.

- [ ] **Step 3: Switch to the custom status screen**

In `config/corne.conf`, uncomment the custom status screen line so the file reads:

```
CONFIG_ZMK_DISPLAY=y
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y
```

Then remove the now-obsolete comment line `# turn on to use https://github.com/M165437/nice-view-gem` that sat above it.

**The four `CONFIG_ZMK_WIDGET_*` lines this step originally deleted are already gone** — Task 2 removed them, because in v0.3.0 they force `app/src/display/widgets/*.c` to compile alongside the shield's own widgets and both expand `ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, …)` into a non-static `K_MUTEX_DEFINE`, which is a duplicate-symbol link error. Leave the explanatory comment Task 2 left in `corne.conf` in place: it is equally true of `nice_view_gem`, whose `Kconfig.defconfig` also implies `NICE_VIEW_WIDGET_STATUS`.

Note that both `nice_view` and `nice_view_gem` already default `ZMK_DISPLAY_STATUS_SCREEN_CUSTOM` via their own `Kconfig.defconfig`, so setting it explicitly is belt-and-braces — but gem's README instructs setting it, so keep it.

- [ ] **Step 4: Reset the west workspace and build**

`west.yml` changed, so the cached workspace must re-resolve the manifest:

```bash
docker volume rm zmk-corne-west
./scripts/zmk-build.sh "corne_right nice_view_adapter nice_view_gem"
./scripts/zmk-build.sh "corne_left nice_view_adapter nice_view_gem"
./scripts/zmk-build.sh "settings_reset"
```

Expected: all three exit 0. The first re-downloads the workspace.

If `west update` fails, check for a duplicate project name in the manifest — that is what broke CI run 262.

If compilation fails on `LV_*` symbols or `lv_animimg_*`, the ZMK pin has drifted off `v0.3.0`; re-check Step 1.

- [ ] **Step 5: Confirm the custom screen is selected**

```bash
grep -E "CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM|CONFIG_NICE_VIEW_WIDGET_STATUS|CONFIG_LV_USE_CANVAS" \
  build-out/corne_right-nice_view_adapter-nice_view_gem/.config
```

Expected: `CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y`, `CONFIG_NICE_VIEW_WIDGET_STATUS=y` and `CONFIG_LV_USE_CANVAS=y`. The last two come from gem's `Kconfig.defconfig` and prove the module is actually in the build.

- [ ] **Step 6: Commit and push**

```bash
git add -A
git commit -m "feat: enable nice-view-gem

Adds M165437/nice-view-gem at v0.3.0, matching the ZMK pin, and switches
both halves to the nice_view_gem shield with the custom status screen."
git push
```

Then confirm CI is green.

- [ ] **Step 7: HUMAN GATE — flash and verify the display**

Flash both halves. Confirm the gem screen renders: the crystal animation on the left (peripheral) and the status widgets — battery, layer, output, WPM gauge — on the right (central). Confirm typing and the trackball still work.

---

## Follow-up (not in this plan)

Recorded in the spec under "Deferred work": porting the fork's acceleration model (inertia, `norm`, `exactness`, `max-accel`) into the driver if plain scaling proves unsatisfying; extracting the driver to a standalone module repo; PIM447 LED control; `zip_temp_layer` for automatic scroll-on-move.

Once Task 5 is verified on hardware, the two memory files describing the old constraints — `zmk-workflow-must-stay-pinned` and `nice-view-gem-incompatible-with-ds87-fork` — are stale and should be rewritten to describe the new v0.3.0 baseline.
