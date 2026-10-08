// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

// Build-time capability probe for the strucpp retain API in the
// upload's headers. -fsyntax-only; only exit status matters. KEEP IN
// SYNC with every strucpp::retain / retain_layout_hash name the shim
// touches, else the probe passes for headers the shim will not build. */

#include <cstddef>
#include <cstdint>

#include "debug_table.hpp"
#include "iec_retain.hpp"

// Format 2 (migration by name) is probed SEPARATELY: Makefile.strucpp compiles
// this file a second time with -DSTRUCPP_RETAIN_PROBE_V2, and only that second
// verdict defines STRUCPP_SHIM_HAS_RETAIN_V2. A header set with the format-1
// API and not this one must still build, with format 1. Both halves compile in
// that second pass, so a v2 verdict always implies a v1 one.
#ifdef STRUCPP_RETAIN_PROBE_V2
#include "debug_dispatch.hpp"
#endif

// Stand-ins for the shim's leaf accessors. Same signatures, so the probe
// exercises the real template argument deduction rather than just the name.
static uint16_t probe_read_leaf(uint8_t, uint16_t, uint8_t *) { return 0; }
static uint16_t probe_size_leaf(uint8_t, uint16_t) { return 0; }
static uint8_t probe_write_leaf(uint8_t, uint16_t, const uint8_t *, uint16_t) { return 0; }

size_t strucpp_retain_probe(uint8_t *out, size_t cap, const uint8_t *blob, size_t len)
{
    return strucpp::retain::blob_size(probe_size_leaf) +
           strucpp::retain::pack(out, cap, probe_read_leaf, probe_size_leaf) +
           static_cast<size_t>(
               strucpp::retain::unpack(blob, len, probe_write_leaf, probe_size_leaf)) +
           static_cast<size_t>(strucpp::debug::retain_layout_hash);
}

#ifdef STRUCPP_RETAIN_PROBE_V2
// KEEP IN SYNC with the STRUCPP_SHIM_HAS_RETAIN_V2 block of the shim: the Host
// it fills, field by field from the same accessors, and the three calls.
size_t strucpp_retain_probe_v2(uint8_t *out, size_t cap, const uint8_t *blob, size_t len)
{
    strucpp::retain::Host host{};
    host.leaf       = strucpp::debug::handle_retain_leaf;
    host.read       = strucpp::debug::handle_read;
    host.write      = probe_write_leaf;
    host.read_text  = strucpp::debug::handle_read_text;
    host.write_text = strucpp::debug::handle_write_text;
    host.wire_size  = strucpp::debug::handle_size;

    static_assert(sizeof(strucpp::retain::Report) == 24, "Report is a 24-byte wire mirror");
    strucpp::retain::Report report{};
    const strucpp::retain::LoadResult res = strucpp::retain::unpack2(blob, len, host, &report);
    return strucpp::retain::blob_size2(host) + strucpp::retain::pack2(out, cap, host) +
           static_cast<size_t>(res) + report.kept;
}
#endif
