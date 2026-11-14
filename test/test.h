#pragma once
#include <stdio.h>

extern int g_checks, g_failed;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    g_checks++; \
    if (_a != _b) { g_failed++; printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
                                       __FILE__, __LINE__, #a, #b, _a, _b); } \
} while (0)

void test_core(void);
void test_net_util(void);
