// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

// Static C-linkage shim in every user .so. Instantiates g_config,
// re-exposes strucpp::locatedVars[] and the strucpp_debug_* PDU
// helpers under C linkage, provides the cross-DSO time-advance entry,
// and exports strucpp_program_md5 for FC 0x45.

#define STRUCPP_V4_DEBUG_EXPORTS_DEFINE
#include "debug_dispatch.hpp"
#include "iec_located.hpp"
#include "iec_std_lib.hpp"   // ConfigurationInstance + __CURRENT_TIME_NS
#include "generated.hpp"

// Retain marshalling is conditional on the STruC++ version — see
// retain_probe.cpp for how the build decides. Include is behind the
// same gate so older header sets are never asked for it.
#ifdef STRUCPP_SHIM_HAS_RETAIN
#include "iec_retain.hpp"    // retain blob format + pack/unpack
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <pthread.h>

// External linkage so generated_debug.cpp can reference &g_config.X.Y at
// compile time. Same constraint as the Arduino sketch's g_config.
strucpp::Configuration_CONFIG0 g_config;

extern "C" strucpp::ConfigurationInstance* strucpp_get_config(void) {
    return &g_config;
}

// strucpp::locatedVars/locatedVarsCount are externs defined per-project
// by generated.cpp. Re-exported via C linkage so the runtime (one copy,
// many .so files) can reach them portably by dlsym.

extern "C" const strucpp::LocatedVar *strucpp_get_located_vars(void) {
    return strucpp::locatedVars;
}

extern "C" uint32_t strucpp_get_located_var_count(void) {
    return strucpp::locatedVarsCount;
}

// Classifier for runtime_external_write(): reports whether (arr, elem)
// is LOCATED and fills image location out-params so the write is routed
// through the image journal (copy_in would clobber a direct IECVar
// poke). Match is pure pointer identity.
extern "C" int strucpp_debug_locate(uint8_t arr, uint16_t elem,
                                    uint8_t *area, uint8_t *size,
                                    uint16_t *byte_index, uint8_t *bit_index) {
    void *p = strucpp::debug::read_entry(arr, elem).ptr;
    if (p == nullptr) return 0;
    for (uint32_t i = 0; i < strucpp::locatedVarsCount; ++i) {
        if (strucpp::locatedVars[i].pointer == p) {
            const strucpp::LocatedVar &v = strucpp::locatedVars[i];
            if (area)       *area       = static_cast<uint8_t>(v.area);
            if (size)       *size       = static_cast<uint8_t>(v.size);
            if (byte_index) *byte_index = v.byte_index;
            if (bit_index)  *bit_index  = v.bit_index;
            return 1;
        }
    }
    return 0;
}

// Project MD5 for FC 0x45. defines.h is emitted by the editor next to
// generated.cpp during compile with PROGRAM_MD5. No fallback: a program
// loaded without defines.h must fail to compile or link.
#include "defines.h"

// Non-const char array: (1) external linkage for dlsym (namespace-scope
// `const` gives internal linkage in C++); (2) symbol address IS the
// string start, since the runtime indexes it directly.
// extern "C" block avoids the g++ "extern initialized" warning.
extern "C" {
char strucpp_program_md5[] = PROGRAM_MD5;
}

// Advances the strucpp scan-cycle clock on the CALLING thread. Kept for
// single-threaded hosts; the GCD dispatcher uses strucpp_set_current_time
// per worker because __CURRENT_TIME_NS is thread_local when STRUCPP_THREADED.
extern "C" void strucpp_advance_time(uint64_t tick_ns) {
    strucpp::__CURRENT_TIME_NS += static_cast<int64_t>(tick_ns);
}

// Sets IEC TIME() base for the CALLING thread (thread_local under
// STRUCPP_THREADED). Called by each worker at the top of its scan before
// run() so TIME() is scan-stable per task. Must run on the worker thread.
extern "C" void strucpp_set_current_time(int64_t ns) {
    strucpp::__CURRENT_TIME_NS = ns;
}

// NOTE: the runtime no longer probes a "threaded ABI" capability symbol. It
// compiles every .so itself with -DSTRUCPP_THREADED, so the threaded
// process-image model is the only one; there is nothing to detect.

#ifdef STRUCPP_SHIM_HAS_RETAIN

static uint16_t retain_read_leaf(uint8_t arr, uint16_t elem, uint8_t* dest) {
    return strucpp::debug::handle_read(arr, elem, dest);
}

static uint16_t retain_size_leaf(uint8_t arr, uint16_t elem) {
    return strucpp::debug::handle_size(arr, elem);
}

/** Bytes a full blob occupies for this program; 0 when nothing is retained. */
extern "C" size_t strucpp_retain_blob_size(void) {
    return strucpp::retain::blob_size(retain_size_leaf);
}

/** Identity of the retain LAYOUT — reported so the runtime can log it. */
extern "C" uint32_t strucpp_retain_layout_hash(void) {
    return strucpp::debug::retain_layout_hash;
}

/** Serialise every retained leaf. Returns bytes written, 0 on failure. */
extern "C" size_t strucpp_retain_pack(uint8_t* out, size_t cap) {
    return strucpp::retain::pack(out, cap, retain_read_leaf, retain_size_leaf);
}

/* Restore every retained leaf via write_leaf. Returns
 * strucpp::retain::LoadResult as a byte; non-zero means nothing was
 * written and variables keep their declared initial values. */
extern "C" uint8_t strucpp_retain_unpack(
    const uint8_t* blob,
    size_t len,
    uint8_t (*write_leaf)(uint8_t arr, uint16_t elem, const uint8_t* bytes, uint16_t n)) {
    return static_cast<uint8_t>(
        strucpp::retain::unpack(blob, len, write_leaf, retain_size_leaf));
}

// ---------------------------------------------------------------------------
// Retain format 2: migration by name.
//
// GATED SEPARATELY, on its own probe (STRUCPP_SHIM_HAS_RETAIN_V2, see
// scripts/Makefile.strucpp): a header set can have the format-1 API above and
// not this one, and such an upload must still build and keep format 1. The
// runtime uses these three only when all of them resolve.
//
// A format-2 blob carries a table naming every stored variable, so when the
// retained variables change (one added, removed, retyped, reordered) the
// values of those that still exist are restored by name, as IEC 61131-3
// 6.5.6.1 rule 1 expects of a warm restart, instead of the whole blob being
// refused. The matching and the conversion rules live in iec_retain.hpp; this
// file only wires the leaf accessors to them.
//
// Same split as format 1: reads and the leaf table come from this .so, and the
// SCALAR write is the runtime's callback, because a retained variable may also
// be located and must go through the image journal. Strings use
// handle_write_text directly: a STRING/WSTRING is never located, and restore
// runs before any task is released.
//
// Anything added here that touches a v2 name MUST stay inside this #ifdef and
// be mirrored into retain_probe.cpp's STRUCPP_RETAIN_PROBE_V2 section (pinned by
// tests/pytest/compile/test_retain_capability_probe.py).
// ---------------------------------------------------------------------------
#ifdef STRUCPP_SHIM_HAS_RETAIN_V2

static_assert(sizeof(strucpp::retain::Report) == 24,
              "the runtime mirrors Report as a 24-byte C struct (plc_retain_report_t)");

// Packing and sizing never write a variable. A refusing stand-in rather than a
// null pointer, so a header set that ever did call it fails safe.
static uint8_t retain_no_write(uint8_t, uint16_t, const uint8_t*, uint16_t) {
    return 0x82;
}

static strucpp::retain::Host retain_host(strucpp::retain::WriteLeaf write) {
    strucpp::retain::Host host{};
    host.leaf       = strucpp::debug::handle_retain_leaf;
    host.read       = retain_read_leaf;
    host.write      = write ? write : retain_no_write;
    host.read_text  = strucpp::debug::handle_read_text;
    host.write_text = strucpp::debug::handle_write_text;
    host.wire_size  = retain_size_leaf;
    return host;
}

/** Bytes a full format-2 blob occupies (header, payload, table); 0 when nothing is retained. */
extern "C" size_t strucpp_retain_blob_size2(void) {
    return strucpp::retain::blob_size2(retain_host(retain_no_write));
}

/** Serialise every retained leaf as format 2. Returns bytes written, 0 on failure. */
extern "C" size_t strucpp_retain_pack2(uint8_t* out, size_t cap) {
    return strucpp::retain::pack2(out, cap, retain_host(retain_no_write));
}

/**
 * Restore from a format-1 or format-2 blob, writing scalars through the
 * runtime's callback.
 *
 * Returns `strucpp::retain::LoadResult` as a byte; only Ok (0) and Migrated (7)
 * wrote anything. `report` (may be NULL) receives min(report_size, 24) bytes
 * of `strucpp::retain::Report`: what was kept, converted, added, dropped and
 * refused, and the two layout hashes.
 */
extern "C" uint8_t strucpp_retain_unpack2(
    const uint8_t* blob,
    size_t len,
    uint8_t (*write_leaf)(uint8_t arr, uint16_t elem, const uint8_t* bytes, uint16_t n),
    void* report,
    size_t report_size) {
    strucpp::retain::Report r{};
    const strucpp::retain::LoadResult res =
        strucpp::retain::unpack2(blob, len, retain_host(write_leaf), &r);
    if (report != nullptr && report_size != 0) {
        std::memcpy(report, &r, report_size < sizeof(r) ? report_size : sizeof(r));
    }
    return static_cast<uint8_t>(res);
}

#endif // STRUCPP_SHIM_HAS_RETAIN_V2

#endif // STRUCPP_SHIM_HAS_RETAIN
