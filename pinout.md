# ESP-USB-Bridge — Waveshare ESP32-S3-Zero pinout

This board runs the `esp-usb-bridge` firmware (PlatformIO build, env `s3zero_jtag`).
Over a single USB-C connection it presents a composite device:

- **CDC serial** — transparent UART bridge to the target (use with `esptool` or any terminal)
- **Vendor / JTAG (or SWD)** — debug probe (`openocd-esp32`, or CMSIS-DAP)
- **MSC** — the `ESPPROG_MSC` drive; drop a `.uf2` here to flash the target (see `README.TXT`)

It also offers an optional **wireless serial** mode — RFC2217 over WiFi, armed with
the BOOT button — so you can flash and monitor the target over the network. WiFi
credentials live in `WIFI.TXT` on the MSC drive. See *Wireless serial* below.

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

## Wireless serial (RFC2217 over WiFi)

The bridge can expose the target's UART over WiFi, so you can flash and monitor it
from anywhere on your network instead of plugging USB into the bridge. It speaks
**RFC2217**, so `esptool`, `idf-monitor`, and any pyserial-based tool work
directly — and the target's BOOT/RST auto-reset is carried over the link, so
flashing works exactly like it does on the cable.

**The radio is off until you turn it on.** No WiFi runs at power-up.

### 1. Set your WiFi credentials

On the `ESPPROG_MSC` drive, open **`WIFI.TXT`** and list your network(s):

```
SSID="MyHomeWiFi"
PWD="supersecret"

# extra pairs act as fallbacks — tried top to bottom
SSID="Workshop2G"
PWD="hunter2"
```

- One `SSID="..."` line, then its `PWD="..."` line underneath.
- `#` starts a comment. A `#` *inside* the quotes is part of the password.
- Open network? Use `PWD=""`.
- List several pairs to have fallbacks; they're tried in order.

Save the file. If `WIFI.TXT` is empty or missing, the bridge rewrites it with a
template you can fill in.

### 2. Arm WiFi

**Double-click the onboard BOOT button** — the little button on the S3-Zero
itself, *not* a target wire. The status LED turns **orange**:

- **blinking orange** — connecting / searching
- **steady orange** — connected and ready

WiFi stays on until you unplug the board. Every power-up starts with it off, so
double-click again to re-arm.

### 3. Connect

Point your tool at the bridge over the network:

```
# flash
esptool --port rfc2217://espprog.local:3333 --baud 460800 write_flash 0x0 firmware.bin

# monitor
python -m serial.tools.miniterm rfc2217://espprog.local:3333 115200
```

If `espprog.local` doesn't resolve on your OS, use the board's IP address (from
your router's client list) in its place.

### 4. See what connected, fix bad credentials

After trying your networks, the bridge rewrites `WIFI.TXT`, moving any that failed
into a `# Failed credentials` section with the reason (`# <-- not found` or
`# <-- refused`). To see the updated file, **unplug and replug** the board — the
drive refreshes on reconnect. Delete any lines you no longer want; whatever stays
listed keeps being retried.

### Notes & limits

- **STA only** — the bridge joins your existing WiFi; it doesn't host its own.
- **One user at a time on the UART** — if a USB serial session is open, the
  network port waits, and vice-versa. JTAG/SWD debug stays USB-only.
- **Security** — the network port is open to anyone on your LAN who can reach it.
  Use a trusted network. Not arming it (or unplugging) keeps the target private.

## Onboard status LED (WS2812 on GP21)

| LED | Meaning |
|-----|---------|
| 🟢 **solid green** | idle — powered, no USB host |
| 🟢 **breathing green** | ready — enumerated by a host |
| 🟠 **blinking orange** | WiFi armed — connecting / searching |
| 🟠 **steady orange** | WiFi connected — RFC2217 ready |
| 🟠 **slow orange blink** | WiFi armed, but no network joined |
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
