/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */
/* Minimal assertions for host unit tests. Include once per test binary. */
#pragma once
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_DONE() do { \
    printf("%s: %d passed, %d failed\n", __FILE__, g_pass, g_fail); \
    return g_fail ? 1 : 0; \
} while (0)
