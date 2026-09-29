// Minimal test harness for tests/host: CHECK() records a failure and continues; TEST_DONE() prints the
// verdict and returns the process exit code.
#pragma once
#include <cstdio>

static int g_checks = 0, g_failures = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define TEST_DONE(name) (std::printf("%-6s %s: %d checks, %d failed (%zu-bit)\n", \
    g_failures ? "FAIL" : "ok", name, g_checks, g_failures, sizeof(void *) * 8), g_failures ? 1 : 0)
