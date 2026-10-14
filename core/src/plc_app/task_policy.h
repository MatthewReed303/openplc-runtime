// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

#ifndef TASK_POLICY_H
#define TASK_POLICY_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* SCHED_FIFO priority of the main watchdog thread. Above everything else. */
#define PLC_FIFO_WATCHDOG 99

/* SCHED_FIFO priority of the GCD master-tick dispatcher. */
#define PLC_FIFO_DISPATCHER 98

/* Highest SCHED_FIFO priority an IEC task may get. Kept below the PREEMPT_RT
 * default of 50 for threaded IRQ handlers. */
#define PLC_FIFO_TASK_MAX 49

/* A task still in one scan after this many of its own periods is stuck. Also
 * the grace each task gets to finish its scan when the PLC stops. */
#define PLC_TASK_STUCK_PERIODS 10

/* A task's first scan is exempt from the stuck count; the main watchdog trips it only
 * past this bound, so a hung initialisation is not permanent. */
#define PLC_FIRST_SCAN_TIMEOUT_MS 10000

/* How long zeroed outputs are held on stop before plugins are stopped, so
 * plugins polling the image from their own threads can send them. */
#define PLC_OUTPUTS_OFF_SETTLE_MS 500

/* How long an aborted task, or an idle one that was woken, may take to exit
 * before the runtime gives up and exits with PLC_EXIT_WATCHDOG_FAULT. */
#define PLC_TASK_ABORT_TIMEOUT_MS 2000

/* Fixed allowance for the stop teardown (plugin stop, unload) on top of the
 * task grace. Past the whole budget the watchdog exits the process. */
#define PLC_STOP_TEARDOWN_ALLOWANCE_MS 30000

/* A dispatcher with no tick for max(10 base ticks, this) is stalled. */
#define PLC_DISPATCHER_STALL_MIN_MS 1000

/* Process exit code for an unrecoverable watchdog fault. The webserver
 * (webserver/runtimemanager.py) restarts the runtime in safe mode on it. */
#define PLC_EXIT_WATCHDOG_FAULT 42

/* Written before the watchdog exit and consumed at the next boot, which then
 * starts in safe mode reporting ERROR even when the exit code was not seen. */
#define PLC_WATCHDOG_FAULT_MARKER "/run/runtime/watchdog_fault"

/* Fault context written into the marker when the safe-mode boot's outputs-off
 * itself hangs; the next boot then skips it instead of looping. */
#define PLC_FAULT_CONTEXT_BOOT_OUTPUTS_OFF "safe-mode boot outputs-off"

/* Signal the teardown sends to a task still in its scan after its grace. Runtime
 * code reached from IEC bodies blocks it around fork/allocation-heavy sections. */
#define PLC_TASK_ABORT_SIGNAL SIGUSR2

/* IEC TASK priority range accepted by the runtime. 0 is the highest. */
#define PLC_IEC_PRIORITY_MIN 0
#define PLC_IEC_PRIORITY_MAX 48

    /**
     * @brief Map an IEC 61131-3 TASK priority to a SCHED_FIFO priority.
     *
     * IEC priority 0 is the highest and maps to PLC_FIFO_TASK_MAX (49); IEC
     * priority 48 maps to 1. Values outside 0..48 are clamped to the nearest end.
     *
     * @param iec_priority priority declared on the IEC TASK
     * @param clamped      set to true when iec_priority was out of range; may be NULL
     * @return SCHED_FIFO priority in 1..PLC_FIFO_TASK_MAX
     */
    int plc_task_fifo_priority(int iec_priority, bool *clamped);

    /**
     * @brief The CPU affinity of a task from its Core setting.
     *
     * The project's `TASK t(...) {CORE := n};` (an IEC 61131-3 §6.2 pragma; the
     * program exports it as strucpp_task_core()) pins the task's thread to CPU
     * n. Auto (-1), a CPU the machine lacks, or one past the 64-bit mask is no
     * pinning (the kernel decides), as before the setting existed; the
     * bare-metal firmware follows the same rule (plc_core_place.h).
     *
     * @param core    the task's core, -1 for Auto
     * @param cpus    CPUs online (sysconf(_SC_NPROCESSORS_ONLN)); <= 0 unknown
     * @param refused set to true when a core was set but cannot be used; may be NULL
     * @return the affinity mask, 0 = no pinning
     */
    uint64_t plc_task_affinity_mask(int32_t core, long cpus, bool *refused);

/* What the dispatcher does with a task it finds still in one scan. */
#define PLC_STUCK_NONE   0 /* not past the limit */
#define PLC_STUCK_STOP   1 /* stop the PLC (ERROR), as always */
#define PLC_STUCK_REPORT 2 /* report it; the PLC and the other tasks keep running */

    /**
     * @brief What a task past the stuck limit does, from its "Stop PLC if stuck".
     *
     * The project's `TASK t(...) {STOP_ON_STUCK := FALSE};` (an IEC 61131-3 §6.2
     * pragma; the program exports it as strucpp_task_stop_on_stuck(), default
     * TRUE) keeps the PLC running when that task is stuck (PLC_TASK_STUCK_PERIODS
     * of its periods in one scan, or a first scan past PLC_FIRST_SCAN_TIMEOUT_MS):
     * it is reported instead. The bare-metal firmware follows the same rule
     * (plc_core_place.h).
     *
     * @param past_limit    the task is past the stuck limit
     * @param stop_on_stuck the task's setting (true = stop, the default)
     * @return PLC_STUCK_NONE, PLC_STUCK_STOP or PLC_STUCK_REPORT
     */
    int plc_task_stuck_action(bool past_limit, bool stop_on_stuck);

/* On a stop, a task set not to stop the PLC when stuck (a communications task in a
 * Wi-Fi / TCP connect) may finish its scan until this long after the stop, when
 * that is later than its own stuck limit. Never above PLC_FIRST_SCAN_TIMEOUT_MS: the
 * stop budget (plc_stop_budget_ms) has that as its floor. */
#define PLC_STOP_WAIT_KEEP_RUNNING_MS 10000

    /**
     * @brief Until when a stop lets a task's in-flight scan run before aborting it.
     *
     * Every task: its release + its scan limit (PLC_TASK_STUCK_PERIODS of its
     * periods, or the first-scan timeout), as always. A task whose "Stop PLC if
     * stuck" is No also gets at least PLC_STOP_WAIT_KEEP_RUNNING_MS from the stop,
     * so a stop during a long connect ends as a normal stop. The bare-metal
     * firmware follows the same rule (plc_core_place.h).
     *
     * @param release       the scan's release time (any unit)
     * @param scan_limit    the task's scan limit (same unit)
     * @param stop_at       when the stop was asked (same unit)
     * @param keep_running  PLC_STOP_WAIT_KEEP_RUNNING_MS in that unit
     * @param stop_on_stuck the task's setting (true = stop, the default)
     * @return the deadline (same unit)
     */
    int64_t plc_task_stop_deadline(int64_t release, int64_t scan_limit, int64_t stop_at, int64_t keep_running,
                                   bool stop_on_stuck);

/* The stack a task's Stack setting may give its thread: below or above these the
 * thread keeps the default stack (logged). Logging and signal handling in a task
 * thread need tens of KB, so the floor is well above PTHREAD_STACK_MIN. */
#define PLC_TASK_STACK_MIN (64u * 1024u)
#define PLC_TASK_STACK_MAX (64u * 1024u * 1024u)

    /**
     * @brief The thread stack of a task from its Stack setting.
     *
     * The project's `TASK t(...) {STACK := n};` (exported as strucpp_task_stack(),
     * 0 = Default) sizes the task's thread: n rounded up to a whole page where it is
     * within PLC_TASK_STACK_MIN..PLC_TASK_STACK_MAX, else the default stack,
     * reported as refused. The bare-metal firmware does the same with its bounds.
     *
     * @param requested the task's stack in bytes, 0 for Default
     * @param page      the page size (sysconf(_SC_PAGESIZE)); <= 0 is taken as 4096
     * @param refused   set to true when a size was set but cannot be used; may be NULL
     * @return the stack size in bytes, 0 = the default
     */
    size_t plc_task_stack_bytes(uint32_t requested, long page, bool *refused);

#ifdef __cplusplus
}
#endif

#endif // TASK_POLICY_H
