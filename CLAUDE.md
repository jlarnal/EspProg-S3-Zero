# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP USB Bridge: an ESP-IDF firmware that turns an **ESP32-S2 or ESP32-S3** into a USB bridge between a host PC and a target microcontroller (usually another ESP chip). It replaces USB-to-UART chips (CP210x etc.) and a debug probe. Over a single USB composite device it exposes:

- **CDC** — serial bridge to the target's UART (use with esptool or any terminal)
- **Vendor interface** — debug probe: either ESP USB-JTAG (`openocd-esp32`) **or** SWD via CMSIS-DAP — selected at build time, not both at once
- **MSC** — mass-storage device; drag-and-drop UF2 binaries to flash the target

The bridge also drives the target's BOOT(IO0)/RST(EN) GPIOs from the CDC control lines so esptool's auto-reset/boot sequencing works through the bridge.

## Build / Flash / Run

Standard ESP-IDF project, IDF **>= 5.0**. Set the target before the first build (S2 and S3 are not interchangeable):

```
idf.py set-target esp32s2      # or esp32s3
idf.py menuconfig              # optional, see "Configuration" below
idf.py build
idf.py -p <PORT> flash monitor
```

`<PORT>` for flashing the bridge itself is the UART connected to the bridge MCU, **not** the bridge's own USB CDC port. After flashing, the bridge operates over its USB interface.

CI is on **GitLab** (`.gitlab-ci.yml` + `.gitlab-ci.yml.d/build.yml`), not GitHub Actions (which only run pre-commit/launchpad/release/jira). The `build_matrix` job is the full cross-product: IDF `release-v5.0`…`v5.5`, `v6.0`, `latest` × `esp32s2`/`esp32s3` × debug interface `jtag`/`swd`. It builds with pedantic flags (`-Werror`, `-Werror=unused-*`, `-Wstrict-prototypes`), so unused variables/functions and missing prototypes fail CI. Keep changes building across that whole matrix. There is no host-side unit-test suite; verification is build-matrix + on-device testing.

Reproduce a CI build locally, e.g. SWD on S3:

```
idf.py -DIDF_TARGET=esp32s3 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.swd.defaults" build
```

## Linting / Style

ESP-IDF coding style, enforced by **pre-commit** (`.pre-commit-config.yaml`) — GitHub PRs run it (`.github/workflows/pre_commit.yml`). Install and run before committing:

```
pip install pre-commit
pre-commit install        # installs both pre-commit and commit-msg hooks
pre-commit run --all-files
```

Hooks include: `astyle_py` (rules in `astyle-rules.yml`), `check-copyright` (config in `check_copyright_config.yaml`), `codespell`, `mdformat`, `double-quote-string-fixer`, and a **Conventional Commits** linter on commit messages (commit-msg stage). Commit subjects must follow Conventional Commits, e.g. `fix(build): ...`, `ci(github): ...` (see git history). Every source file needs the SPDX Apache-2.0 header.

## Architecture

Top-level app lives in `main/`; reusable pieces are ESP-IDF components under `components/`.

### Startup (`main/main.c`)

`app_main()` runs synchronously, then USB takes over:

```
init_led_gpios()                         -> status LEDs (also used for error reporting)
init_serial_no()                         -> device serial = efuse MAC
int_usb_phy()                            -> bring up internal USB OTG PHY (device, full-speed)
serial_handler_init(TRANSPORT_TYPE_UART) -> UART driver + BOOT/RST GPIO control
serial_bridge_init()                     -> wire CDC <-> serial_handler
tusb_init(); msc_init()
xTaskCreate(tusb_device_task)            -> runs tud_task() forever
```

The debug probe is **not** started in `app_main`; `tud_mount_cb()` (also in `main.c`) calls `eub_vendord_start()` and `debug_probe_init()` once the host enumerates the device.

### USB descriptors and interface layout

USB device/config/string/BOS descriptors are defined directly in `main/main.c` (not a separate file). Interface and endpoint numbers are centralized in `main/usb_defs.h` and the **order is fixed**: `ITF_NUM_CDC`, `ITF_NUM_CDC_DATA`, `ITF_NUM_VENDOR`, `ITF_NUM_MSC`. The Vendor interface uses a custom descriptor macro (`TUD_VENDOR_EUB_DESCRIPTOR`) because the ESP USB-JTAG subclass/protocol differ from TinyUSB defaults. BOS + Microsoft OS 2.0 descriptor is present for WebUSB/Windows. The device serial-number string is the efuse MAC; the product string and the protocol-caps string descriptor (`DEBUG_PROBE_STR_DESC_INX`) change depending on JTAG vs CMSIS-DAP.

TinyUSB comes from the IDF component manager (`main/idf_component.yml` → `espressif/tinyusb`), configured via `main/public_include/tusb_config.h` (force-included into the tinyusb component lib by `main/CMakeLists.txt`). `bcdDevice` is derived from the git tag at build time in `main/CMakeLists.txt`.

### Data-path modules

- `main/serial_bridge.c` — connects the USB **CDC** interface to `serial_handler`: CDC RX callback pushes to the handler; a task pumps handler output back to CDC.
- `main/eub_vendord.c` — class driver / data path for the **Vendor** interface used by the debug probe.
- `main/msc.c` (`msc.h`) — **MSC**: presents a synthetic FAT16 skeleton (boot sector / FAT / root dir / README are constants; reads outside them return zeros). Host writes are interpreted as **UF2** blocks and flashed to the target via the `esp-serial-flasher` component. See the long comment block at the top of `msc.c` for the TinyUSB MSC callback contract.

### Components

- `components/serial_handler/` — UART (UART_NUM_1) transport to the target plus **BOOT/RST GPIO control**. Translating CDC DTR/RTS into the target's boot/reset sequence lives here; changes can break esptool auto-reset (see recent `serial_handler` commits before touching it). It also owns the `esp-serial-flasher` path used by MSC flashing (note `atomic_bool is_flashing` gating bridge vs. flash mode). Config menu "**Serial Handler Configuration**": `SERIAL_HANDLER_GPIO_BOOT` (4), `_GPIO_RST` (7), `_GPIO_RXD` (6), `_GPIO_TXD` (5).
- `components/debug_probe/` — the debug interface. Kconfig menu "**Debug Probe Configuration**" → choice `DEBUG_PROBE_INTERFACE` selects **`DEBUG_PROBE_IFACE_JTAG`** (default; ESP USB-JTAG via `esp_usb_jtag.c`, for `openocd-esp32`) or **`DEBUG_PROBE_IFACE_SWD`** (CMSIS-DAP via the bundled ARM sources under `DAP/`, plus `swd.c`); both are always compiled. `DEBUG_PROBE_IFACE_NAME` resolves to "JTAG"/"CMSIS-DAP" and feeds the USB product/string descriptors. `debug_gpio.c` is the pin layer (`DEBUG_PROBE_GPIO_TDI`=3, `_TDO`=9, `_TCK`=10, `_TMS`=8). Public API: `include/debug_probe.h` (`debug_probe_init`, `debug_probe_get_proto_caps`, activity callback, `debug_probe_handle_esp32_tdi_bootstrapping` — TDI/GPIO12 is an ESP32 flash-voltage strapping pin). `DAP/` is third-party and excluded from whitespace hooks.
- `components/util/` — `util.c`, `led_io.h`, `util.h`: LED pin definitions and `eub_abort()` (used for fatal-error LED signaling).

## Configuration (Kconfig)

Options are spread across three menus (project `main/Kconfig.projbuild` "Bridge Configuration", plus each component's `Kconfig`):

- **Bridge Configuration** (`main/Kconfig.projbuild`): `BRIDGE_USB_VID` (0x303A), `BRIDGE_USB_PID` (0x1002), manufacturer/product strings, LED GPIOs `BRIDGE_GPIO_LED1..3` (+ active level), `BRIDGE_MSC_VOLUME_LABEL`.
- **Serial handler Configuration** and **Debug probe Configuration**: see component descriptions above.

IDF auto-applies `sdkconfig.defaults` and `sdkconfig.defaults.<target>`. The debug interface is normally chosen via the layered presets `sdkconfig.jtag.defaults` / `sdkconfig.swd.defaults` — pass them with `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.<jtag|swd>.defaults"` (this is exactly what CI does) — or flip the choice in `menuconfig`. `sdkconfig.defaults.esp_prog2` is a board-specific preset.

Every custom board should set its **own VID/PID** (register a PID under the Espressif VID per the README).

## Gotchas

- TX/RX activity LEDs are intentionally swapped in the callbacks (`main.c`) to indicate bridge→target direction — not a bug.
- A `CFG_TUD_ENDPOINT0_SIZE` vs `CFG_TUD_ENDOINT0_SIZE` (typo) `#ifdef` in `main.c` handles older TinyUSB versions; keep both branches.
