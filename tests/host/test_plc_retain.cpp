// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

/*
 * Host test for plc_retain.cpp: the runtime's half of retain, with the .so and
 * the plugin store replaced by stand-ins.
 *
 * WHAT IT PINS
 * ------------
 *   - format 2 (strucpp_retain_*2) is used only when all three exports resolve,
 *     and a partial set falls back to format 1 exactly as before;
 *   - the store is read with a PLC_RETAIN_BLOB_MAX buffer, not this program's
 *     blob size, so an older program's LARGER blob reaches the migration
 *     intact — through a plugin store and through the real file store;
 *   - 65535 is the real ceiling (it was 64 * 1024, which reached the store as a
 *     capacity of 0);
 *   - the one log line a restore produces, for Ok, Migrated and each refusal,
 *     including the two results format 2 added (Migrated 7, BadTrailer 8);
 *   - the cold restart (IEC 61131-3 Figure 9 rule 4): no restore, the store
 *     still told the identity, the initial values written AND flushed at once;
 *   - the status the RETAIN socket command reports.
 *
 * The .so's marshalling is not under test here (STruC++ has its own tests for
 * iec_retain.hpp); its exports are function pointers, so plain functions that
 * record their arguments stand in for them.
 */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

extern "C" {
#include "../drivers/plugin_driver.h"
#include "utils/log.h"
#include "utils/utils.h"
}

#include "debug_write_journal.h"
#include "image_tables.h"
#include "plc_retain.h"
#include "plc_retain_file_store.h"

// ---------------------------------------------------------------------------
// Log capture. Each line is prefixed with its level, because which level a
// line is logged at is part of what is being pinned.
// ---------------------------------------------------------------------------
static std::string g_log;

static void capture(const char *level, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    g_log += level;
    g_log += buf;
    g_log += '\n';
}

extern "C" void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    capture("I ", fmt, ap);
    va_end(ap);
}

extern "C" void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    capture("W ", fmt, ap);
    va_end(ap);
}

extern "C" void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    capture("E ", fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// Runtime pieces plc_retain.cpp calls into.
// ---------------------------------------------------------------------------
plugin_driver_t *plugin_driver = nullptr;

static char g_md5[PLC_RETAIN_PROGRAM_ID_LEN + 1] = "0123456789abcdef0123456789abcdef";
char       *ext_strucpp_program_md5            = g_md5;

extern "C" void image_lock(void) {}
extern "C" void image_unlock(void) {}
extern "C" void debug_write_journal_drain(void) {}

static int g_external_writes = 0;
extern "C" int runtime_external_write(uint8_t, uint16_t, uint8_t op, const uint8_t *, uint16_t)
{
    if (op == DBGW_OP_WRITE) ++g_external_writes;
    return 0;
}

// The .so's retain exports. Only the ones plc_retain.cpp uses need a definition.
size_t   (*ext_strucpp_retain_blob_size)(void)                         = nullptr;
uint32_t (*ext_strucpp_retain_layout_hash)(void)                       = nullptr;
size_t   (*ext_strucpp_retain_pack)(uint8_t *, size_t)                 = nullptr;
uint8_t  (*ext_strucpp_retain_unpack)(const uint8_t *, size_t,
                                     uint8_t (*)(uint8_t, uint16_t, const uint8_t *, uint16_t)) =
    nullptr;
size_t (*ext_strucpp_retain_blob_size2)(void)         = nullptr;
size_t (*ext_strucpp_retain_pack2)(uint8_t *, size_t) = nullptr;
uint8_t (*ext_strucpp_retain_unpack2)(const uint8_t *, size_t,
                                      uint8_t (*)(uint8_t, uint16_t, const uint8_t *, uint16_t),
                                      void *, size_t) = nullptr;

// ---------------------------------------------------------------------------
// The fake .so: a program whose blob is `g_blob_size` bytes of `g_pack_fill`.
// ---------------------------------------------------------------------------
static size_t              g_blob_size = 20;
static uint8_t             g_pack_fill = 0x11;
static plc_retain_report_t g_next_report;
static int                 g_unpack1_calls = 0;
static int                 g_unpack2_calls = 0;
static std::vector<uint8_t> g_unpacked;      // what the last unpack was handed
static size_t              g_report_size = 0; // report_size the runtime passed

static size_t   so_blob_size(void) { return g_blob_size; }
static uint32_t so_layout_hash(void) { return 0x12345678u; }
static size_t   so_pack(uint8_t *out, size_t cap)
{
    if (cap < g_blob_size) return 0;
    memset(out, g_pack_fill, g_blob_size);
    return g_blob_size;
}
static uint8_t so_unpack1(const uint8_t *blob, size_t len,
                          uint8_t (*write)(uint8_t, uint16_t, const uint8_t *, uint16_t))
{
    ++g_unpack1_calls;
    g_unpacked.assign(blob, blob + len);
    if (g_next_report.result == PLC_RETAIN_RESULT_OK && write) write(0, 0, blob, 1);
    return g_next_report.result;
}
static uint8_t so_unpack2(const uint8_t *blob, size_t len,
                          uint8_t (*write)(uint8_t, uint16_t, const uint8_t *, uint16_t),
                          void *report, size_t report_size)
{
    ++g_unpack2_calls;
    g_unpacked.assign(blob, blob + len);
    g_report_size = report_size;
    if ((g_next_report.result == PLC_RETAIN_RESULT_OK ||
         g_next_report.result == PLC_RETAIN_RESULT_MIGRATED) &&
        write)
        write(0, 0, blob, 1);
    if (report) memcpy(report, &g_next_report, report_size < 24 ? report_size : 24);
    return g_next_report.result;
}

static void load_program(bool v1, int v2_count)
{
    ext_strucpp_retain_layout_hash = so_layout_hash;
    ext_strucpp_retain_blob_size   = v1 ? so_blob_size : nullptr;
    ext_strucpp_retain_pack        = v1 ? so_pack : nullptr;
    ext_strucpp_retain_unpack      = v1 ? so_unpack1 : nullptr;
    ext_strucpp_retain_blob_size2  = v2_count >= 1 ? so_blob_size : nullptr;
    ext_strucpp_retain_pack2       = v2_count >= 2 ? so_pack : nullptr;
    ext_strucpp_retain_unpack2     = v2_count >= 3 ? so_unpack2 : nullptr;
}

// ---------------------------------------------------------------------------
// The fake plugin store. plc_retain.cpp reaches a plugin only through the four
// plugin_driver_* calls below, so these ARE the plugin as far as it can tell.
// ---------------------------------------------------------------------------
static plugin_instance_t    g_plugin;
static bool                 g_plugin_present = true;
static std::vector<uint8_t> g_held;           // what the store holds
static int                  g_load_rc        = 0;
static uint16_t             g_load_cap_seen  = 0;
static int                  g_loads = 0, g_saves = 0, g_flushes = 0;
static std::vector<uint8_t> g_last_saved;

extern "C" plugin_instance_t *plugin_driver_find_retain_store(plugin_driver_t *)
{
    return g_plugin_present ? &g_plugin : nullptr;
}

extern "C" int plugin_driver_retain_load(plugin_instance_t *, const char *, uint16_t, uint8_t *out,
                                         uint16_t cap, uint16_t *out_len)
{
    ++g_loads;
    g_load_cap_seen = cap;
    *out_len        = 0;
    if (g_load_rc != 0)
    {
        *out_len = (uint16_t)g_held.size();
        return g_load_rc;
    }
    if (g_held.size() > cap)
    {
        *out_len = (uint16_t)g_held.size();
        return PLC_RETAIN_STORE_TOO_LARGE;
    }
    memcpy(out, g_held.data(), g_held.size());
    *out_len = (uint16_t)g_held.size();
    return 0;
}

extern "C" int plugin_driver_retain_save(plugin_instance_t *, const uint8_t *blob, uint16_t len)
{
    ++g_saves;
    g_last_saved.assign(blob, blob + len);
    return 0;
}

extern "C" int plugin_driver_retain_flush(plugin_instance_t *)
{
    ++g_flushes;
    if (!g_last_saved.empty()) g_held = g_last_saved;
    return 0;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------
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

static bool logged(const std::string &needle) { return g_log.find(needle) != std::string::npos; }

static std::string g_dir;

static void reset(bool plugin = true)
{
    g_log.clear();
    g_plugin_present = plugin;
    g_held.clear();
    g_load_rc       = 0;
    g_load_cap_seen = 0;
    g_loads = g_saves = g_flushes = 0;
    g_last_saved.clear();
    g_unpack1_calls = g_unpack2_calls = 0;
    g_unpacked.clear();
    g_report_size     = 0;
    g_external_writes = 0;
    g_blob_size       = 20;
    g_pack_fill       = 0x11;
    g_next_report     = plc_retain_report_t{};
    memset(&g_plugin, 0, sizeof(g_plugin));
    strncpy(g_plugin.config.name, "fakestore", sizeof(g_plugin.config.name) - 1);
    remove("retain.conf");
    remove("retain.bin");
}

static std::string status()
{
    char buf[1024];
    return plc_retain_status_json(buf, sizeof(buf)) ? std::string(buf) : std::string();
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

static void case_store_is_read_with_the_full_buffer()
{
    g_case = "the store is read with a 65535-byte buffer, not the program's blob size";
    reset();
    load_program(true, 3);
    g_held.assign(20, 0x22);

    plc_retain_init();
    plc_retain_read();

    CHECK(g_load_cap_seen == PLC_RETAIN_BLOB_MAX, "cap must be PLC_RETAIN_BLOB_MAX");
    CHECK(PLC_RETAIN_BLOB_MAX == 65535u, "the ceiling is what a uint16_t length can carry");
}

static void case_larger_old_blob_reaches_the_migration()
{
    g_case = "an older program's larger blob reaches unpack2 intact (plugin store)";
    reset();
    load_program(true, 3);
    g_blob_size = 20;
    g_held.resize(5000);
    for (size_t i = 0; i < g_held.size(); ++i) g_held[i] = (uint8_t)(i * 7);
    g_next_report.result = PLC_RETAIN_RESULT_MIGRATED;

    plc_retain_init();
    plc_retain_read();

    CHECK(g_unpack2_calls == 1, "format 2 must be used when all three exports resolve");
    CHECK(g_unpacked == g_held, "the whole 5000-byte blob must be handed over, unchanged");
    CHECK(g_report_size == sizeof(plc_retain_report_t), "the report buffer is the 24-byte mirror");
}

static void case_larger_old_blob_through_the_file_store()
{
    g_case = "an older program's larger blob reaches unpack2 intact (file store)";
    reset(false);
    load_program(true, 3);
    g_next_report.result = PLC_RETAIN_RESULT_MIGRATED;

    FILE *f = fopen("retain.conf", "w");
    fprintf(f, "enabled=1\npath=%s/retain.bin\nflush_seconds=10\n", g_dir.c_str());
    fclose(f);
    std::vector<uint8_t> old(3000);
    for (size_t i = 0; i < old.size(); ++i) old[i] = (uint8_t)(i * 13 + 1);
    f = fopen("retain.bin", "wb");
    fwrite(g_md5, 1, PLC_RETAIN_PROGRAM_ID_LEN, f);
    fwrite(old.data(), 1, old.size(), f);
    fclose(f);

    plc_retain_init();
    plc_retain_read();
    plc_retain_file_store_stop();

    CHECK(g_unpacked == old, "the file store must hand back all 3000 bytes, not 20");
}

static void case_file_store_answers_too_large()
{
    g_case = "the file store answers TOO_LARGE with the stored length, never truncates";
    reset(false);
    FILE *f = fopen("retain.conf", "w");
    fprintf(f, "enabled=1\npath=%s/retain.bin\nflush_seconds=10\n", g_dir.c_str());
    fclose(f);
    std::vector<uint8_t> big(1000, 0x5A);
    f = fopen("retain.bin", "wb");
    fwrite(g_md5, 1, PLC_RETAIN_PROGRAM_ID_LEN, f);
    fwrite(big.data(), 1, big.size(), f);
    fclose(f);

    plc_retain_file_store_start("./retain.conf");
    uint8_t  out[400];
    uint16_t got = 0;
    const int rc = plc_retain_file_store_load(g_md5, PLC_RETAIN_PROGRAM_ID_LEN, out, sizeof(out),
                                              &got);
    plc_retain_file_store_stop();

    CHECK(rc == PLC_RETAIN_STORE_TOO_LARGE, "a blob larger than the buffer is TOO_LARGE");
    CHECK(got == 1000, "and *out_len carries the stored length so the caller can say so");
}

static void case_65535_is_the_ceiling()
{
    g_case = "a 65535-byte program is accepted and its store sees cap 65535";
    reset();
    load_program(true, 3);
    g_blob_size = 65535;

    plc_retain_init();
    plc_retain_read();
    plc_retain_save();

    CHECK(logged("Retain: 65535 bytes across"), "65535 must be accepted");
    CHECK(g_load_cap_seen == 65535, "the store must not see a capacity of 0");
    CHECK(g_last_saved.size() == 65535, "the whole blob must be handed to the store");

    g_case = "a 65536-byte program is refused, naming both numbers";
    reset();
    load_program(true, 3);
    g_blob_size = 65536;
    plc_retain_init();
    plc_retain_read();
    CHECK(logged("E Retain: program needs 65536 bytes, this runtime handles at most 65535"),
          "65536 must be refused at init");
    CHECK(g_loads == 0, "and no store may be consulted");
}

static void case_partial_v2_falls_back_to_format_1()
{
    for (int n = 0; n < 3; ++n)
    {
        g_case = "fewer than three v2 exports means format 1, exactly as before";
        reset();
        load_program(true, n);
        g_held.assign(20, 0x33);
        g_next_report.result = PLC_RETAIN_RESULT_OK;

        plc_retain_init();
        plc_retain_read();
        plc_retain_save();

        CHECK(g_unpack1_calls == 1 && g_unpack2_calls == 0, "format-1 unpack must be used");
        CHECK(logged("format 1"), "the init line must name the format");
        CHECK(logged("I Retain: restored 20 bytes (layout 12345678)"), "Ok is logged as before");
    }

    g_case = "v2 alone (no v1 exports) still runs retain";
    reset();
    load_program(false, 3);
    g_held.assign(20, 0x33);
    plc_retain_init();
    plc_retain_read();
    CHECK(g_unpack2_calls == 1, "v2 must not depend on the v1 exports");

    g_case = "neither set: retain stays off";
    reset();
    load_program(false, 2);
    plc_retain_init();
    plc_retain_read();
    CHECK(g_loads == 0, "no store may be consulted");
    CHECK(status().find("\"start\":\"none\"") != std::string::npos, "status says none");
}

static void case_report_logging()
{
    g_case = "Ok logs the bytes restored and the layout";
    reset();
    load_program(true, 3);
    g_held.assign(20, 0x44);
    g_next_report.result         = PLC_RETAIN_RESULT_OK;
    g_next_report.format         = 2;
    g_next_report.program_layout = 0x12345678u;
    plc_retain_init();
    plc_retain_read();
    CHECK(logged("I Retain: restored 20 bytes (layout 12345678)"), "Ok line");
    CHECK(g_external_writes == 1, "Ok writes through the runtime's callback");

    g_case = "Migrated logs every count, at info when nothing was refused";
    reset();
    load_program(true, 3);
    g_held.assign(30, 0x44);
    g_next_report                = plc_retain_report_t{};
    g_next_report.result         = PLC_RETAIN_RESULT_MIGRATED;
    g_next_report.format         = 2;
    g_next_report.kept           = 3;
    g_next_report.truncated      = 1;
    g_next_report.converted      = 2;
    g_next_report.added          = 5;
    g_next_report.dropped        = 4;
    g_next_report.stored_layout  = 0x0000abcdu;
    g_next_report.program_layout = 0x12345678u;
    plc_retain_init();
    plc_retain_read();
    CHECK(logged("I Retain: layout changed 0000abcd -> 12345678: kept 3 (1 shortened), "
                 "converted 2, new 5, dropped 4, refused 0"),
          "Migrated line");
    const std::string st = status();
    CHECK(st.find("\"start\":\"warm\"") != std::string::npos, "status: warm");
    CHECK(st.find("\"result\":7") != std::string::npos, "status: result 7");
    CHECK(st.find("\"result_name\":\"migrated\"") != std::string::npos, "status: name");
    CHECK(st.find("\"restored\":true") != std::string::npos, "status: restored");
    CHECK(st.find("\"kept\":3,\"converted\":2,\"truncated\":1,\"added\":5,\"dropped\":4,"
                  "\"refused\":0") != std::string::npos,
          "status: counts");
    CHECK(st.find("\"stored_layout\":\"0000abcd\",\"program_layout\":\"12345678\"") !=
              std::string::npos,
          "status: layouts");
    CHECK(st.find("\"stored_bytes\":30") != std::string::npos, "status: stored bytes");
    CHECK(st.find("\"store\":\"fakestore\"") != std::string::npos, "status: store name");

    g_case = "Migrated with a refusal is a warning";
    const plc_retain_report_t migrated = g_next_report;
    reset();
    load_program(true, 3);
    g_held.assign(30, 0x44);
    g_next_report         = migrated;
    g_next_report.refused = 2;
    plc_retain_init();
    plc_retain_read();
    CHECK(logged("W Retain: layout changed 0000abcd -> 12345678: kept 3 (1 shortened), "
                 "converted 2, new 5, dropped 4, refused 2"),
          "a refusal loses a value the operator may have set, so it warns");

    struct
    {
        uint8_t     result;
        const char *why;
        const char *name;
    } refusals[] = {
        {PLC_RETAIN_RESULT_EMPTY, "(no data)", "empty"},
        {PLC_RETAIN_RESULT_BAD_MAGIC, "(bad magic)", "bad_magic"},
        {PLC_RETAIN_RESULT_BAD_FORMAT, "(bad format)", "bad_format"},
        {PLC_RETAIN_RESULT_BAD_CRC, "(crc mismatch)", "bad_crc"},
        {PLC_RETAIN_RESULT_STALE_LAYOUT, "(layout is from a different program", "stale_layout"},
        {PLC_RETAIN_RESULT_TRUNCATED, "(truncated)", "truncated"},
        {PLC_RETAIN_RESULT_BAD_TRAILER, "(the stored table of variables is damaged)",
         "bad_trailer"},
        {9, "(unknown)", "unknown"},
    };
    for (const auto &r : refusals)
    {
        g_case = "each refusal names its reason";
        reset();
        load_program(true, 3);
        g_held.assign(20, 0x44);
        g_next_report.result = r.result;
        plc_retain_init();
        plc_retain_read();
        CHECK(logged(std::string("W Retain: stored values refused ") + r.why), r.why);
        CHECK(logged("start at their initial values"), "and says what that means");
        CHECK(g_external_writes == 0, "a refusal writes nothing");
        CHECK(status().find(std::string("\"result_name\":\"") + r.name + "\"") !=
                  std::string::npos,
              r.name);
        CHECK(status().find("\"restored\":false") != std::string::npos, "not restored");
    }
}

static void case_store_answers()
{
    g_case = "an empty store is not a refusal";
    reset();
    load_program(true, 3);
    plc_retain_init();
    plc_retain_read();
    CHECK(logged("I Retain: nothing stored yet"), "empty is info");
    CHECK(g_unpack2_calls == 0, "nothing to unpack");
    CHECK(status().find("\"result\":null") != std::string::npos, "no result to report");

    g_case = "a store error is said as an error, not as 'nothing stored'";
    reset();
    load_program(true, 3);
    g_held.assign(20, 1);
    g_load_rc = -1;
    plc_retain_init();
    plc_retain_read();
    CHECK(logged("W Retain: fakestore could not read its stored values (error -1)"), "error line");
    CHECK(!logged("nothing stored yet"), "not the empty line");
    CHECK(g_unpack2_calls == 0, "nothing to unpack");

    g_case = "TOO_LARGE from a store is reported with the size";
    reset();
    load_program(true, 3);
    g_held.assign(20, 1);
    g_load_rc = PLC_RETAIN_STORE_TOO_LARGE;
    plc_retain_init();
    plc_retain_read();
    CHECK(logged("W Retain: fakestore holds 20 bytes, more than the 65535 this runtime reads"),
          "too-large line");
    CHECK(g_unpack2_calls == 0, "nothing to unpack");
}

static void case_cold_start()
{
    g_case = "cold restart: no restore, identity handed over, initial values written and flushed";
    reset();
    load_program(true, 3);
    g_held.assign(20, 0x77); // the previous run's values
    g_pack_fill = 0x01;      // the program's initial values

    plc_retain_init();
    plc_retain_cold_start();

    CHECK(g_loads == 1, "the store is still read once, for the identity it labels commits with");
    CHECK(g_unpack1_calls == 0 && g_unpack2_calls == 0, "nothing may be restored");
    CHECK(g_external_writes == 0, "no variable may be written");
    CHECK(g_saves == 1 && g_flushes == 1, "the initial values are written and flushed at once");
    CHECK(g_held == std::vector<uint8_t>(20, 0x01), "the store now holds the initial values");
    CHECK(logged("I Retain: cold restart — stored values discarded"), "said once");
    const std::string st = status();
    CHECK(st.find("\"start\":\"cold\"") != std::string::npos, "status: cold");
    CHECK(st.find("\"result\":null") != std::string::npos, "status: no restore result");

    g_case = "a warm start after the cold restart restores the initial values";
    plc_retain_init();
    g_next_report.result = PLC_RETAIN_RESULT_OK;
    plc_retain_read();
    CHECK(g_unpacked == std::vector<uint8_t>(20, 0x01), "the old values must not come back");

    g_case = "cold restart through the file store is on disk before it returns";
    reset(false);
    load_program(true, 3);
    FILE *f = fopen("retain.conf", "w");
    fprintf(f, "enabled=1\npath=%s/retain.bin\nflush_seconds=60\n", g_dir.c_str());
    fclose(f);
    f = fopen("retain.bin", "wb");
    fwrite(g_md5, 1, PLC_RETAIN_PROGRAM_ID_LEN, f);
    const std::vector<uint8_t> old(20, 0x77);
    fwrite(old.data(), 1, old.size(), f);
    fclose(f);
    g_pack_fill = 0x02;

    plc_retain_init();
    plc_retain_cold_start();

    /* Read the file while the store is still running: a power cut now must
     * find the initial values, so they cannot be waiting on the flush timer. */
    std::vector<uint8_t> disk(PLC_RETAIN_PROGRAM_ID_LEN + 64);
    f = fopen("retain.bin", "rb");
    const size_t n = f ? fread(disk.data(), 1, disk.size(), f) : 0;
    if (f) fclose(f);
    plc_retain_file_store_stop();
    CHECK(n == PLC_RETAIN_PROGRAM_ID_LEN + 20, "header plus this program's blob");
    CHECK(n >= PLC_RETAIN_PROGRAM_ID_LEN + 20 &&
              memcmp(disk.data(), g_md5, PLC_RETAIN_PROGRAM_ID_LEN) == 0 &&
              std::vector<uint8_t>(disk.begin() + PLC_RETAIN_PROGRAM_ID_LEN,
                                   disk.begin() + PLC_RETAIN_PROGRAM_ID_LEN + 20) ==
                  std::vector<uint8_t>(20, 0x02),
          "the file holds the initial values, labelled with this program");

    g_case = "cold restart with no store says so and touches nothing";
    reset(false);
    load_program(true, 3);
    plc_retain_init();
    plc_retain_cold_start();
    CHECK(logged("I Retain: cold restart — every variable starts at its initial value"), "line");
    CHECK(g_saves == 0 && g_loads == 0, "no store to touch");
}

int main()
{
    char tmpl[] = "/tmp/retain-test-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir || chdir(dir) != 0) {
        fprintf(stderr, "could not create a temp directory\n");
        return 2;
    }
    g_dir = dir;

    printf("plc_retain — format 2, read buffer, report, cold restart\n");
    case_store_is_read_with_the_full_buffer();
    case_larger_old_blob_reaches_the_migration();
    case_larger_old_blob_through_the_file_store();
    case_file_store_answers_too_large();
    case_65535_is_the_ceiling();
    case_partial_v2_falls_back_to_format_1();
    case_report_logging();
    case_store_answers();
    case_cold_start();

    reset();
    if (chdir("/") == 0) rmdir(g_dir.c_str());

    if (g_failures == 0) {
        printf("all cases passed\n");
        return 0;
    }
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}
