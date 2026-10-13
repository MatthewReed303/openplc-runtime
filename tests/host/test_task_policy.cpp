// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

// Host test: IEC TASK priority to SCHED_FIFO mapping.

#include "task_policy.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, what)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::printf("  FAIL  %s\n", what);                                                     \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

int main()
{
    std::printf("task_policy: IEC priority mapping\n");
    bool clamped = true;

    CHECK(plc_task_fifo_priority(0, &clamped) == 49 && !clamped, "IEC 0 -> FIFO 49");
    CHECK(plc_task_fifo_priority(1, &clamped) == 48 && !clamped, "IEC 1 -> FIFO 48");
    CHECK(plc_task_fifo_priority(48, &clamped) == 1 && !clamped, "IEC 48 -> FIFO 1");
    CHECK(plc_task_fifo_priority(-1, &clamped) == 49 && clamped, "IEC -1 clamps to FIFO 49");
    CHECK(plc_task_fifo_priority(-1000, &clamped) == 49 && clamped, "IEC -1000 clamps to 49");
    CHECK(plc_task_fifo_priority(49, &clamped) == 1 && clamped, "IEC 49 clamps to FIFO 1");
    CHECK(plc_task_fifo_priority(1000, &clamped) == 1 && clamped, "IEC 1000 clamps to FIFO 1");
    CHECK(plc_task_fifo_priority(10, nullptr) == 39, "NULL clamped flag accepted");

    for (int p = -100; p <= 100; ++p)
    {
        int f = plc_task_fifo_priority(p, nullptr);
        if (f < 1 || f > PLC_FIFO_TASK_MAX || f >= PLC_FIFO_DISPATCHER)
        {
            std::printf("  FAIL  IEC %d -> FIFO %d out of task range\n", p, f);
            g_failures++;
        }
    }
    CHECK(PLC_FIFO_WATCHDOG > PLC_FIFO_DISPATCHER, "watchdog above dispatcher");
    CHECK(PLC_FIFO_DISPATCHER > PLC_FIFO_TASK_MAX, "dispatcher above every task");

    // A task's Core setting (`TASK t(...) {CORE := n};`, strucpp_task_core()):
    // pinned to that CPU where the machine has it, else Auto (no pinning, the
    // kernel decides), reported as refused. The firmware follows the same rule.
    std::printf("task_policy: task core to CPU affinity\n");
    bool refused = true;
    CHECK(plc_task_affinity_mask(-1, 4, &refused) == 0 && !refused, "Auto -> no pinning");
    CHECK(plc_task_affinity_mask(0, 4, &refused) == 0x1 && !refused, "core 0 -> CPU 0");
    CHECK(plc_task_affinity_mask(3, 4, &refused) == 0x8 && !refused, "core 3 -> CPU 3");
    CHECK(plc_task_affinity_mask(4, 4, &refused) == 0 && refused, "core 4 of 4 CPUs -> Auto, refused");
    CHECK(plc_task_affinity_mask(63, 64, &refused) == (1ULL << 63) && !refused, "core 63 -> CPU 63");
    CHECK(plc_task_affinity_mask(64, 128, &refused) == 0 && refused, "core 64 is past the mask -> Auto");
    CHECK(plc_task_affinity_mask(1, 0, &refused) == 0 && refused, "CPU count unknown -> Auto");
    CHECK(plc_task_affinity_mask(-1, 0, nullptr) == 0, "NULL refused flag accepted");

    if (g_failures == 0)
        std::printf("all cases passed\n");
    return g_failures == 0 ? 0 : 1;
}
