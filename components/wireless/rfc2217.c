/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rfc2217.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
/* Stub — real implementation lands in the RFC2217 server task. */
void rfc2217_start(uint16_t port) { (void)port; }
void rfc2217_stop(void) {}
#endif
