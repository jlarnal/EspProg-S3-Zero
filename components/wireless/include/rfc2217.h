/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rfc2217_start(uint16_t port);
void rfc2217_stop(void);

#ifdef __cplusplus
}
#endif
