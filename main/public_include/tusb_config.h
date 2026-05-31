/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

# pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_DEBUG              0

#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS                 OPT_OS_FREERTOS

#define CFG_TUD_ENDPOINT0_SIZE      64

// DMA Mode has a priority over Slave/IRQ mode and will be used if hardware supports it
// Slave/IRQ mode has issue with handling zero length packets
#define CFG_TUD_DWC2_DMA_ENABLE     1       // Enable DMA

#define CFG_TUD_CDC                 1
/* Larger than one FS bulk packet (64B) so the CDC FIFOs buffer several packets:
 * the bridge serializes one UART <-> CDC pump, so deep FIFOs cut flush/pace churn
 * and give slack against priority-5 task contention on the S3-Zero. */
#define CFG_TUD_CDC_RX_BUFSIZE      512
#define CFG_TUD_CDC_TX_BUFSIZE      512

#define CFG_TUD_MSC                 1
#define CFG_TUD_MSC_BUFSIZE         512

#define CFG_TUD_VENDOR              0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0

#ifdef __cplusplus
}
#endif
