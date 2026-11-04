/*
 * Copyright 2026 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "porting_assistant.h"

/** Thresholds used to bucket the synthetic-workload execution time into a qualitative score.
 * Values were chosen experimentally against morse reference platforms.
 * The recursion depth and the number of loop-body iterations determine the threshold values
 * If either is modified, the threshold values should be re-evaluated accordingly.
 */
/* Threshold based on only execution time */
#define HIGH_PERF_EXECUTION_TIME_THRESHOLD_MS 600
#define LOW_PERF_EXECUTION_TIME_THRESHOLD_MS  8000

/* Threshold based on only execution time x cpu clock */
#define HIGH_PERF_EXECUTION_TIME_X_CPU_CLOCK_THRESHOLD 60000
#define LOW_PERF_EXECUTION_TIME_X_CPU_CLOCK_THRESHOLD  500000

/** Recursion depth used to exercise the call stack, not just CPU/memory throughput. */
#define LONG_DEEP_FUNC_DEPTH 10

/** Inline computation block macros below to inflate the compiled code size so the test also
 * stresses the instruction fetch path */
#define LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)            \
    memcpy(buf, s, sizeof(s));                                  \
    acc += buf[0];                                              \
    a = a * b + c - d;                                          \
    b = b ^ (a << 1);                                           \
    c = c + (b >> 1) - a;                                       \
    d = d * c ^ b;                                              \
    acc += a + b - c * d;                                       \
    acc ^= (acc << 3) | (acc >> 5);                             \
    acc += (a * b) ^ (c + d);                                   \
    acc -= (b ^ c) + (d - a);                                   \
    acc *= (c | d) ^ (a & b);                                   \
    {                                                           \
        uint32_t v = acc;                                       \
        v = v - ((v >> 1) & 0x55555555);                        \
        v = (v & 0x33333333) + ((v >> 2) & 0x33333333);         \
        v = (((v + (v >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24; \
        acc += v * a;                                           \
    }

#define LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s) \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)       \
    LONG_DEEP_FUNC_BODY(a, b, c, d, acc, buf, s)

#define LONG_DEEP_FUNC_BODY100(a, b, c, d, acc, buf, s) \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)      \
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, s)

enum cpu_mem_benchmark_result
{
    CPU_MEM_BENCHMARK_GOOD,
    CPU_MEM_BENCHMARK_MEDIUM,
    CPU_MEM_BENCHMARK_POOR,
    CPU_MEM_BENCHMARK_UNKNOWN
};

static void execute_long_and_deep_func_inner(uint32_t depth)
{
    /* Consume stack memory, but not too big to cause stack overflow */
    static const char description[] = "Simulate computations";
    char buf[sizeof(description)];
    /* `volatile` prevents the compiler from hoisting/eliminating these, ensuring
     * real loads/stores occur each iteration. */
    volatile uint32_t a = depth, b = depth * 2, c = depth * 3, d = depth * 4;
    volatile uint32_t acc = 0;

    if (depth == 0)
    {
        return;
    }

    /* Number of iterations and bodies are tuned against running time and accuracy */
    for (int i = 0; i < 100; i++)
    {
        LONG_DEEP_FUNC_BODY100(a, b, c, d, acc, buf, description)
        LONG_DEEP_FUNC_BODY100(a, b, c, d, acc, buf, description)
        LONG_DEEP_FUNC_BODY100(a, b, c, d, acc, buf, description)
        LONG_DEEP_FUNC_BODY100(a, b, c, d, acc, buf, description)
        LONG_DEEP_FUNC_BODY100(a, b, c, d, acc, buf, description)
    }
    /* Deliberately recurse into this function to force multiple
     * stack frames to stay active. This lowers the possibility
     * of a caching mechanism artificially speeding up this call,
     * as this test is trying to pressure any instruction caching
     * that might help speed this up.
     */
    execute_long_and_deep_func_inner(--depth);
    /* Do one execution after the recursion to ensure the compiler
     * cannot optimise this away.
     */
    LONG_DEEP_FUNC_BODY10(a, b, c, d, acc, buf, description)
}

/** Entry point for stress-testing deep call stack with recursion to a fixed depth. */
static void execute_long_and_deep_func(void)
{
    execute_long_and_deep_func_inner(LONG_DEEP_FUNC_DEPTH);
}

static const char *benchmark_result_to_str(enum cpu_mem_benchmark_result result)
{
    switch (result)
    {
        case CPU_MEM_BENCHMARK_GOOD:
            return "Good";

        case CPU_MEM_BENCHMARK_MEDIUM:
            return "Medium";

        case CPU_MEM_BENCHMARK_POOR:
            return "Poor";

        default:
            return "N/A";
    }
}

extern const uint32_t mmhal_system_clock;

static enum cpu_mem_benchmark_result evaluate_benchmark_result(uint32_t execution_time)
{
    if (execution_time <= HIGH_PERF_EXECUTION_TIME_THRESHOLD_MS)
    {
        /* The execution time is small enough to pass regardless of CPU speed */
        return CPU_MEM_BENCHMARK_GOOD;
    }
    else if (execution_time > LOW_PERF_EXECUTION_TIME_THRESHOLD_MS)
    {
        /* The execution time is too big. Investigation is needed */
        return CPU_MEM_BENCHMARK_POOR;
    }

    /* Let's check whether the execution time is acceptable compared to CPU speed */
    uint32_t cpu_speed_mhz = mmhal_system_clock / 1000000;
    if (!cpu_speed_mhz)
    {
        return CPU_MEM_BENCHMARK_UNKNOWN;
    }

    uint32_t execution_time_x_cpu_speed = execution_time * cpu_speed_mhz;
    if (execution_time_x_cpu_speed <= HIGH_PERF_EXECUTION_TIME_X_CPU_CLOCK_THRESHOLD)
    {
        /* The execution time is relatively fine compared to CPU speed */
        return CPU_MEM_BENCHMARK_GOOD;
    }
    else if (execution_time_x_cpu_speed > LOW_PERF_EXECUTION_TIME_X_CPU_CLOCK_THRESHOLD)
    {
        /* The execution time is relatively poor compared to CPU speed */
        return CPU_MEM_BENCHMARK_POOR;
    }

    /* The execution time is acceptable compared to CPU speed */
    return CPU_MEM_BENCHMARK_MEDIUM;
}

TEST_STEP(test_step_benchmark_cpu_mem_stack_perf, "Execute CPU/Memory/Stack benchmark function")
{
    uint32_t start_time = mmosal_get_time_ms();
    execute_long_and_deep_func();
    uint32_t execution_time = mmosal_get_time_ms() - start_time;

    enum cpu_mem_benchmark_result result = evaluate_benchmark_result(execution_time);

    TEST_LOG_APPEND("\tExecution time: %lu ms\t(%s, mmhal_system_clock = %lu MHz)\n",
                    execution_time,
                    benchmark_result_to_str(result),
                    (mmhal_system_clock / 1000000));

    if (result == CPU_MEM_BENCHMARK_POOR)
    {
        TEST_LOG_APPEND("\n\tRelatively slow compared to CPU speed. Please check system bus and "
                        "memory performance.\n");
        return TEST_FAILED;
    }
    else if (result == CPU_MEM_BENCHMARK_UNKNOWN)
    {
        TEST_LOG_APPEND("\n\tPlease check whether 'mmhal_system_clock' is valid\n");
        return TEST_FAILED;
    }

    return TEST_PASSED;
}
