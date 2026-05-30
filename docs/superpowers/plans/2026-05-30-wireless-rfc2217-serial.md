# Wireless Serial Bridge (RFC2217 over WiFi STA) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in wireless serial mode that exposes the target UART over WiFi as an RFC2217 server (`rfc2217://espprog.local:3333`), armed by a double-click on the onboard BOOT button, with credentials managed through a `wifi.txt` file mirrored on the MSC drive.

**Architecture:** A new gated `components/wireless/` component holds four units — a pure `wifi.txt` parser, a LittleFS-backed credential store, a WiFi STA manager, an RFC2217 TCP server — plus a GPIO0 double-click arm-gesture watcher. It reuses the existing `serial_handler` UART path (adding an ownership lock so only one of USB-CDC / network drives the target at a time), the existing MSC synthetic-FAT drive (adding a writable `WIFI.TXT` file), and the existing WS2812 `status_led` (adding orange WiFi states). The radio stays off until armed; everything is `#if CONFIG_WIRELESS_SERIAL`.

**Tech Stack:** ESP-IDF 5.5.2 (PlatformIO, env `s3zero_jtag`), `esp_wifi` (STA), `espressif/mdns`, `joltwallet/littlefs`, lwIP BSD sockets, FreeRTOS, TinyUSB MSC.

---

## Testing strategy (read first)

Per `CLAUDE.md` this repo has **no host unit-test suite**; verification is build-matrix + on-device. This plan adapts TDD to that reality:

- **Pure logic (`wifi_creds` parser):** real TDD with a standalone host test (`test/host/test_wifi_creds.c`) compiled with `gcc` — it has zero IDF dependencies. If no host `gcc` is available (e.g. bare Windows), the same cases run on-device via a boot-time self-test gated by `CONFIG_WIRELESS_SELFTEST` (logs `PASS`/`FAIL` over the monitor).
- **Every other task** gates on a clean build: `pio run -e s3zero_jtag` must succeed. CI builds with `-Werror -Werror=unused-* -Wstrict-prototypes`, so unused symbols / missing prototypes fail the gate — treat a clean build as the per-task test.
- **End-to-end behavior** is verified on-device in the final task, from spec §13.

> **PlatformIO patience (hard rule):** editing `platformio.ini` triggers PlatformIO's background reconfigure (dependency re-resolution). After any `platformio.ini` edit, WAIT for it to finish before running `pio run` or touching `managed_components/`. Editing a component `CMakeLists.txt`/`idf_component.yml` does **not** trigger a reconfigure — force one with a trivial `platformio.ini` touch (see `pio-s3zero-build-gotchas` memory). Never delete the penv or component cache to "fix" a build.

Build command used throughout (run from the project root; the working dir is already correct):

```
pio run -e s3zero_jtag
```

Expected on success: `========= [SUCCESS] ...` and a fresh `.pio/build/s3zero_jtag/firmware.bin`.

---

## File structure

**New component `components/wireless/`:**

| File | Responsibility |
|---|---|
| `Kconfig` | `CONFIG_WIRELESS_SERIAL` (gate), `_RFC2217_PORT`, `_MDNS_HOSTNAME`, `_ARM_GPIO`, `_SELFTEST` |
| `idf_component.yml` | managed deps: `joltwallet/littlefs`, `espressif/mdns` |
| `CMakeLists.txt` | register sources + unconditional `REQUIRES` (gated code, not gated requires) |
| `include/wireless.h` | public API used by `main`/`msc` |
| `wifi_creds.h` / `wifi_creds.c` | pure parser + LittleFS store + template + failed-section rewrite |
| `wifi_sta.h` / `wifi_sta.c` | STA manager: connect candidates, events, mDNS, write-back failures, drive LED + rfc2217 |
| `rfc2217.h` / `rfc2217.c` | RFC2217 TCP server: IAC parse, SET_CONTROL→boot/reset, SET_BAUDRATE→baud, data relay |
| `arm_button.h` / `arm_button.c` | GPIO0 runtime button, debounce, double-click → callback |
| `test/host/test_wifi_creds.c` | host TDD test for the parser (gcc) |

**Modified files:**

| File | Change |
|---|---|
| `components/serial_handler/include/serial_handler.h` | add owner-lock API + net data callback |
| `components/serial_handler/serial_handler.c` | implement lock + route RX by owner |
| `components/status_led/include/status_led.h` | add `status_led_set_wifi()` + enum |
| `components/status_led/status_led.c` | render orange WiFi base tint |
| `main/msc.c` | add writable `WIFI.TXT`: cluster 4, dir entry, read render, write capture |
| `main/msc.h` | declare `msc_set_wifi_txt()` |
| `main/main.c` | `wireless_init()` at boot; arm button → `wireless_arm()` |
| `main/CMakeLists.txt` | add `"wireless"` to `dependencies` |
| `partitions.csv` (new) | factory app + `storage` (LittleFS) partition |
| `sdkconfig.s3zero.defaults` | custom partition table + `CONFIG_WIRELESS_SERIAL=y` |
| `pinout.md` | already updated (manual + orange LED rows) — no change here |

## Shared interfaces (defined once, used across tasks)

These exact names/signatures are used by later tasks — do not rename.

```c
/* wifi_creds.h */
#define WIFI_CREDS_MAX     8
#define WIFI_SSID_MAXLEN   32
#define WIFI_PWD_MAXLEN    64
#define WIFI_TXT_MAXLEN    4096   /* one FAT cluster; WIFI.TXT is clamped to this */

typedef enum { WIFI_FAIL_NONE = 0, WIFI_FAIL_NOT_FOUND, WIFI_FAIL_REFUSED } wifi_fail_t;

typedef struct {
    char ssid[WIFI_SSID_MAXLEN + 1];
    char pwd[WIFI_PWD_MAXLEN + 1];
} wifi_cred_t;

/* Pure: parse text into creds[], return count (<= max). No IDF deps. */
size_t wifi_creds_parse(const char *buf, size_t len, wifi_cred_t *creds, size_t max);
/* Pure: true if buf is empty or only whitespace/CR/LF. */
bool   wifi_creds_is_blank(const char *buf, size_t len);
/* Pure: write the fill-in template into out, return length (< outsize). */
size_t wifi_creds_template(char *out, size_t outsize);
/* Pure: rebuild file text: working creds on top, failed ones under a
 * "# Failed credentials" header with annotations. results[i] pairs with creds[i]. */
size_t wifi_creds_rewrite(char *out, size_t outsize,
                          const wifi_cred_t *creds, const wifi_fail_t *results, size_t n);

/* LittleFS-backed (IDF). base path "/cfg", partition label "storage", file "wifi.txt". */
esp_err_t wifi_creds_fs_init(void);                       /* mount; format if needed */
esp_err_t wifi_creds_fs_read(char *buf, size_t bufsize, size_t *out_len);
esp_err_t wifi_creds_fs_write(const char *buf, size_t len);
```

```c
/* serial_handler.h additions */
typedef enum { SERIAL_OWNER_NONE = 0, SERIAL_OWNER_USB, SERIAL_OWNER_NET } serial_owner_t;
bool           serial_handler_acquire(serial_owner_t who); /* true if now owner */
void           serial_handler_release(serial_owner_t who); /* no-op unless 'who' holds it */
serial_owner_t serial_handler_owner(void);
void           serial_handler_mark_usb_activity(void);     /* CDC calls this on RX */
void           serial_handler_register_net_data_callback(transport_data_received_cb_t cb);
```

```c
/* status_led.h additions */
typedef enum {
    LED_WIFI_OFF = 0,    /* disarmed: green base */
    LED_WIFI_CONNECTING, /* blinking orange */
    LED_WIFI_CONNECTED,  /* steady/breathing orange */
    LED_WIFI_FAILED      /* slow-blink orange */
} led_wifi_state_t;
void status_led_set_wifi(led_wifi_state_t s);
```

```c
/* wireless.h (public) */
void wireless_init(void);    /* mount FS, ensure wifi.txt (template if blank), push to MSC cache, start arm button */
bool wireless_is_armed(void);
void wireless_arm(void);     /* idempotent: start wifi_sta (+ rfc2217 on connect) */
void wireless_on_creds_written(const char *buf, size_t len); /* called by msc.c on WIFI.TXT host write */

/* wifi_sta.h */
void wifi_sta_start(void);   /* read creds, connect candidates in order, on success start mDNS + rfc2217 */
void wifi_sta_retry(void);   /* re-read creds and try again (after a live wifi.txt edit) */
bool wifi_sta_connected(void);

/* rfc2217.h */
void rfc2217_start(uint16_t port);
void rfc2217_stop(void);

/* arm_button.h */
typedef void (*arm_button_cb_t)(void);
void arm_button_init(int gpio, arm_button_cb_t on_double_click);

/* msc.h addition */
void msc_set_wifi_txt(const char *buf, uint32_t len); /* update WIFI.TXT read cache + dir size */
```

---

## Task 1: `wifi_creds` pure parser + host test (TDD)

**Files:**
- Create: `components/wireless/wifi_creds.h` (full interface block above)
- Create: `components/wireless/wifi_creds.c` (pure functions only in this task; FS functions stubbed in Task 2)
- Test: `components/wireless/test/host/test_wifi_creds.c`

- [ ] **Step 1: Write the failing host test**

Create `components/wireless/test/host/test_wifi_creds.c`:

```c
/* Host-only TDD for the pure wifi.txt parser. Build:
 *   gcc -I.. -o t test_wifi_creds.c ../wifi_creds.c && ./t
 * (the pure functions compile standalone; FS functions are excluded via WIFI_CREDS_HOST_TEST) */
#define WIFI_CREDS_HOST_TEST 1
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "wifi_creds.h"

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(void) {
    wifi_cred_t c[WIFI_CREDS_MAX];

    /* reference file from the spec -> 4 candidates in order #1,#3,#2,#4 */
    const char *ref =
        "# comment\n"
        "SSID=\"net1\"\nPWD=\"pw1\"\n\n"
        "SSID=\"net3\"\nPWD=\"pw3\"\n\n"
        "# Failed credentials\n"
        "SSID=\"net2\" # <-- not found\nPWD=\"pw2\"\n\n"
        "SSID=\"net4\"\nPWD=\"pw4\" # <-- refused\n";
    size_t n = wifi_creds_parse(ref, strlen(ref), c, WIFI_CREDS_MAX);
    assert(n == 4);
    assert(eq(c[0].ssid, "net1") && eq(c[0].pwd, "pw1"));
    assert(eq(c[1].ssid, "net3") && eq(c[1].pwd, "pw3"));
    assert(eq(c[2].ssid, "net2") && eq(c[2].pwd, "pw2"));
    assert(eq(c[3].ssid, "net4") && eq(c[3].pwd, "pw4"));

    /* CRLF tolerated */
    const char *crlf = "SSID=\"a\"\r\nPWD=\"b\"\r\n";
    n = wifi_creds_parse(crlf, strlen(crlf), c, WIFI_CREDS_MAX);
    assert(n == 1 && eq(c[0].ssid, "a") && eq(c[0].pwd, "b"));

    /* consecutive SSID -> last wins */
    const char *ss = "SSID=\"x\"\nSSID=\"y\"\nPWD=\"p\"\n";
    n = wifi_creds_parse(ss, strlen(ss), c, WIFI_CREDS_MAX);
    assert(n == 1 && eq(c[0].ssid, "y") && eq(c[0].pwd, "p"));

    /* consecutive PWD -> first wins */
    const char *pp = "SSID=\"x\"\nPWD=\"p\"\nPWD=\"q\"\n";
    n = wifi_creds_parse(pp, strlen(pp), c, WIFI_CREDS_MAX);
    assert(n == 1 && eq(c[0].pwd, "p"));

    /* '#' inside quotes is part of the password */
    const char *hash = "SSID=\"x\"\nPWD=\"pa#ss\"\n";
    n = wifi_creds_parse(hash, strlen(hash), c, WIFI_CREDS_MAX);
    assert(n == 1 && eq(c[0].pwd, "pa#ss"));

    /* open network */
    const char *open = "SSID=\"x\"\nPWD=\"\"\n";
    n = wifi_creds_parse(open, strlen(open), c, WIFI_CREDS_MAX);
    assert(n == 1 && eq(c[0].pwd, ""));

    /* dangling SSID with no PWD -> dropped */
    const char *dang = "SSID=\"x\"\nPWD=\"p\"\nSSID=\"y\"\n";
    n = wifi_creds_parse(dang, strlen(dang), c, WIFI_CREDS_MAX);
    assert(n == 1 && eq(c[0].ssid, "x"));

    /* blank detection */
    assert(wifi_creds_is_blank("   \r\n  \n", 8));
    assert(!wifi_creds_is_blank("SSID=\"x\"\n", 9));

    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it and watch it fail to compile/link**

Run: `cd components/wireless/test/host && gcc -I../.. -o t test_wifi_creds.c ../../wifi_creds.c`
Expected: FAIL — `wifi_creds.c`/`wifi_creds.h` do not exist yet (or `undefined reference to wifi_creds_parse`).

(No host `gcc`? Skip Step 2/4 and verify on-device via Task 9's `CONFIG_WIRELESS_SELFTEST` instead.)

- [ ] **Step 3: Write `wifi_creds.h`**

Create `components/wireless/wifi_creds.h` with the SPDX header, include guards, and the **full interface block** from "Shared interfaces". Wrap the FS prototypes so the host test can exclude them:

```c
/* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdbool.h>

#define WIFI_CREDS_MAX     8
#define WIFI_SSID_MAXLEN   32
#define WIFI_PWD_MAXLEN    64
#define WIFI_TXT_MAXLEN    4096

typedef enum { WIFI_FAIL_NONE = 0, WIFI_FAIL_NOT_FOUND, WIFI_FAIL_REFUSED } wifi_fail_t;
typedef struct { char ssid[WIFI_SSID_MAXLEN + 1]; char pwd[WIFI_PWD_MAXLEN + 1]; } wifi_cred_t;

#ifdef __cplusplus
extern "C" {
#endif

size_t wifi_creds_parse(const char *buf, size_t len, wifi_cred_t *creds, size_t max);
bool   wifi_creds_is_blank(const char *buf, size_t len);
size_t wifi_creds_template(char *out, size_t outsize);
size_t wifi_creds_rewrite(char *out, size_t outsize,
                          const wifi_cred_t *creds, const wifi_fail_t *results, size_t n);

#ifndef WIFI_CREDS_HOST_TEST
#include "esp_err.h"
esp_err_t wifi_creds_fs_init(void);
esp_err_t wifi_creds_fs_read(char *buf, size_t bufsize, size_t *out_len);
esp_err_t wifi_creds_fs_write(const char *buf, size_t len);
#endif

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Write the pure parser in `wifi_creds.c`**

Create `components/wireless/wifi_creds.c`. In this task include only the pure functions (guard the IDF/FS half so the host build links):

```c
/* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "wifi_creds.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Extract a key="value" from one already-comment-stripped, trimmed line.
 * Returns 1 and copies value (truncated to vmax) if line is key="...". */
static int match_kv(const char *line, const char *key, char *val, size_t vmax)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return 0;
    const char *p = line + klen;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strrchr(p, '"');     /* last quote on the line */
    if (!end || end < p) return 0;
    size_t n = (size_t)(end - p);
    if (n > vmax) n = vmax;
    memcpy(val, p, n);
    val[n] = '\0';
    return 1;
}

/* Copy one LF-delimited line into out (strip trailing CR), drop the comment:
 * everything from the first '#' that is OUTSIDE double quotes. Then trim ends. */
static void clean_line(const char *src, size_t len, char *out, size_t omax)
{
    int in_quotes = 0;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < omax; i++) {
        char ch = src[i];
        if (ch == '\r') continue;
        if (ch == '"') in_quotes = !in_quotes;
        if (ch == '#' && !in_quotes) break;   /* comment start */
        out[o++] = ch;
    }
    /* right-trim */
    while (o > 0 && (out[o-1] == ' ' || out[o-1] == '\t')) o--;
    out[o] = '\0';
    /* left-trim */
    size_t s = 0;
    while (out[s] == ' ' || out[s] == '\t') s++;
    if (s) memmove(out, out + s, strlen(out + s) + 1);
}

size_t wifi_creds_parse(const char *buf, size_t len, wifi_cred_t *creds, size_t max)
{
    size_t count = 0;
    int have_pending = 0;
    char pending_ssid[WIFI_SSID_MAXLEN + 1] = {0};
    char line[256], val[WIFI_PWD_MAXLEN + 1];

    size_t i = 0;
    while (i < len && count < max) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;
        clean_line(buf + i, j - i, line, sizeof(line));
        i = (j < len) ? j + 1 : j;

        if (line[0] == '\0') continue;
        if (match_kv(line, "SSID", val, WIFI_SSID_MAXLEN)) {
            strncpy(pending_ssid, val, sizeof(pending_ssid) - 1);
            pending_ssid[sizeof(pending_ssid) - 1] = '\0';
            have_pending = 1;                     /* last SSID wins */
        } else if (match_kv(line, "PWD", val, WIFI_PWD_MAXLEN)) {
            if (have_pending) {
                strncpy(creds[count].ssid, pending_ssid, WIFI_SSID_MAXLEN);
                creds[count].ssid[WIFI_SSID_MAXLEN] = '\0';
                strncpy(creds[count].pwd, val, WIFI_PWD_MAXLEN);
                creds[count].pwd[WIFI_PWD_MAXLEN] = '\0';
                count++;
                have_pending = 0;                 /* first PWD wins; ignore until next SSID */
            }
        }
    }
    return count;
}

bool wifi_creds_is_blank(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)buf[i])) return false;
    }
    return true;
}

static const char TEMPLATE_TXT[] =
    "# EspProg-S3 WiFi credentials.\n"
    "# Uncomment and fill the lines below (remove the leading '#'), then save.\n"
    "# Add more SSID/PWD pairs to list fallback networks; tried top to bottom.\n"
    "# Use PWD=\"\" for an open network.\n"
    "#\n"
    "#SSID=\"your network name\"\n"
    "#PWD=\"your password\"\n";

size_t wifi_creds_template(char *out, size_t outsize)
{
    size_t n = sizeof(TEMPLATE_TXT) - 1;
    if (n >= outsize) n = outsize - 1;
    memcpy(out, TEMPLATE_TXT, n);
    out[n] = '\0';
    return n;
}

size_t wifi_creds_rewrite(char *out, size_t outsize,
                          const wifi_cred_t *creds, const wifi_fail_t *results, size_t n)
{
    size_t o = 0;
    #define APPENDF(...) do { int _w = snprintf(out + o, outsize - o, __VA_ARGS__); \
                              if (_w > 0 && (size_t)_w < outsize - o) o += (size_t)_w; } while (0)
    /* working / untried first */
    for (size_t i = 0; i < n; i++) {
        if (results[i] == WIFI_FAIL_NONE) {
            APPENDF("SSID=\"%s\"\nPWD=\"%s\"\n\n", creds[i].ssid, creds[i].pwd);
        }
    }
    /* failed section */
    int any_failed = 0;
    for (size_t i = 0; i < n; i++) if (results[i] != WIFI_FAIL_NONE) { any_failed = 1; break; }
    if (any_failed) {
        APPENDF("# Failed credentials\n");
        for (size_t i = 0; i < n; i++) {
            if (results[i] == WIFI_FAIL_NOT_FOUND) {
                APPENDF("SSID=\"%s\" # <-- not found\nPWD=\"%s\"\n\n", creds[i].ssid, creds[i].pwd);
            } else if (results[i] == WIFI_FAIL_REFUSED) {
                APPENDF("SSID=\"%s\"\nPWD=\"%s\" # <-- refused\n\n", creds[i].ssid, creds[i].pwd);
            }
        }
    }
    #undef APPENDF
    return o;
}

#ifndef WIFI_CREDS_HOST_TEST
/* FS-backed functions are implemented in Task 2. */
#endif
```

- [ ] **Step 5: Run the host test — expect PASS**

Run: `cd components/wireless/test/host && gcc -I../.. -DWIFI_CREDS_HOST_TEST -o t test_wifi_creds.c ../../wifi_creds.c && ./t`
Expected: `PASS`

- [ ] **Step 6: Commit**

```bash
git add components/wireless/wifi_creds.h components/wireless/wifi_creds.c components/wireless/test/host/test_wifi_creds.c
git commit -m "feat(wireless): pure wifi.txt parser with host test"
```

---

## Task 2: Component scaffold, Kconfig, LittleFS store, build gate

This task makes the firmware build with the feature enabled (WiFi/LittleFS/mDNS deps resolved) even though nothing is wired yet.

**Files:**
- Create: `components/wireless/Kconfig`, `components/wireless/idf_component.yml`, `components/wireless/CMakeLists.txt`, `components/wireless/include/wireless.h`
- Create: `components/wireless/wireless.c` (init + stubs)
- Modify: `components/wireless/wifi_creds.c` (add the FS section)
- Create: `partitions.csv`
- Modify: `sdkconfig.s3zero.defaults`, `main/CMakeLists.txt`

- [ ] **Step 1: Kconfig**

Create `components/wireless/Kconfig`:

```
menu "Wireless serial (RFC2217)"

    config WIRELESS_SERIAL
        bool "Enable wireless serial bridge (RFC2217 over WiFi STA)"
        default n
        help
            Expose the target UART over WiFi as an RFC2217 server, armed by a
            double-click on the onboard BOOT button. Off by default.

    config WIRELESS_RFC2217_PORT
        int "RFC2217 TCP port"
        default 3333
        depends on WIRELESS_SERIAL

    config WIRELESS_MDNS_HOSTNAME
        string "mDNS hostname (without .local)"
        default "espprog"
        depends on WIRELESS_SERIAL

    config WIRELESS_ARM_GPIO
        int "Arm button GPIO (onboard BOOT)"
        default 0
        depends on WIRELESS_SERIAL

    config WIRELESS_SELFTEST
        bool "Run wifi.txt parser self-test at boot (logs PASS/FAIL)"
        default n
        depends on WIRELESS_SERIAL

endmenu
```

- [ ] **Step 2: Managed deps + CMakeLists**

Create `components/wireless/idf_component.yml`:

```yaml
dependencies:
  joltwallet/littlefs: "^1.14.0"
  espressif/mdns: "^1.4.0"
```

Create `components/wireless/CMakeLists.txt` (REQUIRES is unconditional — `CONFIG_` is empty at requirement-expansion time; gate the C code with `#if` instead, per the build-gotchas memory):

```cmake
idf_component_register(
    SRCS "wireless.c" "wifi_creds.c" "wifi_sta.c" "rfc2217.c" "arm_button.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_wifi esp_event esp_netif mdns littlefs nvs_flash
             lwip esp_driver_gpio esp_timer serial_handler status_led)
```

> The `joltwallet/littlefs` managed component registers as CMake component **`littlefs`** (header `esp_littlefs.h`); `espressif/mdns` registers as **`mdns`**. If Task 2's build reports "component littlefs not found", check the registered name under `managed_components/` and match it in `REQUIRES`.

**Create all headers and no-op stub sources now so every later `pio run` links** (each `pio run` links the whole app — undefined public symbols break the build). Create these headers with SPDX + the matching blocks from "Shared interfaces": `include/wifi_sta.h` (wifi_sta block), `include/rfc2217.h` (rfc2217 block), `include/arm_button.h` (arm_button block). Then create stub sources that *define* every public function as a no-op (Tasks 5–7 replace these bodies):

```c
/* wifi_sta.c (stub) */
#include "wifi_sta.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
void wifi_sta_start(void) {}
void wifi_sta_retry(void) {}
bool wifi_sta_connected(void) { return false; }
#endif
```
```c
/* rfc2217.c (stub) */
#include "rfc2217.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
void rfc2217_start(uint16_t port) { (void)port; }
void rfc2217_stop(void) {}
#endif
```
```c
/* arm_button.c (stub) */
#include "arm_button.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
void arm_button_init(int gpio, arm_button_cb_t cb) { (void)gpio; (void)cb; }
#endif
```

> Put `include/wifi_sta.h`, `include/rfc2217.h`, `include/arm_button.h` under `include/` (matching `INCLUDE_DIRS "include"`). Add `#include <stdint.h>` to `rfc2217.h` for `uint16_t`.

- [ ] **Step 3: Public header + init/stubs**

Create `components/wireless/include/wireless.h` with the SPDX header and the **wireless.h block** from "Shared interfaces".

Create `components/wireless/wireless.c`:

```c
/* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "wireless.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include <string.h>
#include "wifi_creds.h"
#include "wifi_sta.h"
#include "arm_button.h"
#include "msc.h"
#include "esp_log.h"

static const char *TAG = "wireless";
static bool s_armed = false;
static char s_txt[WIFI_TXT_MAXLEN];

static void load_and_publish(void)
{
    size_t len = 0;
    if (wifi_creds_fs_read(s_txt, sizeof(s_txt), &len) != ESP_OK) len = 0;
    if (len == 0 || wifi_creds_is_blank(s_txt, len)) {
        len = wifi_creds_template(s_txt, sizeof(s_txt));
        wifi_creds_fs_write(s_txt, len);
    }
    msc_set_wifi_txt(s_txt, (uint32_t)len);
}

static void on_double_click(void) { wireless_arm(); }

void wireless_init(void)
{
    if (wifi_creds_fs_init() != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS init failed; wireless unavailable");
        return;
    }
    load_and_publish();
    arm_button_init(CONFIG_WIRELESS_ARM_GPIO, on_double_click);
    ESP_LOGI(TAG, "wireless ready (disarmed) — double-click BOOT to arm");
}

bool wireless_is_armed(void) { return s_armed; }

void wireless_arm(void)
{
    if (s_armed) return;
    s_armed = true;
    ESP_LOGI(TAG, "arming WiFi + RFC2217");
    wifi_sta_start();
}

void wireless_on_creds_written(const char *buf, size_t len)
{
    if (len > sizeof(s_txt)) len = sizeof(s_txt);
    memcpy(s_txt, buf, len);
    wifi_creds_fs_write(s_txt, len);
    if (s_armed) wifi_sta_retry();   /* live edit re-triggers a connect attempt */
}
#else
void wireless_init(void) {}
bool wireless_is_armed(void) { return false; }
void wireless_arm(void) {}
void wireless_on_creds_written(const char *b, size_t l) { (void)b; (void)l; }
#endif
```

- [ ] **Step 4: Add the FS section to `wifi_creds.c`**

Append to `components/wireless/wifi_creds.c` (the `#ifndef WIFI_CREDS_HOST_TEST` block from Task 1):

```c
#ifndef WIFI_CREDS_HOST_TEST
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include "esp_littlefs.h"
#include "esp_log.h"
#include <stdio.h>

#define CFG_BASE   "/cfg"
#define CFG_PART   "storage"
#define CFG_FILE   CFG_BASE "/wifi.txt"
static const char *FTAG = "wifi_creds";

esp_err_t wifi_creds_fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_BASE,
        .partition_label = CFG_PART,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) ESP_LOGE(FTAG, "littlefs register failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t wifi_creds_fs_read(char *buf, size_t bufsize, size_t *out_len)
{
    *out_len = 0;
    FILE *f = fopen(CFG_FILE, "rb");
    if (!f) return ESP_OK;   /* missing == empty; caller writes a template */
    size_t n = fread(buf, 1, bufsize - 1, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return ESP_OK;
}

esp_err_t wifi_creds_fs_write(const char *buf, size_t len)
{
    FILE *f = fopen(CFG_FILE, "wb");
    if (!f) { ESP_LOGE(FTAG, "open %s for write failed", CFG_FILE); return ESP_FAIL; }
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    return (w == len) ? ESP_OK : ESP_FAIL;
}
#endif /* CONFIG_WIRELESS_SERIAL */
#endif /* WIFI_CREDS_HOST_TEST */
```

- [ ] **Step 5: Partition table**

Create `partitions.csv` (4 MB flash; 3 MB app leaves the "free flash" headroom, 512 KB LittleFS). Subtype `spiffs` is used for the data partition because `esp_littlefs` mounts by **label** regardless of data subtype, and `spiffs` is a CSV keyword every IDF version understands:

```
# Name,   Type, SubType,  Offset,   Size,    Flags
nvs,      data, nvs,      0x9000,   0x6000,
phy_init, data, phy,      0xf000,   0x1000,
factory,  app,  factory,  0x10000,  0x300000,
storage,  data, spiffs,   0x310000, 0x80000,
```

- [ ] **Step 6: sdkconfig + main dependency**

Add to `sdkconfig.s3zero.defaults`:

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_WIRELESS_SERIAL=y
```

In `main/CMakeLists.txt` line 1, add `"wireless"` to the `dependencies` list:

```cmake
set(dependencies "tinyusb" "esp_timer" "usb" "util" "serial_handler" "debug_probe" "esp_ringbuf" "status_led" "wireless")
```

Add a **temporary no-op** `msc_set_wifi_txt` so `wireless.c` links now (Task 8 replaces it with the real implementation). In `main/msc.h`, inside the `extern "C"` block:

```c
#include <stdint.h>
void msc_set_wifi_txt(const char *buf, uint32_t len);
```

In `main/msc.c`, add a stub near `msc_init` (delete it in Task 8 Step 4):

```c
void msc_set_wifi_txt(const char *buf, uint32_t len) { (void)buf; (void)len; } /* TASK2 STUB - replaced in Task 8 */
```

- [ ] **Step 7: Build gate**

Because `sdkconfig.s3zero.defaults` is consumed via `platformio.ini`'s `cmake_extra_args`, and `idf_component.yml` changed, force a reconfigure: make a trivial edit to `platformio.ini` (e.g. touch a comment), **wait for PlatformIO's background reconfigure to finish**, then:

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS. New managed components `joltwallet__littlefs` and `espressif__mdns` appear under `managed_components/`. The firmware links because every public symbol now has a definition (real `wireless.c`/`wifi_creds.c`, no-op stubs for `wifi_sta`/`rfc2217`/`arm_button`, temporary `msc_set_wifi_txt`).

- [ ] **Step 8: Commit**

```bash
git add components/wireless/ partitions.csv sdkconfig.s3zero.defaults main/CMakeLists.txt main/msc.c main/msc.h
git commit -m "feat(wireless): component scaffold, Kconfig, LittleFS store, partition table"
```

---

## Task 3: `serial_handler` ownership lock + RX routing

**Files:**
- Modify: `components/serial_handler/include/serial_handler.h` (add the owner-lock block from "Shared interfaces")
- Modify: `components/serial_handler/serial_handler.c`
- Modify: `main/serial_bridge.c`

**Policy:** USB-CDC has priority and is "active" for a short window after each CDC RX. NET (RFC2217) acquires the lock for the whole client connection, but only if the UART is free, not flashing, and USB hasn't been active recently. RX bytes route to whoever owns the lock (NET sink if NET owns, else the CDC sink).

- [ ] **Step 1: Header additions**

Add the owner-lock block (from "Shared interfaces") to `serial_handler.h` before the closing `#ifdef __cplusplus`.

- [ ] **Step 2: Implement the lock in `serial_handler.c`**

Add near the top (after includes — `esp_timer.h` is the only new include needed; the lock uses plain `volatile` state, no FreeRTOS primitive):

```c
#include "esp_timer.h"

static volatile serial_owner_t s_owner = SERIAL_OWNER_NONE;
static volatile int64_t s_usb_active_until_us = 0;   /* USB priority window */
#define USB_ACTIVE_WINDOW_US (1500 * 1000)           /* 1.5 s after last CDC RX */
static transport_data_received_cb_t s_net_cb = NULL;

void serial_handler_mark_usb_activity(void)
{
    s_usb_active_until_us = esp_timer_get_time() + USB_ACTIVE_WINDOW_US;
    if (s_owner == SERIAL_OWNER_NONE) s_owner = SERIAL_OWNER_USB;
}

serial_owner_t serial_handler_owner(void) { return s_owner; }

bool serial_handler_acquire(serial_owner_t who)
{
    if (who == SERIAL_OWNER_NET) {
        bool usb_busy = esp_timer_get_time() < s_usb_active_until_us;
        if (serial_handler_is_flashing() || usb_busy) return false;
        if (s_owner == SERIAL_OWNER_USB) return false;
        s_owner = SERIAL_OWNER_NET;
        return true;
    }
    /* USB */
    s_owner = SERIAL_OWNER_USB;
    return true;
}

void serial_handler_release(serial_owner_t who)
{
    if (s_owner == who) s_owner = SERIAL_OWNER_NONE;
}

void serial_handler_register_net_data_callback(transport_data_received_cb_t cb)
{
    s_net_cb = cb;
}
```

- [ ] **Step 3: Route RX by owner at the existing callback call site**

Find where `serial_handler.c` invokes the registered `transport_data_received_cb_t` (the user data callback, in the UART read task). Replace the direct call with owner-based routing. If the existing call is `if (s_data_cb) s_data_cb(data, len);`, change to:

```c
if (s_owner == SERIAL_OWNER_NET && s_net_cb) {
    s_net_cb(data, len);
} else if (s_data_cb) {
    s_data_cb(data, len);
}
```

(Use the actual existing variable name for the registered callback — search for the field set by `serial_handler_register_data_callback`.)

- [ ] **Step 4: Make CDC owner-aware in `serial_bridge.c`**

In `tud_cdc_rx_cb` (line ~108), mark USB activity and only send when USB owns:

```c
    if (rx_size > 0) {
        serial_handler_mark_usb_activity();
        if (serial_handler_owner() == SERIAL_OWNER_USB) {
            serial_handler_send_data(buf, rx_size);
        }
    }
```

The existing `transport_data_received_callback` (target→USB ring) needs no change: when NET owns, RX is routed to the net sink instead and this callback simply isn't called.

- [ ] **Step 5: Build gate**

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS. (USB-CDC behavior unchanged when no NET client connects — `s_owner` defaults to USB on activity.)

- [ ] **Step 6: Commit**

```bash
git add components/serial_handler/ main/serial_bridge.c
git commit -m "feat(serial_handler): UART ownership lock + owner-routed RX"
```

---

## Task 4: `status_led` orange WiFi states

**Files:**
- Modify: `components/status_led/include/status_led.h` (add the LED block from "Shared interfaces")
- Modify: `components/status_led/status_led.c`

- [ ] **Step 1: Header additions**

Add the `led_wifi_state_t` enum + `status_led_set_wifi()` (from "Shared interfaces") to `status_led.h`. When `CONFIG_STATUS_LED_WS2812` is disabled, the stub must still exist.

- [ ] **Step 2: Render orange tint in `status_led.c`**

Add a module variable and setter:

```c
static volatile led_wifi_state_t s_wifi = LED_WIFI_OFF;
void status_led_set_wifi(led_wifi_state_t s) { s_wifi = s; }
```

In the render task, change the **idle/ready base** branch so that when WiFi is engaged the base tint is orange instead of green. Locate the branch that currently draws the ready breathe / idle solid green (around `put(0, (level), 0)` / `put(0, 255, 0)`), and replace the base-color selection with:

```c
        /* WiFi base tint overrides the green idle/ready base; higher-priority
         * events (flashing/debug/error/blips) are handled above this branch. */
        if (s_wifi == LED_WIFI_CONNECTING) {
            /* blink orange ~2 Hz */
            const bool on = ((now / pdMS_TO_TICKS(250)) & 1) == 0;
            if (on) put(255, 90, 0); else put(0, 0, 0);
        } else if (s_wifi == LED_WIFI_FAILED) {
            /* slow blink orange ~0.5 Hz */
            const bool on = ((now / pdMS_TO_TICKS(1000)) & 1) == 0;
            if (on) put(255, 90, 0); else put(0, 0, 0);
        } else if (s_wifi == LED_WIFI_CONNECTED) {
            /* steady orange, gentle breathe like ready */
            const uint32_t pos = now % breathe_ticks;
            const uint32_t half = breathe_ticks / 2;
            uint32_t level = (pos < half) ? (pos * 100 / half) : ((breathe_ticks - pos) * 100 / half);
            if (level < 20) level = 20;
            put((uint8_t)(255 * level / 100), (uint8_t)(90 * level / 100), 0);
        } else {
            /* LED_WIFI_OFF: unchanged green idle/ready behavior */
            <existing green idle/ready code stays here>
        }
```

> Keep the existing flashing/debug/blip/error branches **above** this base-tint logic so they still take priority. `put()` already does the physical R/G swap, so `put(255,90,0)` is orange.

- [ ] **Step 3: Build gate**

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add components/status_led/
git commit -m "feat(status_led): orange WiFi base-tint states"
```

---

## Task 5: `arm_button` GPIO0 double-click watcher

**Files:**
- Modify: `components/wireless/include/arm_button.h` (created in Task 2 — confirm it matches the Shared-interfaces block)
- Modify: `components/wireless/arm_button.c` (replace the no-op stub from Task 2)

- [ ] **Step 1: Implementation (polling task, debounced double-click)**

Write `components/wireless/arm_button.c`:

```c
/* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "arm_button.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "arm_button";
static int s_gpio;
static arm_button_cb_t s_cb;

#define POLL_MS         10
#define DEBOUNCE_MS     20
#define DBLCLICK_MS     400

static void task(void *arg)
{
    int last = 1, stable = 1;
    int64_t last_change_us = 0, first_release_us = 0;
    int clicks = 0;
    while (1) {
        int raw = gpio_get_level(s_gpio);   /* active low: pressed == 0 */
        int64_t now = esp_timer_get_time();
        if (raw != last) { last = raw; last_change_us = now; }
        else if (raw != stable && (now - last_change_us) > DEBOUNCE_MS * 1000) {
            stable = raw;
            if (stable == 0) {              /* press edge */
                if (clicks == 1 && (now - first_release_us) < DBLCLICK_MS * 1000) {
                    clicks = 0;
                    ESP_LOGI(TAG, "double-click -> arm");
                    if (s_cb) s_cb();
                } /* else first press; wait for release */
            } else {                        /* release edge */
                if (clicks == 0) { clicks = 1; first_release_us = now; }
            }
        }
        if (clicks == 1 && (now - first_release_us) > DBLCLICK_MS * 1000) clicks = 0; /* timeout */
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void arm_button_init(int gpio, arm_button_cb_t on_double_click)
{
    s_gpio = gpio;
    s_cb = on_double_click;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    xTaskCreate(task, "arm_button", 2560, NULL, 4, NULL);
}
#endif
```

- [ ] **Step 2: Build gate**

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add components/wireless/arm_button.c
git commit -m "feat(wireless): GPIO0 double-click arm-gesture watcher"
```

---

## Task 6: `wifi_sta` STA manager

**Files:**
- Modify: `components/wireless/include/wifi_sta.h` (created in Task 2 — confirm it matches the Shared-interfaces block; needs `#include <stdbool.h>`)
- Modify: `components/wireless/wifi_sta.c` (replace the no-op stub)

Connects candidates in order; on association starts mDNS + RFC2217 and sets LED connected; on per-candidate failure records the reason and advances; when the list is exhausted, rewrites `wifi.txt` (failed section) and sets LED failed.

- [ ] **Step 1: Implementation**

Replace the stub `components/wireless/wifi_sta.c` with:

```c
/* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "wifi_sta.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include <string.h>
#include "wifi_creds.h"
#include "rfc2217.h"
#include "status_led.h"
#include "msc.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_sta";
static wifi_cred_t   s_creds[WIFI_CREDS_MAX];
static wifi_fail_t   s_results[WIFI_CREDS_MAX];
static size_t        s_n = 0, s_idx = 0;
static bool          s_connected = false;
static bool          s_started = false;
static esp_netif_t  *s_netif = NULL;

static void try_candidate(size_t i)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_creds[i].ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_creds[i].pwd, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = (s_creds[i].pwd[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "trying [%u/%u] SSID=\"%s\"", (unsigned)(i + 1), (unsigned)s_n, s_creds[i].ssid);
    status_led_set_wifi(LED_WIFI_CONNECTING);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}

static void publish_failures(void)
{
    char out[WIFI_TXT_MAXLEN];
    /* keep only entries we actually tested as 'failed'; untested stay NONE */
    size_t len = wifi_creds_rewrite(out, sizeof(out), s_creds, s_results, s_n);
    wifi_creds_fs_write(out, len);
    msc_set_wifi_txt(out, (uint32_t)len);
}

static void evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_n) try_candidate(s_idx = 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_connected) {                       /* drop after being connected -> reconnect */
            s_connected = false;
            status_led_set_wifi(LED_WIFI_CONNECTING);
            esp_wifi_connect();
            return;
        }
        s_results[s_idx] = (d->reason == WIFI_REASON_NO_AP_FOUND)
                           ? WIFI_FAIL_NOT_FOUND : WIFI_FAIL_REFUSED;
        ESP_LOGW(TAG, "candidate %u failed (reason %d)", (unsigned)s_idx, d->reason);
        if (++s_idx < s_n) {
            try_candidate(s_idx);
        } else {
            ESP_LOGW(TAG, "all candidates failed");
            status_led_set_wifi(LED_WIFI_FAILED);
            publish_failures();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_results[s_idx] = WIFI_FAIL_NONE;
        status_led_set_wifi(LED_WIFI_CONNECTED);
        mdns_hostname_set(CONFIG_WIRELESS_MDNS_HOSTNAME);
        mdns_service_add(NULL, "_rfc2217", "_tcp", CONFIG_WIRELESS_RFC2217_PORT, NULL, 0);
        publish_failures();                      /* records which ones failed before this one */
        rfc2217_start(CONFIG_WIRELESS_RFC2217_PORT);
    }
}

static void load_creds(void)
{
    char txt[WIFI_TXT_MAXLEN];
    size_t len = 0;
    wifi_creds_fs_read(txt, sizeof(txt), &len);
    s_n = wifi_creds_parse(txt, len, s_creds, WIFI_CREDS_MAX);
    for (size_t i = 0; i < WIFI_CREDS_MAX; i++) s_results[i] = WIFI_FAIL_NONE;
    s_idx = 0;
}

void wifi_sta_start(void)
{
    if (s_started) { wifi_sta_retry(); return; }
    s_started = true;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, evt, NULL, NULL));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    load_creds();
    ESP_ERROR_CHECK(esp_wifi_start());           /* fires WIFI_EVENT_STA_START -> try_candidate */
}

void wifi_sta_retry(void)
{
    if (!s_started) { wifi_sta_start(); return; }
    esp_wifi_disconnect();
    s_connected = false;
    load_creds();
    if (s_n) try_candidate(s_idx = 0);
}

bool wifi_sta_connected(void) { return s_connected; }
#endif
```

> **Note for the implementer:** to keep the radio off core 0's data path, the WiFi/event tasks are created by IDF; pin tuning (e.g. `CONFIG_ESP_WIFI_TASK_CORE_ID`) is a sdkconfig option, not code. Leave defaults unless on-device testing shows contention.

- [ ] **Step 2: Build gate**

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS. (`rfc2217_start` still the Task-2 no-op stub until Task 7 — links fine.)

- [ ] **Step 3: Commit**

```bash
git add components/wireless/wifi_sta.c
git commit -m "feat(wireless): WiFi STA manager with candidate iteration + mDNS"
```

---

## Task 7: `rfc2217` TCP server

**Files:**
- Modify: `components/wireless/include/rfc2217.h` (created in Task 2 — confirm it matches; needs `#include <stdint.h>`)
- Modify: `components/wireless/rfc2217.c` (replace the no-op stub)

Implements enough RFC2217 (RFC 2217 / Telnet COM-PORT-CONTROL option 44) for esptool/pyserial: data relay plus `SET_BAUDRATE` and `SET_CONTROL` (DTR/RTS), with DTR/RTS mapped to the target through the **same** logic the CDC path uses (`tud_cdc_line_state_cb` in `serial_bridge.c`).

- [ ] **Step 1: Implementation**

Replace the stub `components/wireless/rfc2217.c` with:

```c
/* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "rfc2217.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include <string.h>
#include "serial_handler.h"
#include "debug_probe.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "rfc2217";
static int s_listen = -1, s_client = -1;
static transport_data_received_cb_t s_prev_net_cb;

/* Telnet / RFC2217 constants */
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240
#define COM_PORT_OPT       44
#define SET_BAUDRATE        1
#define SET_CONTROL         5
/* SET_CONTROL values we care about */
#define DTR_ON  8
#define DTR_OFF 9
#define RTS_ON 11
#define RTS_OFF 12

static bool s_dtr = false, s_rts = false;

/* Mirror serial_bridge.c tud_cdc_line_state_cb mapping (minus the 10ms postpone
 * timer; over TCP the spurious DTR&RTS frame esptool emits on USB does not occur). */
static void apply_control(void)
{
    bool rst = true, boot = true;
    if (!s_dtr && s_rts)      { rst = false; boot = true; }
    else if (s_dtr && !s_rts) { rst = true;  boot = false; }
    ESP_LOGI(TAG, "DTR=%d RTS=%d -> BOOT=%d RST=%d", s_dtr, s_rts, boot, rst);
    serial_handler_set_boot_reset_pins(boot, rst);
    if (!rst) serial_handler_set_baudrate(115200);
    if (boot) debug_probe_handle_esp32_tdi_bootstrapping(!rst);
}

/* called by serial_handler when NET owns the UART: target -> socket */
static void net_rx(const uint8_t *data, size_t len)
{
    if (s_client < 0) return;
    /* escape any 0xFF as IAC IAC per telnet */
    uint8_t tmp[1024];
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (o + 2 >= sizeof(tmp)) { send(s_client, tmp, o, 0); o = 0; }
        tmp[o++] = data[i];
        if (data[i] == IAC) tmp[o++] = IAC;
    }
    if (o) send(s_client, tmp, o, 0);
}

/* parse one inbound buffer: strip telnet IAC sequences, forward data to UART */
static void feed(const uint8_t *buf, int n)
{
    static uint8_t sb[16]; static int sbn = 0; static int state = 0; /* 0 data,1 iac,2 opt,3 sb,4 sb-iac */
    uint8_t out[1024]; int o = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = buf[i];
        switch (state) {
        case 0: if (b == IAC) state = 1; else out[o++] = b; break;
        case 1:
            if (b == IAC) { out[o++] = IAC; state = 0; }          /* escaped 0xFF */
            else if (b == SB) { sbn = 0; state = 3; }
            else if (b == DO || b == DONT || b == WILL || b == WONT) state = 2;
            else state = 0;                                       /* other 2-byte cmd */
            break;
        case 2: state = 0; break;                                 /* swallow option byte */
        case 3:
            if (b == IAC) state = 4;
            else if (sbn < (int)sizeof(sb)) sb[sbn++] = b;
            break;
        case 4:
            if (b == SE) {
                /* sb[0]=option(44), sb[1]=cmd, sb[2..]=value */
                if (sbn >= 2 && sb[0] == COM_PORT_OPT) {
                    if (sb[1] == SET_CONTROL && sbn >= 3) {
                        switch (sb[2]) {
                        case DTR_ON:  s_dtr = true;  apply_control(); break;
                        case DTR_OFF: s_dtr = false; apply_control(); break;
                        case RTS_ON:  s_rts = true;  apply_control(); break;
                        case RTS_OFF: s_rts = false; apply_control(); break;
                        default: break;
                        }
                    } else if (sb[1] == SET_BAUDRATE && sbn >= 6) {
                        uint32_t baud = (sb[2] << 24) | (sb[3] << 16) | (sb[4] << 8) | sb[5];
                        if (baud) serial_handler_set_baudrate(baud);
                    }
                }
                state = 0;
            } else { if (sbn < (int)sizeof(sb)) sb[sbn++] = b; state = 3; }
            break;
        }
    }
    if (o > 0) serial_handler_send_data(out, o);
}

static void server_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1; setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(port) };
    if (bind(s_listen, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s_listen, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen failed"); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "RFC2217 listening on :%u", port);
    while (1) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int c = accept(s_listen, (struct sockaddr *)&ca, &cl);
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (!serial_handler_acquire(SERIAL_OWNER_NET)) {
            ESP_LOGW(TAG, "UART busy (USB session) -> refusing client");
            close(c);
            continue;
        }
        s_client = c; s_dtr = s_rts = false;
        serial_handler_register_net_data_callback(net_rx);
        ESP_LOGI(TAG, "client connected");
        uint8_t buf[1024];
        while (1) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            feed(buf, n);
        }
        serial_handler_register_net_data_callback(NULL);
        serial_handler_release(SERIAL_OWNER_NET);
        close(c); s_client = -1;
        ESP_LOGI(TAG, "client disconnected");
    }
}

void rfc2217_start(uint16_t port)
{
    static bool started = false;
    if (started) return;
    started = true;
    (void)s_prev_net_cb;
    xTaskCreate(server_task, "rfc2217", 4096, (void *)(uintptr_t)port, 5, NULL);
}

void rfc2217_stop(void)
{
    if (s_client >= 0) { close(s_client); s_client = -1; }
    if (s_listen >= 0) { close(s_listen); s_listen = -1; }
}
#endif
```

- [ ] **Step 2: Build gate**

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add components/wireless/rfc2217.c
git commit -m "feat(wireless): RFC2217 TCP server (data relay + SET_CONTROL/SET_BAUDRATE)"
```

---

## Task 8: MSC `WIFI.TXT` writable file

**Files:**
- Modify: `main/msc.h` (declare `msc_set_wifi_txt`)
- Modify: `main/msc.c`

Adds cluster 4 + a root-dir entry for `WIFI.TXT`, serves it from a RAM cache, and captures host writes (by LBA) — using the file size the host writes into the root-dir entry as the authoritative length — then hands the content to `wireless_on_creds_written()`.

- [ ] **Step 1: Declare the setter in `msc.h`**

Add (inside the `extern "C"` block):

```c
#include <stdint.h>
void msc_set_wifi_txt(const char *buf, uint32_t len);
```

- [ ] **Step 2: FAT table — add cluster 4**

In `msc.c`, extend `msc_disk_fat_table_sector0` (line 143) with a fourth EOC entry:

```c
static const uint8_t msc_disk_fat_table_sector0[] = {
    0xF8, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, // Cluster 2 - README
    0xFF, 0xFF, // Cluster 3 - PINOUT
    0xFF, 0xFF, // Cluster 4 - WIFI.TXT
};
```

- [ ] **Step 3: Read cache + a root-dir entry for WIFI.TXT**

After the PINOUT externs (line ~166), add the WIFI.TXT RAM cache:

```c
// WIFI.TXT is writable and firmware-owned; content lives in this RAM cache,
// kept in sync with LittleFS by wireless.c via msc_set_wifi_txt().
static uint8_t  msc_disk_wifi[FAT_SECTORS_PER_CLUSTER * FAT_SECTOR_SIZE];
static uint32_t msc_disk_wifi_size = 0;
```

Add a third entry to `msc_disk_root_directory_sector0` (after the pinout entry, before the closing `};`). Attribute `0x20` (archive) so the host treats it as writable:

```c
    // wifi.txt file (writable)
    'W', 'I', 'F', 'I', ' ', ' ', ' ', ' ', 'T', 'X', 'T',
    0x20, // archive attribute (read-write)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x04, 0, // starting cluster
    0, 0, 0, 0, // size: patched at runtime by msc_set_wifi_txt()
```

- [ ] **Step 4: Setter — update cache + patch dir-entry size**

Add near `msc_init`. `WIFI.TXT` is the **4th** 32-byte dir entry (index 3: volume-label, README, PINOUT, WIFI); its size field is at offset `3*32 + 28 = 124`, i.e. `4 * FAT_ROOT_ENTRY_SIZE - 4`:

```c
void msc_set_wifi_txt(const char *buf, uint32_t len)
{
    if (len > sizeof(msc_disk_wifi)) len = sizeof(msc_disk_wifi);
    memcpy(msc_disk_wifi, buf, len);
    msc_disk_wifi_size = len;
    uint8_t *sz = &msc_disk_root_directory_sector0[4 * FAT_ROOT_ENTRY_SIZE - 4]; /* index 3 size @124 */
    sz[0] = len & 0xFF; sz[1] = (len >> 8) & 0xFF; sz[2] = (len >> 16) & 0xFF; sz[3] = (len >> 24) & 0xFF;
}
```

> Use the existing `FAT_ROOT_ENTRY_SIZE` macro (32). The existing PINOUT patch in `msc_init` is at `2*FAT_ROOT_ENTRY_SIZE + 28` (=92, the 3rd entry); WIFI is the next entry, `4*FAT_ROOT_ENTRY_SIZE - 4` (=124). If the macro is named differently, match the README/PINOUT idiom.

- [ ] **Step 5: LBA macros — insert WIFI between PINOUT and ELSE**

Replace the macro block (lines 245-252) so WIFI.TXT gets its own cluster and `IS_LBA_PINOUT` no longer extends to `FIRST_ELSE_SECTOR`:

```c
#define FIRST_PINOUT_SECTOR   (FIRST_README_SECTOR + FAT_SECTORS_PER_CLUSTER)
#define FIRST_WIFI_SECTOR     (FIRST_PINOUT_SECTOR + FAT_SECTORS_PER_CLUSTER)
#define FIRST_ELSE_SECTOR     (FIRST_WIFI_SECTOR + FAT_SECTORS_PER_CLUSTER)
#define IS_LBA_BOOT(lba)      ((lba) < FIRST_FAT_SECTOR)
#define IS_LBA_FAT(lba)       ((lba) >= FIRST_FAT_SECTOR && (lba) < FIRST_ROOT_SECTOR)
#define IS_LBA_ROOT(lba)      ((lba) >= FIRST_ROOT_SECTOR && (lba) < FIRST_README_SECTOR)
#define IS_LBA_README(lba)    ((lba) >= FIRST_README_SECTOR && (lba) < FIRST_PINOUT_SECTOR)
#define IS_LBA_PINOUT(lba)    ((lba) >= FIRST_PINOUT_SECTOR && (lba) < FIRST_WIFI_SECTOR)
#define IS_LBA_WIFI(lba)      ((lba) >= FIRST_WIFI_SECTOR && (lba) < FIRST_ELSE_SECTOR)
#define IS_LBA_ELSE(lba)      ((lba) >= FIRST_ELSE_SECTOR)
```

- [ ] **Step 6: Read — serve WIFI.TXT from the cache**

In `tud_msc_read10_cb`, add a branch after the PINOUT branch (before the closing `}`):

```c
    } else if (IS_LBA_WIFI(lba)) {
        const uint32_t off = (lba - FIRST_WIFI_SECTOR) * FAT_SECTOR_SIZE;
        if (off < msc_disk_wifi_size) {
            addr = msc_disk_wifi + off;
            size = msc_disk_wifi_size - off;
            if (size > FAT_SECTOR_SIZE) size = FAT_SECTOR_SIZE;
        }
    }
```

- [ ] **Step 7: Write — capture WIFI.TXT and the size from the dir entry**

In `tud_msc_write10_cb`, the UF2 magic check is at line ~525. In the **non-UF2 path** (the `else` of the UF2 magic `if`, or where non-UF2 writes are currently discarded), add capture logic. Add file-scope state first:

```c
static uint8_t  s_wifi_wbuf[FAT_SECTORS_PER_CLUSTER * FAT_SECTOR_SIZE];
static uint32_t s_wifi_declared_size = 0;   /* from the root-dir entry the host writes */
static bool     s_wifi_dirty = false;
```

In the write callback, before/around the UF2 handling, route by LBA:

```c
    if (IS_LBA_WIFI(lba)) {
        const uint32_t off = (lba - FIRST_WIFI_SECTOR) * FAT_SECTOR_SIZE + offset;
        if (off + bufsize <= sizeof(s_wifi_wbuf)) {
            memcpy(s_wifi_wbuf + off, buffer, bufsize);
            s_wifi_dirty = true;
        }
        return bufsize;   /* not a UF2 block */
    }
    if (IS_LBA_ROOT(lba)) {
        /* the host rewrites the root dir with WIFI.TXT's new size; grab it.
         * WIFI.TXT is the 4th 32-byte entry (index 3); size field at 3*32+28 = 124. */
        const uint8_t *e = (const uint8_t *)buffer + (4 * 32 - 4);  /* = 124 */
        if (offset == 0 && bufsize >= 4 * 32) {
            s_wifi_declared_size = e[0] | (e[1] << 8) | (e[2] << 16) | ((uint32_t)e[3] << 24);
        }
        /* fall through: root writes are otherwise ignored by the synthetic FS */
    }
```

In `tud_msc_write10_complete_cb` (line ~559), flush a dirty WIFI.TXT to the credential layer:

```c
    if (s_wifi_dirty) {
        s_wifi_dirty = false;
        uint32_t len = s_wifi_declared_size;
        if (len == 0 || len > sizeof(s_wifi_wbuf)) {
            /* no size seen: trim at first NUL as a fallback */
            len = 0; while (len < sizeof(s_wifi_wbuf) && s_wifi_wbuf[len]) len++;
        }
        wireless_on_creds_written((const char *)s_wifi_wbuf, len);
        msc_set_wifi_txt((const char *)s_wifi_wbuf, len);
        s_wifi_declared_size = 0;
    }
```

Add `#include "wireless.h"` to msc.c's includes.

- [ ] **Step 8: Build gate**

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS. This is the first full link of `wireless.c` ↔ `msc.c` (resolves the `msc_set_wifi_txt` / `wireless_on_creds_written` pair).

- [ ] **Step 9: Commit**

```bash
git add main/msc.c main/msc.h
git commit -m "feat(msc): writable WIFI.TXT mirror (read cache + host-write capture)"
```

---

## Task 9: Wire it together in `main.c` (+ optional self-test)

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Call `wireless_init()` at boot**

In `main.c`, add `#include "wireless.h"`. In `app_main()`, after `serial_bridge_init()` and `msc_init()` (so the MSC cache exists before WIFI.TXT is published), add:

```c
    wireless_init();   /* mounts LittleFS, ensures WIFI.TXT, starts the arm-button watcher */
```

No other wiring is needed: the arm-button callback registered inside `wireless_init()` calls `wireless_arm()`, which starts `wifi_sta` → `rfc2217`.

- [ ] **Step 2: Optional boot self-test for the parser (no host gcc)**

If you couldn't run the host test, add behind the flag in `wireless_init()` (top of the function):

```c
#if CONFIG_WIRELESS_SELFTEST
    {
        wifi_cred_t c[WIFI_CREDS_MAX];
        const char *ref = "SSID=\"a\"\nSSID=\"b\"\nPWD=\"p\"\nPWD=\"q\"\n";
        size_t n = wifi_creds_parse(ref, strlen(ref), c, WIFI_CREDS_MAX);
        ESP_LOGI(TAG, "SELFTEST parse: %s",
                 (n == 1 && !strcmp(c[0].ssid, "b") && !strcmp(c[0].pwd, "p")) ? "PASS" : "FAIL");
    }
#endif
```

- [ ] **Step 3: Build gate**

Run: `pio run -e s3zero_jtag`
Expected: SUCCESS — full feature compiles and links.

- [ ] **Step 4: Commit**

```bash
git add main/main.c
git commit -m "feat(wireless): wire wireless_init into app_main"
```

---

## Task 10: On-device acceptance (run when the S3-Zero is connected)

> The board is currently unplugged. Run this task once it's plugged into the bridge's flashing UART (or use `pio run -t nobuild -t upload` after a build). Capture monitor output with UTF-8 so esptool's progress doesn't truncate the log: `pio device monitor` (or set `$env:PYTHONIOENCODING='utf-8'; chcp 65001` first, per the build-gotchas memory).

- [ ] **Step 1: Flash & confirm boot**

Run: `pio run -e s3zero_jtag -t upload` then `pio device monitor -e s3zero_jtag`
Expected: log line `wireless ready (disarmed) — double-click BOOT to arm`; LED green (idle/ready). `D:\WIFI.TXT` shows the template (or your creds if previously set).

- [ ] **Step 2: Set credentials**

Edit `D:\WIFI.TXT`, add a real `SSID=`/`PWD=` pair, save.
Expected: log `arming`/`trying` is NOT yet shown (not armed); content persisted.

- [ ] **Step 3: Arm**

Double-click the onboard BOOT button.
Expected: log `double-click -> arm`, then `trying [1/n] SSID="..."`; LED blinking orange → on success `connected, IP ...` and steady orange; `_rfc2217._tcp` advertised; `espprog.local` resolves (`ping espprog.local`).

- [ ] **Step 4: Flash the target over WiFi**

Run: `esptool --port rfc2217://espprog.local:3333 --baud 460800 chip_id`
Expected: esptool connects, toggles DTR/RTS (monitor shows `DTR=.. RTS=.. -> BOOT=.. RST=..`), reads chip id. Then a real `write_flash` succeeds; LED shows blue during flash, back to orange after.

- [ ] **Step 5: Failed-credential write-back**

Add a deliberately wrong `PWD=` for a real SSID (or a non-existent SSID), save, double-click again (or it retries). After it settles, **unplug & replug**, reopen `D:\WIFI.TXT`.
Expected: the bad entry now sits under `# Failed credentials` with `# <-- refused` or `# <-- not found`.

- [ ] **Step 6: Exclusive lock**

Open a USB serial monitor on the bridge's CDC port, then connect `rfc2217://espprog.local:3333` while typing on USB.
Expected: the network client is refused (log `UART busy (USB session) -> refusing client`); no garbled bytes. Close USB, reconnect network → works.

- [ ] **Step 7: Power-cycle disarms**

Unplug/replug power.
Expected: boots disarmed, LED green, WiFi off until you double-click again.

- [ ] **Step 8: Commit any on-device fixes**

```bash
git add -A && git commit -m "fix(wireless): on-device tuning from acceptance testing"
```

---

## Self-review notes (author)

- **Spec coverage:** §3 UX → Tasks 5,6,7,9,10. §4 parser/format → Task 1. §4.1 storage/MSC mirror → Tasks 2,8. §4.4 failed-section rewrite → Task 1 (`wifi_creds_rewrite`) + Task 6 (`publish_failures`). §4.5 template → Task 1/2. §5 architecture → all. §6 arm gesture → Task 5. §7 LED → Task 4. §8 data flow → Tasks 6/7/8/9. §9 UART lock → Task 3. §10 security (open port) → inherent (no auth); documented in pinout.md. §11 error handling → Tasks 2 (FS fail), 6 (all-fail/reconnect). §12 build/config → Task 2. §13 verification → Tasks 1 + 10.
- **Manual refresh (§3.5):** firmware-written changes appear after replug — Task 8 serves from the cache, which `publish_failures()` updates; the host only re-reads on remount. Matches spec.
- **Known on-device-tuning risks (flagged, not placeholders):** exact `serial_handler` RX call-site variable name (Task 3 Step 3); the root-dir size-capture offset assumes WIFI.TXT is the **4th** entry / index 3 (offset 124 — true given Task 8 appends it last); `littlefs` REQUIRES name (Task 2); WPA3-only networks may need `WIFI_AUTH_WPA2_WPA3_PSK` — adjust in `try_candidate` if an AP refuses. These are real integration points, verified during Task 10.

## Deviations made during execution (Tasks 1–9, committed)

Two changes from the plan as written, made because the build surfaced them:

1. **Inverted the WIFI.TXT cache ownership.** The plan had `main/msc.c` own the cache and `wireless`/`wifi_sta` call `msc_set_wifi_txt()`. That made the `wireless` *component* `#include "msc.h"` (from `main`) — a backwards dependency that fails to compile (components can't depend on `main`). Fixed by having the **`wireless` component own the cache** and expose `wireless_wifi_txt()` (getter) + `wireless_update_wifi_txt()` (firmware rewrite); `main/msc.c` (which already depends on `wireless`) *reads* from it, refreshing the WIFI.TXT dir-entry size on each root-dir read. `msc_set_wifi_txt()` no longer exists. Net behavior is identical; only the dependency direction changed (now strictly main → wireless).
2. **Dropped the `debug_probe_handle_esp32_tdi_bootstrapping()` call from `rfc2217.c`.** Keeping it would make `wireless` REQUIRE `debug_probe` (another reconfigure + coupling) for an ESP32-classic-only JTAG flash-voltage edge case. The essential DTR/RTS→BOOT/RST + baud mapping (mirroring `tud_cdc_line_state_cb`) is preserved. If wireless-flashing a bare ESP32 over JTAG-shared wiring ever needs it, add `debug_probe` to the `wireless` REQUIRES and restore the call in `apply_control()`.

All nine buildable tasks compile and link (`pio run -e s3zero_jtag` → SUCCESS; flash 22.2%, RAM 20.8%). Task 10 (on-device acceptance) is pending hardware.
