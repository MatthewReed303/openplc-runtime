// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

/**
 * @file plc_retain.h
 * @brief Retain-variable persistence — the runtime's half (NODE-94).
 *
 * The runtime MARSHALS and the platform STORES, exactly as on baremetal. The
 * marshalling itself lives inside the loaded .so (STruC++'s `iec_retain.hpp`,
 * reached through the `strucpp_retain_*` exports), because that is where the
 * debug tables live and because one copy of a wire format is better than two.
 * What this file owns is the buffer and the call sites.
 *
 * ONE DRIVER INTERFACE, SEVERAL DRIVERS
 * ------------------------------------
 * Storage reaches this file as a `retain_driver_t` — three function pointers,
 * filled in once at init by whichever driver claimed the role. Nothing below
 * that point knows or asks which kind of store it got: a VPP plugin holding
 * FRAM and the runtime's own file store are the same shape, called at the same
 * points, and neither is a special case in an `if`.
 *
 * The built-in file store is therefore a DRIVER, not a fallback branch. It is
 * also the lowest-ranked one and the only one that has to be switched on: it
 * declares itself available only when `retain.conf` enables it, which the
 * editor emits from the project's Persistent Storage settings. A VPP that owns
 * retention declares `hidesNativeScreens: ['persistent-storage']`, the editor
 * emits no `retain.conf`, the upload removes any stale copy, and the file store
 * simply does not offer itself — leaving the vendor's driver as the only
 * candidate. Replacement needs no priority table because a correctly declared
 * device presents exactly one store.
 *
 * With no driver at all the calls are no-ops and retain degrades to NON_RETAIN,
 * which is what the runtime did before any of this existed.
 *
 * THE THREE CALLS, AND WHEN THEY HAPPEN
 * ------------------------------------
 *
 *     start   plc_retain_read()    once, before the first scan
 *     scan    plc_retain_save()    every cycle, WHILE RUNNING ONLY
 *     stop    plc_retain_flush()   once, as the program is unloaded
 *
 * A COLD restart calls plc_retain_cold_start() where plc_retain_read() would
 * be: no restore, and the store is overwritten with the initial values through
 * the same three calls, so a store needs nothing new to support it.
 *
 * Identical, deliberately, to baremetal's `openplc_retain.h`: same names, same
 * order, same meaning, so a vendor reads one contract and implements the same
 * shape twice.
 *
 * CADENCE IS NOT OURS TO DECIDE
 * ----------------------------
 * `plc_retain_save()` is called once per scan cycle, unconditionally, for as
 * long as the PLC is running. It does not diff, does not rate-limit and does
 * not judge whether a value is worth keeping — the driver holds the bytes and
 * commits on whatever schedule its medium can sustain. A driver over flash that
 * wrote through on every call would consume its endurance budget in hours; one
 * over FRAM is free to write every cycle, which is why the call happens at that
 * cadence at all.
 *
 * SAVE IS THE DURABILITY PATH; FLUSH IS ONLY A HINT. A power cut does not call
 * flush(), and retention exists for the power cut nobody schedules.
 */

#ifndef PLC_RETAIN_H
#define PLC_RETAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Length of the program identity passed to a driver's read hook.
 *
 * An MD5 as lower-case hex: exactly 32 characters, and NOT NUL-terminated —
 * compare with memcmp over this length, never strcmp.
 *
 * Defined once, here, because three copies of the number existed and were tied
 * together only by comments. Baremetal's `OPLC_RETAIN_PROGRAM_ID_LEN` is the
 * fourth, and deliberately stays separate: it sits across a process and
 * toolchain boundary where a shared header would be worse than a documented
 * constant. It must carry the same value.
 */
#define PLC_RETAIN_PROGRAM_ID_LEN 32

/**
 * @brief The largest blob this runtime reads, packs or hands to a store.
 *
 * Every length on the store interface is a uint16_t, so 65535 is the real
 * ceiling. It was once written as 64 * 1024, which is one more than that: a
 * program needing exactly 65536 bytes passed the size check and was then
 * handed to its store with a capacity that truncated to 0.
 */
#define PLC_RETAIN_BLOB_MAX 65535u

/**
 * @brief A store's answer when what it holds is larger than the caller's buffer.
 *
 * The read hook's `cap` is the CALLER's buffer, which is PLC_RETAIN_BLOB_MAX
 * and so usually larger than this program's blob: a blob written by an older
 * program with more retained variables must still be readable, because a
 * format-2 blob is migrated by name. A store must never refuse a stored blob
 * for being larger than the program's own blob size. When it really does not
 * fit, the store returns this and sets *out_len to the stored length. Same
 * number as baremetal's OPLC_RETAIN_TOO_LARGE.
 */
#define PLC_RETAIN_STORE_TOO_LARGE 4

/**
 * @brief What a restore did, as STruC++'s `strucpp::retain::LoadResult`.
 *
 * The numbers are ABI: the .so returns them as a byte and this runtime logs
 * and reports them by number, so they are mirrored here rather than shared.
 * Only OK and MIGRATED wrote any value; every other result left every
 * retained variable at its declared initial value.
 */
enum
{
    PLC_RETAIN_RESULT_OK           = 0, /* identical layout (or format 1, same hash) */
    PLC_RETAIN_RESULT_EMPTY        = 1,
    PLC_RETAIN_RESULT_BAD_MAGIC    = 2,
    PLC_RETAIN_RESULT_BAD_FORMAT   = 3,
    PLC_RETAIN_RESULT_BAD_CRC      = 4,
    PLC_RETAIN_RESULT_STALE_LAYOUT = 5, /* format 1 from another layout: cannot migrate */
    PLC_RETAIN_RESULT_TRUNCATED    = 6,
    PLC_RETAIN_RESULT_MIGRATED     = 7, /* format 2, layout changed, matched by name */
    PLC_RETAIN_RESULT_BAD_TRAILER  = 8,
};

/**
 * @brief Mirror of STruC++'s `strucpp::retain::Report` (24 bytes, POD).
 *
 * Filled by `strucpp_retain_unpack2`, which copies min(size, 24) bytes into
 * whatever the caller passes — so the layout below is a wire contract with the
 * .so, not a convenience, and the static assertion is what keeps it one.
 */
typedef struct
{
    uint8_t  result;         /* PLC_RETAIN_RESULT_* */
    uint8_t  format;         /* stored blob's format, 0 = none / unknown */
    uint16_t kept;           /* copied with the same type (includes `truncated`) */
    uint16_t converted;      /* IEC 61131-3 Figure 12 implicit conversion */
    uint16_t truncated;      /* strings shortened to a smaller declared length */
    uint16_t added;          /* no stored value: declared initial value */
    uint16_t dropped;        /* stored values whose variable no longer exists */
    uint16_t refused;        /* matched by name, type not convertible (or write refused) */
    uint16_t _pad;
    uint32_t stored_layout;  /* layout hash in the stored blob's header */
    uint32_t program_layout; /* layout hash of the program now loaded */
} plc_retain_report_t;

#ifdef __cplusplus
static_assert(sizeof(plc_retain_report_t) == 24,
              "plc_retain_report_t must mirror strucpp::retain::Report byte for byte");
#else
_Static_assert(sizeof(plc_retain_report_t) == 24,
               "plc_retain_report_t must mirror strucpp::retain::Report byte for byte");
#endif

/**
 * @brief How the last program start treated retained values.
 */
typedef enum
{
    PLC_RETAIN_START_NONE = 0, /* retain not in play: no exports, nothing retained, or no store */
    PLC_RETAIN_START_WARM = 1, /* restore attempted (IEC 61131-3 6.5.6.1 rule 1) */
    PLC_RETAIN_START_COLD = 2, /* restore skipped, store reset (IEC 61131-3 Figure 9 rule 4) */
} plc_retain_start_t;

/**
 * @brief Decide once, after the program is loaded, whether retain can run, and
 *        bind whichever driver claimed the store.
 *
 * Checks that the .so exports the retain entry points (a program built by an
 * older STruC++ does not), that the program retains anything at all, and that
 * the blob fits the runtime's buffer. Then asks the drivers, in rank order,
 * which of them will hold the bytes. Logs what it found, once — including the
 * layout hash, which is the thing to compare when a restore is unexpectedly
 * refused, and the name of the store that won.
 *
 * Call after symbols are resolved and `g_config` is constructed, before the
 * first task is released.
 */
void plc_retain_init(void);

/**
 * @brief Restore the retained values the driver is holding FOR THIS PROGRAM.
 *
 * The running program's identity (`strucpp_program_md5`) goes to the driver,
 * which decides whether what it holds still belongs here. A driver that finds a
 * different program's values discards them, logs that storage was cleared, and
 * reports empty — so every retained variable starts at its declared initial
 * value, which is what a new program means. That decision is the driver's
 * because it is inseparable from how the driver stores things, and because it
 * is the only way a store can be correct without the runtime having to be told
 * when an upload happened.
 *
 * On top of that the blob is validated inside the .so (magic, format, layout
 * hash, crc32), and one that fails leaves every variable at its initial value —
 * a machine starting from its defaults is recoverable, one starting from
 * plausible-looking garbage is not.
 *
 * A format-2 blob (STruC++ with `strucpp_retain_unpack2`) carries a table of
 * the variables it holds, so a changed layout is migrated BY NAME instead of
 * refused: a variable that still exists gets its value back (IEC 61131-3
 * 6.5.6.1 rule 1), a new one starts at its initial value (6.5.6.2), and the
 * outcome is logged as counts and kept for plc_retain_status_json(). The read
 * uses a PLC_RETAIN_BLOB_MAX buffer, not this program's blob size, because the
 * stored blob may come from an older program with more retained variables.
 *
 * The values are applied before this returns (not left for the dispatcher's
 * cycle-end drain), so scan 1 sees them.
 *
 * Safe and cheap when nothing is retained or no driver claimed the store, and
 * idempotent: call once per program start, after plc_retain_init() and
 * journal_init(), before any task is released.
 */
void plc_retain_read(void);

/**
 * @brief Hand the current retained values to the driver.
 *
 * Called ONCE PER SCAN CYCLE from the dispatcher's quiescent window, where
 * `g_tasks_running == 0` and no worker is inside a body — the same guarantee
 * `image_tables_copy_config_globals_out()` relies on. Reading the leaves
 * anywhere else would race the task threads.
 */
void plc_retain_save(void);

/**
 * @brief Ask the driver to commit anything it is still holding, now.
 *
 * Called once as the program is unloaded, after the cycle thread has been
 * joined so no scan is mid-save, and before the plugins are stopped so a
 * plugin-backed store is still alive to answer.
 *
 * A hint, not the durability mechanism — see the note at the top of this file.
 * What it buys is that a CLEAN stop loses nothing on a driver that buffers.
 */
void plc_retain_flush(void);

/**
 * @brief The COLD restart counterpart of plc_retain_read().
 *
 * IEC 61131-3 Figure 9 rule 4 (p.57): a cold restart initializes every RETAIN
 * and NON_RETAIN variable. The program was just loaded, so every variable is
 * already at its declared initial value; what remains is to skip the restore
 * and to make the STORE agree. So this asks the store for its bytes only to
 * hand it the program's identity (a store labels its commits with the identity
 * load() gave it), discards them, packs the initial values and writes and
 * flushes them at once. A power cut after this returns cannot bring the old
 * values back on the next warm restart.
 *
 * Same calling point and preconditions as plc_retain_read(), in its place.
 */
void plc_retain_cold_start(void);

/**
 * @brief The last start's retain outcome as one line of JSON, for queries.
 *
 * Writes at most `cap` bytes including the NUL and returns the length written
 * (0 when `cap` is too small). The fields are stable; see plc_retain.cpp.
 */
size_t plc_retain_status_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RETAIN_H */
