# Wireless serial bridge (RFC2217 over WiFi STA) — Design

- **Status:** Draft for review
- **Date:** 2026-05-30
- **Target:** Waveshare ESP32-S3-Zero, PlatformIO / ESP-IDF 5.5.2
- **Repo:** jlarnal/EspProg-S3-Zero (`origin`)

## 1. Goal

Add a wireless serial mode to the bridge: expose the target's UART over WiFi
as an **RFC2217** server so `esptool`, `idf-monitor`, and any pyserial-based
tool can flash and monitor the target across the network via
`rfc2217://espprog.local:3333` — with the target's BOOT/RST auto-reset working
wirelessly, exactly as it does over USB-CDC today.

The radio is **off by default** and costs zero RF/current until the user
deliberately arms it with a button gesture. Credentials are managed through a
single human-editable `wifi.txt` file surfaced on the existing MSC drive.

## 2. Non-goals (YAGNI)

- No web UI / captive portal / HTTP server.
- No SoftAP. STA only.
- No wireless JTAG/SWD. Debug probe stays USB-only.
- No raw-TCP serial port. RFC2217 only (it covers both flashing and monitoring).
- No persistence of the "armed" state across power cycles.
- No authentication on the RFC2217 port in v1 (trusted-LAN assumption; see §10).

## 3. User-facing behavior

1. **Power on.** WiFi is off. Device behaves exactly as today (USB-CDC bridge,
   JTAG/SWD probe, MSC UF2 flasher). LED: green idle / breathing-green ready.
2. **Arm.** User **double-clicks the onboard BOOT button (GPIO0)** — the
   S3-Zero's own button, *not* the target BOOT line (GPIO11). WiFi + RFC2217
   power on and stay on **until power is cut**. LED base tint switches to
   **orange**, blinking while connecting.
3. **Connect.** Firmware reads `wifi.txt` from LittleFS, parses the candidate
   networks, and tries them **in file order**. First association wins. LED goes
   steady/breathing orange. mDNS publishes `espprog.local`.
4. **Use.** Host connects `rfc2217://espprog.local:3333`. esptool's DTR/RTS map
   to the target's BOOT/RST via the existing `serial_handler` path → flashing
   and auto-reset work wirelessly. LED shows blue during flashing as usual, then
   returns to orange.
5. **Manage creds.** User edits `wifi.txt` on the MSC drive. The firmware moves
   any tested-and-failed credentials into a generated `# Failed credentials`
   section (annotated) so the user can see and delete them. Firmware edits become
   visible after a **manual replug/reboot** (the MSC mirror refreshes only on
   re-enumeration — chosen so afterthought corrections are never interrupted).

## 4. `wifi.txt` format and parser

### 4.1 Storage model (option B)

The canonical file lives in a **LittleFS** partition on internal flash. The MSC
drive surfaces a **live mirror** of it as `WIFI.TXT`. The MSC stays a synthetic
FAT skeleton, so **UF2 drag-drop flashing is untouched**. `WIFI.TXT` occupies a
known, fixed cluster in that synthetic FAT; the firmware therefore identifies
writes to the file purely by **LBA** (no content sniffing needed) — reads render
the current LittleFS content, writes to the file's cluster are captured.

### 4.2 Syntax

- Line-oriented. Lines are split on **LF**; a trailing **CR** is tolerated and
  stripped (so CRLF and LF both work). Only LF ends an entry.
- A `#` **outside** quotes starts a comment to end-of-line. A `#` **inside** the
  quoted value is literal (passwords may contain `#`).
- Recognized keys: `SSID="..."` and `PWD="..."` (value is the quoted string).
- Whitespace around the line and around `=` is trimmed.
- An **open network** is expressed as `PWD=""` (a dangling `SSID` with no `PWD`
  is dropped — see pairing rules).

### 4.3 Pairing algorithm

```
pending_ssid = none
candidates   = []   # ordered
for each line (split on LF, strip trailing CR):
    strip comment: drop from the first '#' that is OUTSIDE quotes
    trim
    if empty: continue
    if line matches  SSID="..."  :
        pending_ssid = value          # new SSID overwrites an unpaired one → "last SSID wins"
    else if line matches  PWD="..." :
        if pending_ssid is set:
            candidates.append( (pending_ssid, value) )
            pending_ssid = none        # extra PWDs until next SSID are ignored → "first PWD wins"
        # else: PWD with no pending SSID → ignored
# a trailing pending_ssid with no PWD is discarded
```

Worked example (the user's reference file) yields four candidates in file order:
`#1, #3, #2, #4`. The `# Working` / `# Failed` / `# not found` annotations are
ignored by the parser; whether a pair works is decided at runtime.

### 4.4 Firmware-owned sections

`wifi.txt` is bidirectional and firmware-owned:

- The **user** writes candidate `SSID`/`PWD` lines at the top.
- The **firmware** maintains a generated `# Failed credentials` section: any
  candidate it tried and that failed is moved there with an annotation
  (`# <-- not found` for no-AP-found, `# <-- refused` for auth failure). The
  entries stay as **live (uncommented) candidate lines** — the section is purely
  informational/ordering. Because the parser ignores the comment header and reads
  every `SSID`/`PWD` pair, failed creds **remain candidates and are retried** on
  each connect cycle; file order (fresh user entries on top, failed ones moved to
  the bottom) means they are simply tried *last*, as fallback. The user stops
  retrying a permanently-bad network by **deleting** its lines.
- If `wifi.txt` is **missing, empty, or whitespace-only**, the firmware
  regenerates a commented **template** with instructions and `#SSID=`/`#PWD=`
  stubs to uncomment.

### 4.5 Template (regenerated on empty/missing)

```
# EspProg-S3 WiFi credentials.
# Uncomment and fill the lines below (remove the leading '#'), then save.
# Add more SSID/PWD pairs to list fallback networks; they are tried top-down.
# Use PWD="" for an open network.
#
#SSID="your network name"
#PWD="your password"
```

## 5. Architecture

New component **`components/wireless/`** (public API in
`include/wireless.h`), plus targeted edits to existing modules.

| Unit | File | Responsibility | Depends on |
|---|---|---|---|
| Credential parser/store | `wifi_creds.c` | Parse `wifi.txt` text → ordered candidate list; serialize back with `# Failed credentials` section + annotations; generate template; LittleFS read/write. Pure, side-effect-light core. | esp_littlefs |
| WiFi STA manager | `wifi_sta.c` | Bring up STA; iterate candidates; handle connect/disconnect/got-IP events; auto-reconnect to the working AP; write failures back via `wifi_creds`; start/stop mDNS. | esp_wifi, mdns, wifi_creds |
| RFC2217 server | `rfc2217.c` | TCP listener on :3333; RFC2217 telnet COM-port-control option (baud via SET_BAUDRATE, DTR/RTS via SET_CONTROL); relay socket ⇄ `serial_handler`; honor the UART lock. PSRAM buffers. | lwip, serial_handler |
| Arm gesture | `arm_button.c` | GPIO0 runtime button; debounce; double-click (two presses ≤ ~400 ms) → fire arm event (start `wifi_sta` + `rfc2217`). One-shot per power cycle. | esp_driver_gpio, esp_timer |

Edits to existing modules:

- **`serial_handler`** — add a UART **ownership lock** API
  (`serial_handler_acquire(owner)` / `_release(owner)` / `_owner()`), where
  `owner ∈ {NONE, USB_CDC, NETWORK}`. Both `serial_bridge` (CDC) and `rfc2217`
  acquire before relaying. Integrates with existing `is_flashing` / reset logic.
- **`msc.c`** — add dynamic `WIFI.TXT`: a fixed cluster whose **read** renders
  the current LittleFS file (size patched into the FAT dir entry at runtime from
  a RAM cache), and whose **write** (identified by LBA) is captured, parsed, and
  persisted. UF2 write path unchanged.
- **`status_led`** — add orange WiFi states as an orthogonal base tint (§7).
- **`main.c`** — mount LittleFS at boot (regenerate template if needed), init the
  arm-button watcher; the arm event starts the wireless component. WiFi/RFC2217
  tasks pinned to **core 1**.

New build assets:

- **`partitions.csv`** — custom table: `nvs`, `phy_init`, `factory` (app), and a
  `storage` partition (subtype `littlefs`, e.g. 256 KB). 4 MB flash, ample room.
- **`idf_component.yml`** additions — `joltwallet/littlefs`, `espressif/mdns`.
- **`sdkconfig.s3zero.defaults`** additions — enable WiFi, mDNS, custom partition
  table, `CONFIG_WIRELESS_SERIAL=y` (Kconfig gate for the whole feature).

## 6. Activation gesture (GPIO0 double-click)

- GPIO0 is the onboard BOOT button. It is only a strapping pin **at reset**;
  after boot it is read freely as an active-low input (internal pull-up; press =
  low). The bridge does not otherwise use GPIO0.
- Detection: debounce ~20 ms; a **double-click** = two press→release cycles whose
  second press starts within ~400 ms of the first release.
- Effect: fire the arm event once. WiFi + RFC2217 start and remain on until power
  is removed. Not stored in NVS — every power-up begins disarmed.
- A single click does nothing (reserved; avoids accidental arming).

## 7. LED behavior (orange WiFi tint)

WiFi state is an **orthogonal axis** layered on the existing single pixel as the
idle/ready **base tint**, with operation events taking priority:

| WiFi state | Base tint (when otherwise idle/ready) |
|---|---|
| Disarmed | green (unchanged) |
| Armed, connecting / searching | orange, blinking |
| Connected (associated, IP) | orange, steady (breathes like ready) |
| Armed but all candidates failed | orange, slow blink (= "on, not connected") |

Priority override (unchanged, higher than base tint): 🔵 flashing, 🟣 JTAG/debug,
🔴 fatal error, and 🔵/🩵 TX/RX traffic blips. So an esptool flash over WiFi still
shows blue, then falls back to orange. Orange (physical RGB ≈ `255,90,0`) is
distinct from every color currently in use (green/blue/cyan/purple/red).

## 8. Data flow

```
double-click GPIO0
   └─> arm event ─> wifi_sta.start()
                      ├─ wifi_creds.load()  ── LittleFS ──> [candidates]
                      ├─ for each candidate: STA connect (in order)
                      │     success ─> got-IP ─> mdns(espprog.local) ─> rfc2217.start(:3333)
                      │     failure ─> wifi_creds.mark_failed(c)  ──> LittleFS (visible after replug)
                      └─ auto-reconnect to the working AP on drop

host  rfc2217://espprog.local:3333
   └─> rfc2217.accept() ─> serial_handler.acquire(NETWORK)
            data:  socket <──> serial_handler UART <──> target
            DTR/RTS (SET_CONTROL) ─> serial_handler boot/reset  (== CDC path)
            baud   (SET_BAUDRATE) ─> serial_handler set baud
        on close ─> serial_handler.release(NETWORK)

host edits WIFI.TXT on MSC (while armed)
   └─> msc write (by LBA) ─> capture ─> wifi_creds.parse ─> LittleFS store
                                          └─> wifi_sta re-attempt with new candidates
       (the updated file with results is shown after a manual replug)
```

## 9. UART arbitration

Exactly one transport owns the target UART at a time. `serial_handler` holds an
owner token (`NONE` / `USB_CDC` / `NETWORK`). Whoever holds the lock keeps it
until it releases; a newcomer that finds the UART owned by the other transport is
**refused** (e.g. the RFC2217 socket is accepted then closed with the UART busy).
This guarantees no interleaved bytes corrupt a flash. The existing `is_flashing`
MSC-UF2 gate remains the highest-priority holder.

## 10. Security

The RFC2217 port is **open** in v1 — anyone on the LAN who can reach
`espprog.local:3333` can monitor or flash the target. This is acceptable under a
trusted-LAN assumption and is **documented** in `README.TXT` / `pinout.md`. A
simple shared-token gate is recorded as a future toggle, default off. The
double-click-to-arm gesture already limits exposure: the radio is dark until a
human physically arms it, and a power cycle disarms it.

## 11. Error handling

- **No candidates / all fail:** radio stays on (armed), LED slow-blink orange,
  failures written to the `# Failed credentials` section (still live candidates,
  retried as fallback per §4.4). Editing `wifi.txt` while armed re-triggers a
  connect attempt (no re-arm needed); results show after a replug.
- **LittleFS mount failure:** fall back to a RAM-only template, log an error,
  continue (USB bridge unaffected); WiFi cannot be armed until storage recovers.
- **mDNS failure:** non-fatal; the raw DHCP IP still works.
- **Lock contention:** newcomer refused (§9); no corruption.
- **WiFi drop:** auto-reconnect to the last working AP; RFC2217 sessions drop and
  must reconnect.

## 12. Configuration / build

- `CONFIG_WIRELESS_SERIAL` (Kconfig, default `n`) gates the entire feature;
  enabled in `sdkconfig.s3zero.defaults`. WiFi/mDNS/LittleFS pulled in only then.
- Custom `partitions.csv` (adds the `littlefs` storage partition).
- RFC2217 port and mDNS hostname exposed as Kconfig options (defaults `3333`,
  `espprog`).
- WiFi and RFC2217 tasks pinned to **core 1**; bridge data path stays on core 0.
- Must keep building across the existing CI matrix when the feature is **off**
  (no new mandatory deps unless `CONFIG_WIRELESS_SERIAL`).

## 13. Verification

No host unit-test suite exists (per `CLAUDE.md`); verification is build + on-device.

**Parser (the riskiest pure logic) — on-device or scratch host build:**
- Reference file → 4 candidates `#1,#3,#2,#4` in order.
- `SSID;SSID;PWD` → last SSID wins.
- `SSID;PWD;PWD` → first PWD wins.
- Password containing `#` inside quotes → preserved.
- `PWD=""` → open network candidate.
- Empty / whitespace-only / missing → template regenerated.

**On-device end-to-end:**
- Double-click GPIO0 → LED orange-blink → connects → orange-steady; mDNS resolves.
- `esptool --port rfc2217://espprog.local:3333 ...` flashes the target; BOOT/RST
  auto-reset works.
- Bad credentials appear in `# Failed credentials` after a replug.
- USB-CDC and a network client cannot both drive the UART (one is refused).
- Power-cycle → disarmed (radio off) until re-armed.

## 14. Suggested build order (for the implementation plan)

1. LittleFS partition + mount + `wifi_creds` parser/store/template (testable alone).
2. `WIFI.TXT` mirror in MSC (read render + LBA write capture).
3. `serial_handler` ownership lock; wire `serial_bridge` to it.
4. WiFi STA manager (connect, candidate iteration, failure write-back, mDNS).
5. RFC2217 server; relay + control-line mapping; lock integration.
6. GPIO0 double-click arm gesture wiring it all together.
7. `status_led` orange WiFi tint.
8. Docs: `README.TXT` / `pinout.md` updates (wifi.txt format, arming, URL, LED).

## 15. Open questions / future

- Optional RFC2217 shared-token auth (default off).
- Should `WIFI.TXT` also list the currently-connected SSID/IP as a generated
  status comment? (cheap, deferred).
- Multiple-pixel or brightness coding if more orthogonal states accumulate.
