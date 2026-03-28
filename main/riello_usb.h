/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#pragma once
#include "esp_err.h"

/* -----------------------------------------------------------------------
 * Riello USB driver — non-HID bulk-transfer UPS protocol.
 *
 * Registers a second USB host client alongside the HID host.
 * Handles VID:PID 0x04b4:0x5500 (Cypress-based Riello UPS interface).
 * Writes variables directly into the shared ups_driver variable store.
 *
 * Call riello_usb_init() BEFORE ups_driver_init() so the Riello client
 * is registered first and claims the interface before the HID host sees it.
 * ----------------------------------------------------------------------- */

esp_err_t riello_usb_init(void);
void      riello_usb_task(void *arg);
