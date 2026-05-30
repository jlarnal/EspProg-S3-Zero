/* Host-only TDD for the pure wifi.txt parser. Build:
 *   gcc -I../.. -o t test_wifi_creds.c ../../wifi_creds.c && ./t
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
