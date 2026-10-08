// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

/*
 * Host test for the command socket's COLD_START and RETAIN commands.
 *
 * The real unix_socket.c is linked; the state manager, the mode switch and
 * retain are stand-ins that record what was asked of them. What is pinned:
 *
 *   - COLD_START has START's preconditions and refusals, in COLD_START:*
 *     replies, and is refused (not turned into stop-then-start) while RUNNING;
 *   - the cold mark is armed only AFTER the start is claimed, and before the
 *     transition body runs — so a refused claim can never leave a later,
 *     ordinary start armed, and the start that was claimed is the cold one;
 *   - a plain START never arms it;
 *   - RETAIN answers RETAIN:<json>, also while a transition is in flight.
 *
 * C, not C++: unix_socket.c is C (its malloc result is not cast), so run.sh
 * builds this file and its sources with $CC.
 */

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../drivers/plugin_driver.h"
#include "plc_state_manager.h"
#include "plc_switch.h"
#include "unix_socket.h"

/* Defined in unix_socket.c and called only from its own socket thread, so no
 * header declares it. */
void handle_unix_socket_commands(const char *command, char *response, size_t response_size);

/* ---- stand-ins --------------------------------------------------------- */

volatile sig_atomic_t keep_running = 1;

void log_info(const char *fmt, ...) { (void)fmt; }
void log_warn(const char *fmt, ...) { (void)fmt; }
void log_error(const char *fmt, ...) { (void)fmt; }

size_t parse_hex_string(const char *hex_string, uint8_t *data) { return 0; }
void   bytes_to_hex_string(const uint8_t *bytes, size_t len, char *out_str, size_t out_size,
                           const char *prepend)
{
}
size_t process_debug_data(uint8_t *data, size_t length) { return 0; }
int    format_timing_stats_response(char *buffer, size_t buffer_size) { return 0; }
size_t plugin_driver_append_stats_json(plugin_driver_t *driver, char *buffer, size_t buffer_size)
{
    return 0;
}
int plugin_driver_execute_command(plugin_driver_t *driver, const char *plugin_name,
                                  const char *command_json, char *response, size_t response_size)
{
    return -1;
}

static PLCState g_state         = PLC_STATE_STOPPED;
static bool     g_transitioning = false;
static bool     g_claim_ok      = true;
static bool     g_switch_run    = true;

static volatile int g_claims        = 0;
static volatile int g_arms          = 0;
static volatile int g_arms_at_claim = -1; /* g_arms when the claim was made */
static volatile int g_set_running   = 0;
static volatile int g_arms_at_set   = -1; /* g_arms when the body ran */

PLCState plc_get_state(void) { return g_state; }
bool     plc_state_is_transitioning(void) { return g_transitioning; }

bool plc_claim_transition(PLCState target)
{
    g_claims++;
    g_arms_at_claim = g_arms;
    return g_claim_ok;
}

void plc_arm_cold_start(void) { g_arms++; }

bool plc_set_state(PLCState new_state)
{
    if (new_state == PLC_STATE_RUNNING)
    {
        g_arms_at_set = g_arms;
        g_set_running++;
    }
    return true;
}

plc_switch_t plc_get_switch_position(void) { return g_switch_run ? PLC_SWITCH_RUN : PLC_SWITCH_STOP; }
bool         plc_switch_allows_run(void) { return g_switch_run; }
bool         plc_switch_take_movement(void) { return false; }
void         plc_switch_note_movement(void) {}

size_t plc_retain_status_json(char *out, size_t cap)
{
    const char *json = "{\"start\":\"cold\",\"result\":null}";
    if (strlen(json) + 1 > cap) return 0;
    strcpy(out, json);
    return strlen(json);
}

/* ---- harness ------------------------------------------------------------ */

static int         g_failures = 0;
static const char *g_case     = "";

#define CHECK(cond, what)                                                                      \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            fprintf(stderr, "  FAIL  %s: %s\n         (%s:%d)\n", g_case, (what), __FILE__,     \
                    __LINE__);                                                                 \
            g_failures++;                                                                       \
        }                                                                                      \
    } while (0)

static char g_resp[512];

static const char *send_cmd(const char *cmd)
{
    memset(g_resp, 0, sizeof(g_resp));
    handle_unix_socket_commands(cmd, g_resp, sizeof(g_resp));
    return g_resp;
}

static void reset(void)
{
    g_state         = PLC_STATE_STOPPED;
    g_transitioning = false;
    g_claim_ok      = true;
    g_switch_run    = true;
    g_claims = g_arms = g_set_running = 0;
    g_arms_at_claim = g_arms_at_set = -1;
}

/* The transition body runs on a detached worker; give it a moment. */
static void wait_for_body(int want)
{
    for (int i = 0; i < 200 && g_set_running < want; ++i)
    {
        struct timespec t = {0, 5 * 1000000L};
        nanosleep(&t, NULL);
    }
}

int main(void)
{
    printf("unix_socket — COLD_START and RETAIN\n");

    g_case = "COLD_START from STOPPED claims, arms after the claim, then runs the start";
    reset();
    CHECK(strcmp(send_cmd("COLD_START"), "COLD_START:OK\n") == 0, g_resp);
    wait_for_body(1);
    CHECK(g_claims == 1, "one claim");
    CHECK(g_arms_at_claim == 0, "not armed before the claim");
    CHECK(g_arms == 1, "armed once");
    CHECK(g_set_running == 1, "the start body ran");
    CHECK(g_arms_at_set == 1, "and it ran armed");

    g_case = "COLD_START whose claim is refused never arms";
    reset();
    g_claim_ok = false;
    CHECK(strcmp(send_cmd("COLD_START"), "COLD_START:ERROR\n") == 0, g_resp);
    CHECK(g_arms == 0, "a refused claim must not leave a later start armed");
    CHECK(g_set_running == 0, "nothing ran");

    g_case = "COLD_START while RUNNING is refused, not turned into stop-then-start";
    reset();
    g_state = PLC_STATE_RUNNING;
    CHECK(strcmp(send_cmd("COLD_START"), "COLD_START:ERROR_ALREADY_RUNNING\n") == 0, g_resp);
    CHECK(g_claims == 0 && g_arms == 0, "nothing claimed or armed");

    g_case = "COLD_START with the mode switch in STOP is refused like START";
    reset();
    g_switch_run = false;
    CHECK(strcmp(send_cmd("COLD_START"), "COLD_START:ERROR_SWITCH_STOP\n") == 0, g_resp);
    CHECK(g_claims == 0 && g_arms == 0, "nothing claimed or armed");

    g_case = "COLD_START during a transition is BUSY";
    reset();
    g_transitioning = true;
    CHECK(strcmp(send_cmd("COLD_START"), "COMMAND:BUSY\n") == 0, g_resp);
    CHECK(g_claims == 0 && g_arms == 0, "nothing claimed or armed");

    g_case = "a plain START never arms the cold mark";
    reset();
    CHECK(strcmp(send_cmd("START"), "START:OK\n") == 0, g_resp);
    wait_for_body(1);
    CHECK(g_set_running == 1, "the start body ran");
    CHECK(g_arms == 0 && g_arms_at_set == 0, "warm: never armed");

    g_case = "RETAIN answers RETAIN:<json>";
    reset();
    CHECK(strcmp(send_cmd("RETAIN"), "RETAIN:{\"start\":\"cold\",\"result\":null}\n") == 0, g_resp);

    g_case = "RETAIN is answered mid-transition, like STATUS";
    reset();
    g_transitioning = true;
    CHECK(strncmp(send_cmd("RETAIN"), "RETAIN:{", 8) == 0, g_resp);

    if (g_failures == 0)
    {
        printf("all cases passed\n");
        return 0;
    }
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}
