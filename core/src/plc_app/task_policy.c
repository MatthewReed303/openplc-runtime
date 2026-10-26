// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Autonomy®

#include "task_policy.h"

int plc_task_fifo_priority(int iec_priority, bool *clamped)
{
    int p    = iec_priority;
    bool out = false;
    if (p < PLC_IEC_PRIORITY_MIN)
    {
        p   = PLC_IEC_PRIORITY_MIN;
        out = true;
    }
    else if (p > PLC_IEC_PRIORITY_MAX)
    {
        p   = PLC_IEC_PRIORITY_MAX;
        out = true;
    }
    if (clamped)
        *clamped = out;
    return PLC_FIFO_TASK_MAX - p;
}

uint64_t plc_task_affinity_mask(int32_t core, long cpus, bool *refused)
{
    const bool set = core >= 0;
    const bool ok  = set && core < 64 && (long)core < cpus;
    if (refused)
        *refused = set && !ok;
    return ok ? (1ULL << core) : 0;
}

int plc_task_stuck_action(bool past_limit, bool stop_on_stuck)
{
    if (!past_limit)
        return PLC_STUCK_NONE;
    return stop_on_stuck ? PLC_STUCK_STOP : PLC_STUCK_REPORT;
}

int64_t plc_task_stop_deadline(int64_t release, int64_t scan_limit, int64_t stop_at, int64_t keep_running,
                               bool stop_on_stuck)
{
    const int64_t own = release + scan_limit;
    if (stop_on_stuck || stop_at + keep_running <= own)
        return own;
    return stop_at + keep_running;
}

size_t plc_task_stack_bytes(uint32_t requested, long page, bool *refused)
{
    const bool set = requested != 0;
    const bool ok  = set && requested >= PLC_TASK_STACK_MIN && requested <= PLC_TASK_STACK_MAX;
    if (refused)
        *refused = set && !ok;
    if (!ok)
        return 0;
    const size_t p = page > 0 ? (size_t)page : 4096u;
    return (((size_t)requested + p - 1) / p) * p;
}
