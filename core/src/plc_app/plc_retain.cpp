// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

/**
 * @file plc_retain.cpp
 * @brief Retain-variable persistence — the runtime's half (NODE-94).
 *
 * See plc_retain.h for the split: the .so marshals, a plugin stores, and this
 * file owns the buffer and the call sites.
 */

#include "plc_retain.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include "../drivers/plugin_driver.h"
#include "utils/log.h"
#include "utils/utils.h"  // ext_strucpp_program_md5 — the program's identity
}

#include "debug_write_journal.h"
#include "image_tables.h"
#include "plc_retain_file_store.h"

extern plugin_driver_t *plugin_driver;

namespace {

/**
 * Cap on the blob this runtime will handle.
 *
 * Generous compared with baremetal's 512 bytes — there is no SRAM pressure
 * here — but bounded on purpose: the buffer is read from the scan path, and an
 * unbounded allocation driven by a program's declaration count is not
 * something to discover on a running machine. A program needing more is
 * refused at init with a message naming both numbers.
 *
 * PLC_RETAIN_BLOB_MAX (65535), not 64 * 1024: every length on the store
 * interface is a uint16_t, and 65536 passed the check above only to reach the
 * store as a capacity of 0.
 */
constexpr size_t RETAIN_BUFFER_MAX = PLC_RETAIN_BLOB_MAX;

std::vector<uint8_t> g_buffer;
std::atomic<bool>    g_active{false};

/* Whether this program offers the format-2 API (strucpp_retain_*2). Written
 * only by plc_retain_init() before `g_active` publishes, like `g_driver`. */
bool g_v2 = false;

size_t retain_blob_size()
{
    return g_v2 ? ext_strucpp_retain_blob_size2() : ext_strucpp_retain_blob_size();
}

size_t retain_pack(uint8_t *out, size_t cap)
{
    return g_v2 ? ext_strucpp_retain_pack2(out, cap) : ext_strucpp_retain_pack(out, cap);
}

uint32_t program_layout_hash()
{
    return ext_strucpp_retain_layout_hash ? ext_strucpp_retain_layout_hash() : 0u;
}

/**
 * The last start's outcome, kept for plc_retain_status_json().
 *
 * Written by the PLC thread while a start lands and read by the command-socket
 * thread whenever someone asks, so it has its own lock. Neither side holds it
 * for longer than a copy.
 */
struct RetainStatus
{
    plc_retain_start_t  start        = PLC_RETAIN_START_NONE;
    uint8_t             api          = 0; /* 0 none, 1 format 1, 2 format 2 */
    size_t              blob_bytes   = 0; /* this program's blob */
    uint16_t            stored_bytes = 0; /* what the store handed back */
    bool                have_report  = false;
    plc_retain_report_t report       = {};
    std::string         store;
};

std::mutex   g_status_lock;
RetainStatus g_status;

/**
 * Why a restore wrote nothing, indexed by `strucpp::retain::LoadResult`.
 *
 * Deliberately explicit about WHICH check failed. "Retain refused" sends
 * someone hunting; "the stored layout is from a different program" tells them
 * it was the upload, and a crc failure tells them it was the store. MIGRATED
 * (7) is never a refusal and is here only so every later index stays equal to
 * its LoadResult number.
 */
const char *const k_why[] = {
    "ok",
    "no data",
    "bad magic",
    "bad format",
    "crc mismatch",
    "layout is from a different program, and the stored values carry no names to match by",
    "truncated",
    "migrated",
    "the stored table of variables is damaged",
};

/* The same results as stable identifiers, for the JSON status. */
const char *const k_result_name[] = {
    "ok",          "empty",     "bad_magic", "bad_format",  "bad_crc",
    "stale_layout", "truncated", "migrated",  "bad_trailer",
};

constexpr size_t k_result_count = sizeof(k_why) / sizeof(k_why[0]);
static_assert(sizeof(k_result_name) / sizeof(k_result_name[0]) == k_result_count,
              "every LoadResult needs both a reason and a name");

const char *why_text(uint8_t res)
{
    return res < k_result_count ? k_why[res] : "unknown";
}

/** Say what a restore did, in one line. */
void log_report(const plc_retain_report_t *r, uint16_t stored_bytes)
{
    if (r->result == PLC_RETAIN_RESULT_OK)
    {
        log_info("Retain: restored %u bytes (layout %08" PRIx32 ")", (unsigned)stored_bytes,
                 r->program_layout);
        return;
    }
    if (r->result == PLC_RETAIN_RESULT_MIGRATED)
    {
        /* A changed layout migrated by name. IEC 61131-3 6.5.6.1 rule 1 gives
         * every variable that still exists its stored value; 6.5.6.2 gives a new
         * one, and one whose stored value could not be converted, its declared
         * initial value. A refusal is the only part that loses something the
         * operator may have set, so it is the part that raises the level. */
        char line[192];
        snprintf(line, sizeof(line),
                 "Retain: layout changed %08" PRIx32 " -> %08" PRIx32
                 ": kept %u (%u shortened), converted %u, new %u, dropped %u, refused %u",
                 r->stored_layout, r->program_layout, (unsigned)r->kept, (unsigned)r->truncated,
                 (unsigned)r->converted, (unsigned)r->added, (unsigned)r->dropped,
                 (unsigned)r->refused);
        if (r->refused != 0)
            log_warn("%s", line);
        else
            log_info("%s", line);
        return;
    }
    log_warn("Retain: stored values refused (%s) — retained variables start at their initial values",
             why_text(r->result));
}

/**
 * Restore writes go through the runtime's external-write path, NOT straight to
 * the IECVar.
 *
 * A retained variable may also be located (`VAR RETAIN x AT %MW10`). Poking
 * such a leaf's storage directly is undone by the next copy-in from the process
 * image, so the value would appear to restore and then silently revert on the
 * first scan. `runtime_external_write` classifies the leaf and routes a located
 * one through the image journal — the same path OPC-UA writes take.
 *
 * DBGW_OP_WRITE, never a force: restoring a retained value must not pin it. The
 * program has to be able to move it on the very next scan, and an operator's
 * force has to stay authoritative over whatever was stored.
 */
uint8_t retain_write_leaf(uint8_t arr, uint16_t elem, const uint8_t *bytes, uint16_t len)
{
    const int rc = runtime_external_write(arr, elem, (uint8_t)DBGW_OP_WRITE, bytes, len);

    /* Restoring happens once, at program load, before any task is released —
     * so apply the write now instead of leaving it for the dispatcher's
     * cycle-end drain. Left queued, scan 1 would run on the initial values and
     * the restore would then overwrite what scan 1 wrote; and a program with
     * more retained leaves than the queue holds would lose the rest. */
    image_lock();
    debug_write_journal_drain();
    image_unlock();

    return rc == 0 ? 0x7E : 0x82;
}

/**
 * A store, whatever kind it is.
 *
 * Three function pointers and a name. Everything past init() calls through this
 * record, so there is exactly one path to storage and no branch anywhere that
 * asks whether the bytes are going to a plugin or to a file. Adding a third
 * kind of store means filling this in from somewhere new and changing nothing
 * else.
 */
struct RetainDriver
{
    const char *name;
    int (*read)(const char *program_md5, uint16_t md5_len, uint8_t *out, uint16_t cap,
                uint16_t *out_len);
    int (*write)(const uint8_t *blob, uint16_t len);
    int (*flush)(void);
};

/* Written only by plc_retain_init(), read from the scan thread.
 *
 * `g_active` IS THE PUBLICATION BARRIER for this record. init() stores false
 * before mutating it and true after, both seq_cst, and every reader checks
 * g_active before touching g_driver — so a reader that sees active==true is
 * guaranteed to see the completed record. Nothing else orders these writes, so
 * an early return that skips the `store(true)`, or a relaxed memory order on
 * either store, would break it silently. */
RetainDriver g_driver = {nullptr, nullptr, nullptr, nullptr};

/* The plugin acting as the store, when a plugin claimed it. Held only so the
 * thunks below have something to forward to — the plugin hooks are instance
 * methods and the driver record is plain function pointers. */
plugin_instance_t *g_plugin_store = nullptr;

int plugin_read_thunk(const char *program_md5, uint16_t md5_len, uint8_t *out, uint16_t cap,
                      uint16_t *out_len)
{
    return plugin_driver_retain_load(g_plugin_store, program_md5, md5_len, out, cap, out_len);
}

int plugin_write_thunk(const uint8_t *blob, uint16_t len)
{
    return plugin_driver_retain_save(g_plugin_store, blob, len);
}

int plugin_flush_thunk(void)
{
    return plugin_driver_retain_flush(g_plugin_store);
}

/** Whether a store is bound. Cheap enough to ask on the scan path. */
bool driver_bound()
{
    return g_driver.read != nullptr && g_driver.write != nullptr;
}

/** Record how this start treated retained values, for plc_retain_status_json(). */
void record_start(plc_retain_start_t start, uint16_t stored_bytes,
                  const plc_retain_report_t *report)
{
    std::lock_guard<std::mutex> guard(g_status_lock);
    g_status.start        = start;
    g_status.stored_bytes = stored_bytes;
    g_status.have_report  = report != nullptr;
    g_status.report       = report ? *report : plc_retain_report_t{};
}

/**
 * The program's identity, or NULL after standing retain down.
 *
 * No identity to compare against means a driver cannot tell a new program from
 * the old one, and restoring on that basis is how one program inherits
 * another's state. Refuse — and stand the store DOWN rather than leave saves
 * running.
 *
 * Returning while `g_active` stayed true left retain half-on: the per-scan
 * save kept packing and handing over bytes that no driver could ever commit,
 * because the identity a commit needs is only ever set by the read this branch
 * skipped. The file store then refused every write and logged "short write"
 * once per flush interval, forever, naming a cause that was not the real one.
 * One warning, said once, is the whole story — so make it true.
 */
const char *program_identity_or_stand_down()
{
    if (ext_strucpp_program_md5) return ext_strucpp_program_md5;
    log_warn("Retain: the program exports no MD5 — retained variables start at their "
             "initial values");
    g_active.store(false);
    {
        std::lock_guard<std::mutex> guard(g_status_lock);
        g_status.start = PLC_RETAIN_START_NONE;
    }
    return nullptr;
}

/**
 * A buffer as large as any blob a store may hold, for the one read per start.
 *
 * NOT this program's blob size. The stored blob may have been written by an
 * older program with more retained variables, and a format-2 blob from it is
 * still migratable by name — so a read capped at THIS program's size would
 * truncate exactly the blob migration exists for. Allocated per start and freed
 * straight after: the scan path never sees it.
 */
std::unique_ptr<uint8_t[]> make_read_buffer()
{
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[RETAIN_BUFFER_MAX]);
    if (!buf)
    {
        log_error("Retain: cannot allocate %zu bytes to read the stored values — retained "
                  "variables start at their initial values",
                  RETAIN_BUFFER_MAX);
    }
    return buf;
}

/**
 * Ask the store for what it holds for this program.
 *
 * Returns the bytes handed back (0 when there is nothing to restore, whatever
 * the reason; the reason is logged here). The call also hands the store the
 * program's identity, which it keeps for labelling its next commit — so a cold
 * restart makes it too, and throws the bytes away.
 */
uint16_t read_from_store(const char *md5, uint8_t *buf, bool quiet)
{
    uint16_t  got = 0;
    const int rc  = g_driver.read(md5, PLC_RETAIN_PROGRAM_ID_LEN, buf, (uint16_t)RETAIN_BUFFER_MAX,
                                  &got);
    if (rc == PLC_RETAIN_STORE_TOO_LARGE)
    {
        log_warn("Retain: %s holds %u bytes, more than the %zu this runtime reads — retained "
                 "variables start at their initial values",
                 g_driver.name ? g_driver.name : "the store", (unsigned)got, RETAIN_BUFFER_MAX);
        return 0;
    }
    if (rc != 0)
    {
        log_warn("Retain: %s could not read its stored values (error %d) — retained variables "
                 "start at their initial values",
                 g_driver.name ? g_driver.name : "the store", rc);
        return 0;
    }
    if (got == 0 && !quiet)
    {
        log_info("Retain: nothing stored yet — retained variables start at their initial values");
    }
    return got;
}

/** Minimal JSON string escaping for the store name (a path from retain.conf). */
void json_escape(std::string &out, const char *s)
{
    for (; s && *s; ++s)
    {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += (char)c;
        }
        else if (c < 0x20)
        {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            out += esc;
        }
        else
        {
            out += (char)c;
        }
    }
}

}  // namespace

void plc_retain_init(void)
{
    g_active.store(false);
    g_plugin_store = nullptr;
    g_driver       = {nullptr, nullptr, nullptr, nullptr};
    g_buffer.clear();
    g_v2 = false;
    {
        std::lock_guard<std::mutex> guard(g_status_lock);
        g_status = RetainStatus{};
    }
    /* Re-read retain.conf on every program load, so settings that arrived with
     * a program upload take effect on the next PLC start without needing the
     * daemon restarted. Stopping first is what forces the re-read, and it also
     * commits anything the previous run was still holding. */
    plc_retain_file_store_stop();

    /* Format 2 when the program offers all three of its entry points: a blob
     * that names its variables, so a changed layout is migrated by name rather
     * than refused. Anything less is the format-1 path exactly as before. */
    g_v2 = ext_strucpp_retain_blob_size2 && ext_strucpp_retain_pack2 && ext_strucpp_retain_unpack2;
    if (!g_v2 &&
        (!ext_strucpp_retain_blob_size || !ext_strucpp_retain_pack || !ext_strucpp_retain_unpack))
    {
        /* A program built by an older STruC++. Not an error — retain simply
         * does not run, exactly as before these exports existed. */
        return;
    }

    const size_t needed = retain_blob_size();
    if (needed == 0) return; /* the program retains nothing */

    if (needed > RETAIN_BUFFER_MAX)
    {
        log_error("Retain: program needs %zu bytes, this runtime handles at most %zu — "
                  "retained variables will NOT be preserved",
                  needed, RETAIN_BUFFER_MAX);
        return;
    }

    /* Ask the drivers, in rank order, which will hold the bytes.
     *
     * A vendor plugin outranks the built-in file store because the vendor knows
     * what the box actually has — FRAM, battery-backed SRAM, an NVS partition —
     * and a file on the data partition is the runtime's default, not its
     * preference. In a correctly declared device only one of them offers itself
     * at all: the file store answers no unless retain.conf enabled it, and the
     * editor emits no retain.conf for a target whose VPP declared that it owns
     * retention. So this is a rank, not an arbitration. */
    g_plugin_store = plugin_driver_find_retain_store(plugin_driver);
    if (g_plugin_store)
    {
        g_driver = {g_plugin_store->config.name, plugin_read_thunk, plugin_write_thunk,
                    plugin_flush_thunk};
    }
    else if (plc_retain_file_store_start("./retain.conf"))
    {
        g_driver = {plc_retain_file_store_path(), plc_retain_file_store_load,
                    plc_retain_file_store_save, plc_retain_file_store_flush};
    }
    else
    {
        /* Info, not a warning: a device with no retention configured is a
         * normal state, and the program still runs correctly — its retained
         * variables just behave as NON_RETAIN. Said once so the operator can
         * tell "not switched on here" from "switched on and broken". */
        log_info("Retain: %zu bytes of retained variables, but no storage is configured — "
                 "they will start at their initial values. Turn on persistent storage in the "
                 "project and upload again, or install a VPP that provides a store.",
                 needed);
        return;
    }

    g_buffer.assign(needed, 0);
    {
        std::lock_guard<std::mutex> guard(g_status_lock);
        g_status.api        = g_v2 ? 2 : 1;
        g_status.blob_bytes = needed;
        g_status.store      = g_driver.name ? g_driver.name : "";
    }
    g_active.store(true);
    log_info("Retain: %zu bytes across the program's retained variables (layout %08x, format %d), "
             "stored by %s",
             needed, program_layout_hash(), g_v2 ? 2 : 1, g_driver.name ? g_driver.name : "?");
}

void plc_retain_read(void)
{
    if (!g_active.load() || !driver_bound()) return;

    /* The program's identity, so the driver can tell whether the bytes it holds
     * belong to the program now running. Resolved from the .so at load time
     * (image_tables.cpp), so it is already available here. Exactly 32
     * characters of hex and NOT guaranteed NUL-terminated, which is why the
     * length travels with it rather than being recovered with strlen. */
    const char *md5 = program_identity_or_stand_down();
    if (!md5) return;

    record_start(PLC_RETAIN_START_WARM, 0, nullptr);

    std::unique_ptr<uint8_t[]> buf = make_read_buffer();
    if (!buf) return;

    const uint16_t got = read_from_store(md5, buf.get(), false);
    if (got == 0) return;

    plc_retain_report_t report = {};
    if (g_v2)
    {
        report.result = ext_strucpp_retain_unpack2(buf.get(), got, retain_write_leaf, &report,
                                                   sizeof(report));
    }
    else
    {
        /* Format 1 reports only its result. Fill in what is known so the status
         * query reads the same either way. */
        report.result         = ext_strucpp_retain_unpack(buf.get(), got, retain_write_leaf);
        report.format         = report.result == PLC_RETAIN_RESULT_OK ? 1 : 0;
        report.program_layout = program_layout_hash();
    }

    record_start(PLC_RETAIN_START_WARM, got, &report);
    log_report(&report, got);
}

void plc_retain_cold_start(void)
{
    if (!g_active.load() || !driver_bound())
    {
        log_info("Retain: cold restart — every variable starts at its initial value");
        return;
    }

    const char *md5 = program_identity_or_stand_down();
    if (!md5) return;

    /* The store is still asked for its bytes, and they are thrown away: the
     * call is how a store learns the identity it labels its next commit with
     * (plugin_driver.h), and a store that never got it would refuse the
     * write below. */
    std::unique_ptr<uint8_t[]> buf = make_read_buffer();
    if (buf) (void)read_from_store(md5, buf.get(), true);
    buf.reset();

    record_start(PLC_RETAIN_START_COLD, 0, nullptr);

    /* IEC 61131-3 Figure 9 rule 4: a cold restart initializes every RETAIN and
     * NON_RETAIN variable. The program was loaded a moment ago, so every
     * variable already holds its declared initial value; skipping the restore
     * is what keeps it that way. But the STORE still holds the old values, and
     * the next warm restart (or a power cut) would bring them back. So hand the
     * initial values to the store now and ask it to commit at once, rather than
     * leaving it to the first scan's save and the store's own schedule. */
    const size_t n = retain_pack(g_buffer.data(), g_buffer.size());
    if (n == 0)
    {
        log_error("Retain: cold restart could not pack the initial values — the store still "
                  "holds the previous ones, and a warm restart would restore them");
        return;
    }
    const int wrc = g_driver.write(g_buffer.data(), (uint16_t)n);
    const int frc = g_driver.flush ? g_driver.flush() : 0;
    if (wrc != 0 || frc != 0)
    {
        log_error("Retain: cold restart — %s did not accept the initial values (write %d, "
                  "flush %d); a warm restart may restore the previous ones",
                  g_driver.name ? g_driver.name : "the store", wrc, frc);
        return;
    }
    log_info("Retain: cold restart — stored values discarded, every variable starts at its "
             "initial value, and %s now holds those (%zu bytes)",
             g_driver.name ? g_driver.name : "the store", n);
}

void plc_retain_save(void)
{
    if (!g_active.load() || !driver_bound()) return;

    const size_t n = retain_pack(g_buffer.data(), g_buffer.size());
    if (n == 0) return;

    /* Hand the bytes over and return. Whether this is committed to storage now,
     * in five seconds, or on shutdown is the driver's decision — it is the only
     * layer that knows what its medium costs. */
    g_driver.write(g_buffer.data(), (uint16_t)n);
}

void plc_retain_flush(void)
{
    if (!g_active.load() || !g_driver.flush) return;
    g_driver.flush();
}

size_t plc_retain_status_json(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;

    RetainStatus st;
    {
        std::lock_guard<std::mutex> guard(g_status_lock);
        st = g_status;
    }

    static const char *const start_name[] = {"none", "warm", "cold"};
    std::string json = "{\"start\":\"";
    json += start_name[st.start <= PLC_RETAIN_START_COLD ? st.start : 0];
    json += "\",\"format\":" + std::to_string(st.api);
    json += ",\"store\":\"";
    json_escape(json, st.store.c_str());
    json += "\",\"blob_bytes\":" + std::to_string(st.blob_bytes);
    json += ",\"stored_bytes\":" + std::to_string(st.stored_bytes);
    if (st.have_report)
    {
        const plc_retain_report_t &r = st.report;
        char layouts[64];
        snprintf(layouts, sizeof(layouts),
                 ",\"stored_layout\":\"%08" PRIx32 "\",\"program_layout\":\"%08" PRIx32 "\"",
                 r.stored_layout, r.program_layout);
        json += ",\"result\":" + std::to_string(r.result);
        json += ",\"result_name\":\"";
        json += r.result < k_result_count ? k_result_name[r.result] : "unknown";
        json += "\",\"restored\":";
        json += (r.result == PLC_RETAIN_RESULT_OK || r.result == PLC_RETAIN_RESULT_MIGRATED)
                    ? "true"
                    : "false";
        json += ",\"stored_format\":" + std::to_string(r.format);
        json += ",\"kept\":" + std::to_string(r.kept);
        json += ",\"converted\":" + std::to_string(r.converted);
        json += ",\"truncated\":" + std::to_string(r.truncated);
        json += ",\"added\":" + std::to_string(r.added);
        json += ",\"dropped\":" + std::to_string(r.dropped);
        json += ",\"refused\":" + std::to_string(r.refused);
        json += layouts;
    }
    else
    {
        json += ",\"result\":null,\"restored\":false";
    }
    json += "}";

    if (json.size() + 1 > cap) return 0;
    memcpy(out, json.c_str(), json.size() + 1);
    return json.size();
}
