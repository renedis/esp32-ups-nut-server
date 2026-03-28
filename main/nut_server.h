/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#pragma once
#include "esp_err.h"

esp_err_t nut_server_init(void);
void      nut_server_stop(void);
void      nut_server_task(void *arg);
