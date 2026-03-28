/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#pragma once
#include "esp_err.h"

esp_err_t web_server_start(void);
void      web_server_stop(void);
