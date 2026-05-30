# ESP-USB-Bridge — Waveshare ESP32-S3-Zero pinout

This board runs the `esp-usb-bridge` firmware (PlatformIO build, env `s3zero_jtag`).
Over a single USB-C connection it presents a composite device:

- **CDC serial** — transparent UART bridge to the target (use with `esptool` or any terminal)
- **Vendor / JTAG (or SWD)** — debug probe (`openocd-esp32`, or CMSIS-DAP)
- **MSC** — the `ESPPROG_MSC` drive; drop a `.uf2` here to flash the target (see `README.TXT`)

## Bridge ↔ target wiring

All eight target lines are on the **right castellated edge** of the board.
Connect a **common ground** between the bridge and the target.

| S3-Zero pad | Bridge signal | Goes to target |
|:-----------:|---------------|----------------|
| **GP13** | UART **TXD** (push-pull when active, Hi-Z when idle) | target **RX** |
| **GP12** | UART **RXD** (input) | target **TX** |
| **GP11** | **BOOT** (open-drain, idle high) | target **IO0** |
| **GP10** | **RST** (open-drain, idle high) | target **EN / RESET** |
| **GP9**  | JTAG **TDI** | target TDI |
| **GP8**  | JTAG **TDO** (input) | target TDO |
| **GP7**  | JTAG **TCK** / SWD **SWCLK** | target TCK |
| **GP16** | JTAG **TMS** / SWD **SWDIO** | target TMS |
| **GND**  | ground | target GND |

### Wiring notes

- **Minimum to flash over serial:** TXD, RXD, GND. BOOT/RST are optional — without
  them, `esptool` auto-reset can't sequence the target, so you boot it manually.
- **BOOT/RST are open-drain**, only ever pulled low (idle high via pull-ups), so an
  external programmer can share those lines without contention.
- **TXD goes high-impedance between sessions** so a target that repurposes its RX pin
  at runtime (e.g. as a display control line) isn't held by the dormant bridge.
- JTAG and SWD share the same TCK/TMS pads; the debug interface is selected at build
  time (JTAG by default).

## Onboard status LED (WS2812 on GP21)

| LED | Meaning |
|-----|---------|
| 🟢 **solid green** | idle — powered, no USB host |
| 🟢 **breathing green** | ready — enumerated by a host |
| 🔵 **blue** | flashing a UF2 to the target |
| 🔵 **blips** | bridge ↔ target serial traffic (blue → target, cyan ← target) |
| 🟣 **purple** | JTAG / SWD debug active |
| 🔴 **fast red blink** | fatal error |
| ⚫ **dark** | ROM bootloader / download mode (app not running) |

## Reserved / not usable for bridging

| Pins | Use |
|------|-----|
| GP19 / GP20 | native USB D− / D+ (the USB-C port) |
| GP21 | onboard WS2812 status LED |
| GP26–GP32 | in-package SPI flash (4 MB) + quad PSRAM (2 MB) |
| GP0 | BOOT strapping / onboard BOOT button |
| GP3, GP45, GP46 | strapping pins (avoid) |
