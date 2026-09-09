#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>

/**
 * \file benchmark_strcopy.c
 * \brief High-precision benchmark comparing strlcpy vs snprintf vs strlen+memcpy
 *        under production C23 / -O3 compilation flags used by evhttp.
 */

static inline void do_not_optimize(const void* p) {
    __asm__ volatile("" : : "g"(p) : "memory");
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// -----------------------------------------------------------------------------
// Copy Implementations
// -----------------------------------------------------------------------------

// Method 1: BSD / POSIX.1-2024 / glibc 2.38+ strlcpy
static inline void copy_via_strlcpy(char *dest, size_t dest_size, const char *src) {
    (void)strlcpy(dest, src, dest_size);
}

// Method 2: snprintf (idiom heavily used in evhttp)
static inline void copy_via_snprintf(char *dest, size_t dest_size, const char *src) {
    (void)snprintf(dest, dest_size, "%s", src);
}

// Method 3: strlen + bounds clamp + memcpy + null terminator (user's proposal)
static inline void copy_via_strlen_memcpy(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) return;
    size_t len = strlen(src);
    if (len >= dest_size) {
        len = dest_size - 1;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

// Method 4: POSIX stpncpy (common Linux idiom)
static inline void copy_via_stpncpy(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) return;
    char *end = stpncpy(dest, src, dest_size - 1);
    *end = '\0';
}

// Method 5: Linux kernel strscpy (single-pass bounded scalar copy)
static inline void copy_via_strscpy(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) return;
    size_t i = 0;
    while (i < dest_size) {
        char c = src[i];
        dest[i] = c;
        if (c == '\0') return;
        i++;
    }
    dest[dest_size - 1] = '\0';
}

// Method 6: Linux kernel strscpy (64-bit word-at-a-time optimized)
static inline void copy_via_strscpy_word(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) return;
    size_t max = dest_size;
    const uint64_t *s64 = (const uint64_t *)src;
    uint64_t *d64 = (uint64_t *)dest;

    if ((((uintptr_t)src | (uintptr_t)dest) & 7) == 0) {
        while (max >= 8) {
            uint64_t w = *s64;
            uint64_t has_zero = (w - 0x0101010101010101ULL) & ~w & 0x8080808080808080ULL;
            if (has_zero) {
                char *d = (char *)d64;
                const char *s = (const char *)s64;
                for (int i = 0; i < 8; i++) {
                    d[i] = s[i];
                    if (s[i] == '\0') return;
                }
            }
            *d64++ = w;
            s64++;
            max -= 8;
        }
    }

    char *d = (char *)d64;
    const char *s = (const char *)s64;
    for (size_t i = 0; i < max; i++) {
        d[i] = s[i];
        if (s[i] == '\0') return;
    }
    dest[dest_size - 1] = '\0';
}

// -----------------------------------------------------------------------------
// Correctness Verification
// -----------------------------------------------------------------------------

static void verify_correctness(void) {
    const char *test_src = "c133eb18-67ea-4c53-b3be-20c8a273414e"; // 36 chars
    char buf1[64] = {0};
    char buf2[64] = {0};
    char buf3[64] = {0};
    char buf4[64] = {0};
    char buf5[64] = {0};
    char buf6[64] = {0};

    copy_via_strlcpy(buf1, sizeof(buf1), test_src);
    copy_via_snprintf(buf2, sizeof(buf2), test_src);
    copy_via_strlen_memcpy(buf3, sizeof(buf3), test_src);
    copy_via_stpncpy(buf4, sizeof(buf4), test_src);
    copy_via_strscpy(buf5, sizeof(buf5), test_src);
    copy_via_strscpy_word(buf6, sizeof(buf6), test_src);

    assert(strcmp(buf1, test_src) == 0);
    assert(strcmp(buf2, test_src) == 0);
    assert(strcmp(buf3, test_src) == 0);
    assert(strcmp(buf4, test_src) == 0);
    assert(strcmp(buf5, test_src) == 0);
    assert(strcmp(buf6, test_src) == 0);

    // Truncation verification (36 chars into 16-byte buffer)
    char trunc1[16] = {0};
    char trunc2[16] = {0};
    char trunc3[16] = {0};
    char trunc4[16] = {0};
    char trunc5[16] = {0};
    char trunc6[16] = {0};

    copy_via_strlcpy(trunc1, sizeof(trunc1), test_src);
    copy_via_snprintf(trunc2, sizeof(trunc2), test_src);
    copy_via_strlen_memcpy(trunc3, sizeof(trunc3), test_src);
    copy_via_stpncpy(trunc4, sizeof(trunc4), test_src);
    copy_via_strscpy(trunc5, sizeof(trunc5), test_src);
    copy_via_strscpy_word(trunc6, sizeof(trunc6), test_src);

    assert(strlen(trunc1) == 15);
    assert(strlen(trunc2) == 15);
    assert(strlen(trunc3) == 15);
    assert(strlen(trunc4) == 15);
    assert(strlen(trunc5) == 15);
    assert(strlen(trunc6) == 15);
    assert(strcmp(trunc1, trunc2) == 0);
    assert(strcmp(trunc2, trunc3) == 0);
    assert(strcmp(trunc3, trunc4) == 0);
    assert(strcmp(trunc4, trunc5) == 0);
    assert(strcmp(trunc5, trunc6) == 0);
}

// -----------------------------------------------------------------------------
// Benchmark Runner Macros & Harness
// -----------------------------------------------------------------------------

typedef struct {
    const char *name;
    uint64_t    min_ns;
    double      ns_per_op;
    double      mops;
} BenchResult;

typedef void (*copy_fn)(char *dest, size_t dest_size, const char *src);

#define BENCHMARK_LOOP(fn_call, dest_buf, dest_sz, src_str, iterations, out_ns) \
    do { \
        uint64_t best_ns = UINT64_MAX; \
        constexpr int ROUNDS = 5; \
        for (int r = 0; r < ROUNDS; ++r) { \
            /* Warmup */ \
            for (size_t w = 0; w < 50000; ++w) { \
                fn_call(dest_buf, dest_sz, src_str); \
                do_not_optimize(dest_buf); \
            } \
            uint64_t t0 = get_time_ns(); \
            for (size_t i = 0; i < (iterations); ++i) { \
                fn_call(dest_buf, dest_sz, src_str); \
                do_not_optimize(dest_buf); \
            } \
            uint64_t t1 = get_time_ns(); \
            uint64_t diff = t1 - t0; \
            if (diff < best_ns) best_ns = diff; \
        } \
        (out_ns) = best_ns; \
    } while (0)

static void run_scenario(
    const char *scenario_desc,
    const char *src_str,
    size_t dest_size,
    size_t iterations
) {
    printf("\n========================================================================================================\n");
    printf(" SCENARIO: %s\n", scenario_desc);
    printf(" Source Length: %zu chars | Dest Buffer: %zu bytes | Iterations: %zu\n", 
           strlen(src_str), dest_size, iterations);
    printf("========================================================================================================\n");
    printf("%-26s | %12s | %12s | %12s | %10s\n", 
           "Method", "Best Time", "ns / op", "MOps / sec", "vs snprintf");
    printf("---------------------------+--------------+--------------+--------------+------------\n");

    char *dest = (char*)malloc(dest_size + 16);
    if (!dest) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    struct {
        const char *name;
        copy_fn     fn;
    } candidates[] = {
        { "snprintf (\"%s\")",      copy_via_snprintf },
        { "strlcpy",                copy_via_strlcpy },
        { "strlen + memcpy",        copy_via_strlen_memcpy },
        { "stpncpy",                copy_via_stpncpy },
        { "strscpy (Linux kernel)", copy_via_strscpy },
        { "strscpy (word64)",       copy_via_strscpy_word }
    };
    constexpr size_t NUM_CANDIDATES = sizeof(candidates) / sizeof(candidates[0]);

    BenchResult results[NUM_CANDIDATES];
    double baseline_ns_per_op = 0.0;

    for (size_t c = 0; c < NUM_CANDIDATES; ++c) {
        uint64_t elapsed_ns = 0;
        copy_fn f = candidates[c].fn;
        BENCHMARK_LOOP(f, dest, dest_size, src_str, iterations, elapsed_ns);

        double ns_op = (double)elapsed_ns / (double)iterations;
        double mops = ((double)iterations / ((double)elapsed_ns / 1e9)) / 1e6;

        results[c].name = candidates[c].name;
        results[c].min_ns = elapsed_ns;
        results[c].ns_per_op = ns_op;
        results[c].mops = mops;

        if (c == 0) {
            baseline_ns_per_op = ns_op;
        }

        double speedup = baseline_ns_per_op / ns_op;
        printf("%-26s | %9.2f ms | %9.2f ns | %9.2f M/s | %8.2fx\n",
               results[c].name,
               (double)results[c].min_ns / 1e6,
               results[c].ns_per_op,
               results[c].mops,
               speedup);
    }

    free(dest);
}

int main(void) {
    verify_correctness();

    printf("\n########################################################################################################\n");
    printf("   evhttp High-Performance String Copy Microbenchmark\n");
    printf("   Toolchain : gcc (C23 / -std=gnu2x) -O3 -march=x86-64-v3 -D_GNU_SOURCE\n");
    printf("   Clock     : CLOCK_MONOTONIC (Best of 5 rounds after 50k warmup iterations)\n");
    printf("########################################################################################################\n");

    // 1. Tiny String: e.g. Customer ID ("ALFKI") - 5 chars into 16-byte buffer
    run_scenario(
        "1. Tiny String (Customer ID / Action Tag)",
        "ALFKI",
        16,
        10000000
    );

    // 2. Short String: UUIDv4 / Session ID - 36 chars into 64-byte buffer
    run_scenario(
        "2. Short String (UUIDv4 Session ID / Client IP / Username)",
        "c133eb18-67ea-4c53-b3be-20c8a273414e",
        64,
        10000000
    );

    // 3. Medium String: URL / Path / Bearer Token - 92 chars into 256-byte buffer
    run_scenario(
        "3. Medium String (API Route / Full URL / Query String)",
        "/api/v1/customer/orders?filter=active&start_date=1994-01-01&end_date=1996-12-31&sort=asc&limit=50",
        256,
        5000000
    );

    // 4. Truncation Scenario: 92 chars into 32-byte buffer (Tests bounds clamp behavior)
    run_scenario(
        "4. Truncation Scenario (92-char URI clamped into 32-byte buffer)",
        "/api/v1/customer/orders?filter=active&start_date=1994-01-01&end_date=1996-12-31&sort=asc&limit=50",
        32,
        5000000
    );

    // 5. Long String: Database Connection String / Error Detail - 350 chars into 1024-byte buffer
    run_scenario(
        "5. Long String (ODBC Connection String / Diagnostics / JSON snippet)",
        "Driver={ODBC Driver 18 for SQL Server};Server=tcp:demodb.database.windows.net,1433;Database=testdb;"
        "Uid=apiserver_worker;Pwd=ComplexPass!2026;Encrypt=yes;TrustServerCertificate=no;Connection Timeout=30;"
        "APP=evhttp-apiserver-worker-pool;WSID=core-reactor-node-01;MultipleActiveResultSets=false;"
        "Packet Size=32768;Language=us_english;ApplicationIntent=ReadOnly;",
        1024,
        2000000
    );

    printf("\n========================================================================================================\n");
    printf(" Benchmark Completed Successfully.\n");
    printf("========================================================================================================\n\n");

    return EXIT_SUCCESS;
}
