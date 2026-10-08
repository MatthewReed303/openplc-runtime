# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Autonomy®

"""The retain capability probe, and the gate it drives.

Why this file exists
--------------------
Runtime v4.2.0 shipped a shim (``core/strucpp_runtime/runtime_v4_entry.cpp``)
that calls ``strucpp::retain`` and ``strucpp::debug::retain_layout_hash``. That
file is the ONE runtime source compiled against STruC++ headers the EDITOR
shipped inside ``program.zip``, and the retain API landed in STruC++ v0.6.5 --
newer than the STruC++ pinned by every OpenPLC Editor released to date. So the
release could not build a program from any editor in the field, and nothing in
the suite noticed, because nothing here compiles the shim against a header set
older than the developer's own.

``retain_probe.cpp`` plus ``SHIM_HAS_RETAIN`` in ``scripts/Makefile.strucpp``
are the fix: measure the header set per upload, and compile the retain block out
when it cannot supply the API. These tests pin the three ways that gate can rot.

1. **The probe discriminates.** It must fail against a pre-v0.6.5 header shape
   and pass against a v0.6.5+ one. The legacy stub below is modelled on the real
   v0.6.2 ``iec_retain.hpp``: the file EXISTS and declares things in
   ``namespace strucpp``, which is exactly why a "does the header exist" check or
   a grep for the word ``retain`` would both have passed it.
2. **The shim's retain code stays behind the gate.** A single new
   ``strucpp::retain`` call added outside the ``#ifdef`` reintroduces the
   original bug in full.
3. **The probe covers what the shim uses.** A probe that checks less than the
   shim calls would pass for a header set the shim cannot compile against --
   turning a clear build error into a confusing one.

Retain format 2 (migration by name) repeats all three for a SECOND gate,
``STRUCPP_SHIM_HAS_RETAIN_V2``, measured by the same probe compiled again with
``-DSTRUCPP_RETAIN_PROBE_V2``. A header set with the format-1 API and not the
format-2 one is every editor released before format 2, and it must keep
building with format 1 -- so the two verdicts are independent, and the v2 one is
only taken when the v1 one passed.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[3]
_SHIM = _REPO / "core" / "strucpp_runtime" / "runtime_v4_entry.cpp"
_PROBE = _REPO / "core" / "strucpp_runtime" / "retain_probe.cpp"
_MAKEFILE = _REPO / "scripts" / "Makefile.strucpp"

_CXX = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")

# Every name in the shim that only a v0.6.5+ header set can satisfy. Kept as a
# list rather than a regex so a failure names the missing one.
_RETAIN_NAMES = (
    "strucpp::retain::blob_size",
    "strucpp::retain::pack",
    "strucpp::retain::unpack",
    "strucpp::debug::retain_layout_hash",
)

# Every name the shim's format-2 block uses that only a format-2 header set can
# satisfy. These must sit inside `#ifdef STRUCPP_SHIM_HAS_RETAIN_V2`, and the
# probe's v2 section must touch every one.
_RETAIN_V2_NAMES = (
    "strucpp::retain::Host",
    "strucpp::retain::Report",
    "strucpp::retain::blob_size2",
    "strucpp::retain::pack2",
    "strucpp::retain::unpack2",
    "strucpp::debug::handle_retain_leaf",
    "strucpp::debug::handle_read_text",
    "strucpp::debug::handle_write_text",
)

# --- header stubs ----------------------------------------------------------
#
# Deliberately minimal. The probe includes exactly two headers, so these are all
# it can see, and hand-written stubs let the test state the SHAPE that matters
# without vendoring a copy of STruC++ into the runtime repo.

_DEBUG_TABLE_COMMON = """
#pragma once
#include <cstdint>
namespace strucpp {
namespace debug {
extern const uint16_t retain_var_count;
}  // namespace debug
}  // namespace strucpp
"""

# v0.6.5+ added the layout-hash declaration alongside the retain table.
_DEBUG_TABLE_CAPABLE = (
    _DEBUG_TABLE_COMMON
    + """
namespace strucpp {
namespace debug {
extern const uint32_t retain_layout_hash;
}  // namespace debug
}  // namespace strucpp
"""
)

# Modelled on the real v0.6.1 - v0.6.4 header: present, in namespace strucpp,
# talks about retain variables, and offers the shim nothing it can call.
_IEC_RETAIN_LEGACY = """
#pragma once
#include <cstddef>
#include <cstdint>
namespace strucpp {
// Metadata for a retain variable. A future namespace retain will own the walk.
struct RetainVarDescriptor {
    const char* name;
    void*       storage;
    uint16_t    size;
};
}  // namespace strucpp
"""

# The v0.6.5 shape: same three entry points the real header exposes, same
# argument order, templated on the leaf accessors.
_IEC_RETAIN_CAPABLE = """
#pragma once
#include <cstddef>
#include <cstdint>
#include "debug_table.hpp"
namespace strucpp {
namespace retain {
enum class LoadResult : uint8_t { Ok = 0, Empty = 1, StaleLayout = 2 };

template <typename SizeLeaf>
inline size_t blob_size(SizeLeaf size_of) noexcept { (void)size_of; return 0; }

template <typename ReadLeaf, typename SizeLeaf>
inline size_t pack(uint8_t* out, size_t cap, ReadLeaf read_leaf, SizeLeaf size_of) noexcept {
    (void)out; (void)cap; (void)read_leaf; (void)size_of; return 0;
}

template <typename WriteLeaf, typename SizeLeaf>
inline LoadResult unpack(const uint8_t* blob, size_t len,
                         WriteLeaf write_leaf, SizeLeaf size_of) noexcept {
    (void)blob; (void)len; (void)write_leaf; (void)size_of; return LoadResult::Ok;
}
}  // namespace retain
}  // namespace strucpp
"""


# Format 2 adds the per-leaf table accessor to debug_table.hpp...
_DEBUG_TABLE_V2 = (
    _DEBUG_TABLE_CAPABLE
    + """
namespace strucpp {
namespace debug {
struct RetainLeafInfo {
    uint8_t arr; uint16_t elem; uint32_t id; int32_t index; uint8_t tag; uint8_t cap;
};
}  // namespace debug
}  // namespace strucpp
"""
)

# ...the debug accessors every header set has (debug_dispatch.hpp)...
_DEBUG_DISPATCH_COMMON = """
#pragma once
#include <cstdint>
#include "debug_table.hpp"
namespace strucpp {
namespace debug {
inline uint16_t handle_read(uint8_t, uint16_t, uint8_t*) noexcept { return 0; }
inline uint16_t handle_size(uint8_t, uint16_t) noexcept { return 0; }
inline uint8_t handle_write(uint8_t, uint16_t, const uint8_t*, uint16_t) noexcept { return 0; }
}  // namespace debug
}  // namespace strucpp
"""

# ...plus the three format 2 adds: the leaf table and the string accessors.
_DEBUG_DISPATCH_V2 = (
    _DEBUG_DISPATCH_COMMON
    + """
namespace strucpp {
namespace debug {
inline bool handle_retain_leaf(uint16_t, RetainLeafInfo*) noexcept { return false; }
inline uint16_t handle_read_text(uint8_t, uint16_t, uint8_t*, uint16_t) noexcept { return 0; }
inline uint8_t handle_write_text(uint8_t, uint16_t, const uint8_t*, uint16_t) noexcept {
    return 0;
}
}  // namespace debug
}  // namespace strucpp
"""
)

# The format-2 iec_retain.hpp: the format-1 entry points unchanged, plus the
# Host record, the 24-byte Report and the three *2 entry points.
_IEC_RETAIN_V2 = """
#pragma once
#include <cstddef>
#include <cstdint>
#include "debug_table.hpp"
namespace strucpp {
namespace retain {
enum class LoadResult : uint8_t { Ok = 0, Empty = 1, StaleLayout = 5, Migrated = 7, BadTrailer = 8 };
using ReadLeaf = uint16_t (*)(uint8_t, uint16_t, uint8_t*);
using WriteLeaf = uint8_t (*)(uint8_t, uint16_t, const uint8_t*, uint16_t);
using SizeLeaf = uint16_t (*)(uint8_t, uint16_t);
using LeafAt = bool (*)(uint16_t, debug::RetainLeafInfo*);
using ReadText = uint16_t (*)(uint8_t, uint16_t, uint8_t*, uint16_t);
using WriteText = uint8_t (*)(uint8_t, uint16_t, const uint8_t*, uint16_t);
struct Host {
    LeafAt leaf; ReadLeaf read; WriteLeaf write;
    ReadText read_text; WriteText write_text; SizeLeaf wire_size;
};
struct Report {
    uint8_t result; uint8_t format;
    uint16_t kept, converted, truncated, added, dropped, refused, _pad;
    uint32_t stored_layout; uint32_t program_layout;
};
inline size_t blob_size(SizeLeaf) noexcept { return 0; }
inline size_t pack(uint8_t*, size_t, ReadLeaf, SizeLeaf) noexcept { return 0; }
inline LoadResult unpack(const uint8_t*, size_t, WriteLeaf, SizeLeaf) noexcept {
    return LoadResult::Ok;
}
inline size_t blob_size2(const Host&) noexcept { return 0; }
inline size_t pack2(uint8_t*, size_t, const Host&) noexcept { return 0; }
inline LoadResult unpack2(const uint8_t*, size_t, const Host&, Report*) noexcept {
    return LoadResult::Ok;
}
}  // namespace retain
}  // namespace strucpp
"""


def _header_set(root: Path, *, capable: bool, v2: bool = False, v2_dispatch: bool = True) -> Path:
    """Write a stub `strucpp_runtime/include` and return it.

    `capable` is the format-1 API (v0.6.5+), `v2` adds format 2. `v2_dispatch`
    False models a half-ported header set: iec_retain.hpp has format 2 but
    debug_dispatch.hpp lacks the accessors the shim wires into it.
    """
    include = root / "strucpp_runtime" / "include"
    include.mkdir(parents=True)
    if v2:
        table, retain = _DEBUG_TABLE_V2, _IEC_RETAIN_V2
        dispatch = _DEBUG_DISPATCH_V2 if v2_dispatch else _DEBUG_DISPATCH_COMMON
    else:
        table = _DEBUG_TABLE_CAPABLE if capable else _DEBUG_TABLE_COMMON
        retain = _IEC_RETAIN_CAPABLE if capable else _IEC_RETAIN_LEGACY
        dispatch = _DEBUG_DISPATCH_COMMON
    (include / "debug_table.hpp").write_text(table, encoding="utf-8")
    (include / "iec_retain.hpp").write_text(retain, encoding="utf-8")
    (include / "debug_dispatch.hpp").write_text(dispatch, encoding="utf-8")
    return include


def _run_probe(include: Path, *, v2: bool = False) -> subprocess.CompletedProcess:
    """The probe exactly as Makefile.strucpp runs it (`v2`: the second pass)."""
    return subprocess.run(
        [
            _CXX,
            "-std=c++17",
            "-DSTRUCPP_THREADED",
            *(["-DSTRUCPP_RETAIN_PROBE_V2"] if v2 else []),
            "-I",
            str(include),
            "-fsyntax-only",
            str(_PROBE),
        ],
        capture_output=True,
        text=True,
        check=False,
    )


# ---------------------------------------------------------------------------
# 1. The probe discriminates
# ---------------------------------------------------------------------------


@pytest.mark.skipif(_CXX is None, reason="no C++ compiler on PATH")
def test_the_probe_fails_against_pre_v065_headers(tmp_path):
    """The v4.2.0 regression, reproduced at the level that catches it.

    If this ever passes, the gate has stopped gating and every editor in the
    field is one release away from being unable to upload again.
    """
    result = _run_probe(_header_set(tmp_path, capable=False))

    assert result.returncode != 0, "probe accepted a header set with no retain API"
    # And it fails for the RIGHT reason -- not a typo or a missing include path.
    assert "retain" in result.stderr


@pytest.mark.skipif(_CXX is None, reason="no C++ compiler on PATH")
def test_the_probe_passes_against_v065_headers(tmp_path):
    result = _run_probe(_header_set(tmp_path, capable=True))

    assert result.returncode == 0, result.stderr


@pytest.mark.skipif(_CXX is None, reason="no C++ compiler on PATH")
def test_a_missing_iec_retain_header_is_not_a_build_failure(tmp_path):
    """Older still: a header set without the file at all.

    The Makefile's wildcard guard is what handles this, but the probe must fail
    cleanly rather than do something surprising if it is ever run anyway.
    """
    include = tmp_path / "strucpp_runtime" / "include"
    include.mkdir(parents=True)
    (include / "debug_table.hpp").write_text(_DEBUG_TABLE_COMMON, encoding="utf-8")

    assert _run_probe(include).returncode != 0


@pytest.mark.skipif(_CXX is None, reason="no C++ compiler on PATH")
def test_the_v2_probe_fails_against_a_format_1_header_set(tmp_path):
    """Every editor released before format 2: format 1 yes, format 2 no."""
    include = _header_set(tmp_path, capable=True)

    assert _run_probe(include).returncode == 0, "format 1 must still be detected"
    result = _run_probe(include, v2=True)
    assert result.returncode != 0, "v2 probe accepted a header set with no format-2 API"
    assert "unpack2" in result.stderr or "Host" in result.stderr or "Report" in result.stderr


@pytest.mark.skipif(_CXX is None, reason="no C++ compiler on PATH")
def test_the_v2_probe_passes_against_a_format_2_header_set(tmp_path):
    include = _header_set(tmp_path, capable=True, v2=True)

    v1 = _run_probe(include)
    v2 = _run_probe(include, v2=True)
    assert v1.returncode == 0, v1.stderr
    assert v2.returncode == 0, v2.stderr


@pytest.mark.skipif(_CXX is None, reason="no C++ compiler on PATH")
def test_the_v2_probe_checks_the_debug_accessors_too(tmp_path):
    """A header set whose iec_retain.hpp has format 2 but whose debug_dispatch.hpp
    lacks handle_retain_leaf / handle_*_text cannot build the shim's v2 block, so
    the probe must say no rather than leave the shim to fail."""
    include = _header_set(tmp_path, capable=True, v2=True, v2_dispatch=False)

    result = _run_probe(include, v2=True)
    assert result.returncode != 0
    assert "handle_retain_leaf" in result.stderr or "handle_read_text" in result.stderr


@pytest.mark.skipif(_CXX is None, reason="no C++ compiler on PATH")
def test_the_v2_probe_fails_against_pre_v065_headers(tmp_path):
    """The v2 pass compiles the v1 half too, so a v2 verdict implies a v1 one."""
    assert _run_probe(_header_set(tmp_path, capable=False), v2=True).returncode != 0


# ---------------------------------------------------------------------------
# 2. The shim's retain code stays behind the gate
# ---------------------------------------------------------------------------


def _gated_regions(source: str, macro: str = "STRUCPP_SHIM_HAS_RETAIN") -> list[tuple[int, int]]:
    """Line spans covered by `#ifdef <macro>` ... `#endif`.

    Counts nesting so an inner #if inside the block does not close it early.
    The macro is matched as a whole word, so STRUCPP_SHIM_HAS_RETAIN does not
    also match the _V2 gate nested inside it.
    """
    spans: list[tuple[int, int]] = []
    start: int | None = None
    depth = 0
    opener = re.compile(r"#\s*if(def)?\s+.*\b" + re.escape(macro) + r"\b")
    for lineno, line in enumerate(source.splitlines(), start=1):
        stripped = line.strip()
        if start is None:
            if opener.match(stripped):
                start, depth = lineno, 1
            continue
        if re.match(r"#\s*if", stripped):
            depth += 1
        elif re.match(r"#\s*endif", stripped):
            depth -= 1
            if depth == 0:
                spans.append((start, lineno))
                start = None
    assert start is None, f"unterminated {macro} block in the shim"
    return spans


@pytest.mark.parametrize("name", _RETAIN_NAMES)
def test_every_retain_reference_in_the_shim_is_gated(name):
    """One ungated call is the whole v4.2.0 bug back again."""
    source = _SHIM.read_text(encoding="utf-8")
    spans = _gated_regions(source)
    assert spans, "the shim has no STRUCPP_SHIM_HAS_RETAIN block at all"

    for lineno, line in enumerate(source.splitlines(), start=1):
        code = line.split("//", 1)[0]
        if name not in code:
            continue
        assert any(lo <= lineno <= hi for lo, hi in spans), (
            f"{_SHIM.name}:{lineno} uses {name} outside "
            f"#ifdef STRUCPP_SHIM_HAS_RETAIN -- an upload from an editor older "
            f"than v4.2.12 will fail to build"
        )


@pytest.mark.parametrize("name", _RETAIN_V2_NAMES)
def test_every_v2_reference_in_the_shim_is_behind_the_v2_gate(name):
    """Behind the v1 gate is not enough: a format-1 header set passes that one."""
    source = _SHIM.read_text(encoding="utf-8")
    spans = _gated_regions(source, "STRUCPP_SHIM_HAS_RETAIN_V2")
    assert spans, "the shim has no STRUCPP_SHIM_HAS_RETAIN_V2 block at all"

    seen = False
    for lineno, line in enumerate(source.splitlines(), start=1):
        code = line.split("//", 1)[0]
        if name not in code:
            continue
        seen = True
        assert any(lo <= lineno <= hi for lo, hi in spans), (
            f"{_SHIM.name}:{lineno} uses {name} outside #ifdef STRUCPP_SHIM_HAS_RETAIN_V2 -- "
            f"an upload from an editor with format 1 only will fail to build"
        )
    assert seen, f"the shim no longer uses {name}; drop it from _RETAIN_V2_NAMES"


def test_the_v2_block_sits_inside_the_v1_block():
    """The v2 exports reuse the v1 block's leaf accessors and layout hash."""
    source = _SHIM.read_text(encoding="utf-8")
    outer = _gated_regions(source)
    inner = _gated_regions(source, "STRUCPP_SHIM_HAS_RETAIN_V2")
    assert inner
    for lo, hi in inner:
        assert any(olo < lo and hi < ohi for olo, ohi in outer), (lo, hi)


def test_the_iec_retain_include_is_gated():
    """Belt and braces for a header set that lacks the file entirely."""
    source = _SHIM.read_text(encoding="utf-8")
    spans = _gated_regions(source)

    for lineno, line in enumerate(source.splitlines(), start=1):
        if not re.match(r'\s*#\s*include\s+"iec_retain\.hpp"', line):
            continue
        assert any(lo <= lineno <= hi for lo, hi in spans), (
            f"{_SHIM.name}:{lineno} includes iec_retain.hpp unconditionally"
        )


# ---------------------------------------------------------------------------
# 3. The probe covers what the shim uses
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("name", _RETAIN_NAMES)
def test_the_probe_exercises_every_name_the_shim_needs(name):
    """Keeps the two files honest about each other.

    A probe that touches fewer names than the shim calls reports "capable" for a
    header set the shim cannot compile against -- the build then fails with the
    original confusing error, and the gate looks innocent.
    """
    probe = _PROBE.read_text(encoding="utf-8")
    code = "\n".join(line.split("//", 1)[0] for line in probe.splitlines())

    assert name in code, f"retain_probe.cpp never references {name}"


@pytest.mark.parametrize("name", _RETAIN_V2_NAMES)
def test_the_v2_probe_section_exercises_every_v2_name_the_shim_needs(name):
    """Same honesty, for the second gate: the names must be in the probe's
    STRUCPP_RETAIN_PROBE_V2 section, which is the only part the v2 verdict adds."""
    probe = _PROBE.read_text(encoding="utf-8")
    sections = re.findall(
        r"#ifdef STRUCPP_RETAIN_PROBE_V2\n(.*?)#endif", probe, flags=re.DOTALL
    )
    code = "\n".join(
        line.split("//", 1)[0] for section in sections for line in section.splitlines()
    )

    assert name in code, f"retain_probe.cpp's v2 section never references {name}"


def test_the_makefile_only_defines_the_gate_from_the_probe():
    """The wiring: verdict -> define -> the shim's own flag set.

    Checked as text because the alternative is running `make` against a
    generated tree that does not exist outside a real upload.
    """
    makefile = _MAKEFILE.read_text(encoding="utf-8")

    assert "-fsyntax-only $(RETAIN_PROBE)" in makefile
    assert "SHIM_HAS_RETAIN :=" in makefile
    assert "SHIM_HAS_RETAIN_V2 :=" in makefile
    # The v2 verdict is the same probe, second pass, and only after v1 passed.
    assert "-DSTRUCPP_RETAIN_PROBE_V2" in makefile
    assert "SHIM_HAS_RETAIN_V2 := $(strip $(if $(SHIM_HAS_RETAIN)," in makefile
    # Each define must be reachable ONLY through its own probe verdict.
    for line in makefile.splitlines():
        for define, verdict in (
            ("-DSTRUCPP_SHIM_HAS_RETAIN_V2", "$(if $(SHIM_HAS_RETAIN_V2),"),
            ("-DSTRUCPP_SHIM_HAS_RETAIN,", "$(if $(SHIM_HAS_RETAIN),"),
        ):
            if define in line:
                assert verdict in line, line
    # ...and the shim recipe must use the gated flags, not the common ones.
    assert "$(CXX) $(SHIM_CXXFLAGS) -c $< -o $@" in makefile


# ---------------------------------------------------------------------------
# 4. The wiring: probe verdict -> compiler flag
# ---------------------------------------------------------------------------
#
# Everything above can pass while the build still does the wrong thing. It did:
# the first cut of the Makefile wrote the verdict with a line continuation, and
# GNU make counts the lone space that leaves behind as a NON-EMPTY $(if)
# condition -- so a legacy header set produced " " instead of "", the gate turned
# on for exactly the uploads it exists to protect, and the original error came
# straight back. Only asking make itself catches that class of bug.

_MAKE = shutil.which("make") or shutil.which("gmake")


def _shim_recipe(generated_dir: Path, build_dir: Path) -> str:
    """What make WOULD run to compile the shim, without running it.

    GENERATED_DIR and BUILD_DIR are both overridden: the first points the probe
    at the stub header set, the second keeps a stale object in the developer's
    real build/ from making the target look up to date (which would print
    nothing to assert on).
    """
    proc = subprocess.run(
        [
            _MAKE,
            "-f",
            "scripts/Makefile.strucpp",
            f"GENERATED_DIR={generated_dir}",
            f"BUILD_DIR={build_dir}",
            "-n",
            str(build_dir / "runtime_v4_entry.o"),
        ],
        cwd=_REPO,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def _generated_dir(tmp_path: Path, *, capable: bool, v2: bool = False) -> Path:
    """A stub `core/generated` with the two prerequisites the shim target needs."""
    generated = tmp_path / "generated"
    generated.mkdir()
    _header_set(generated, capable=capable, v2=v2)
    (generated / "generated.hpp").write_text("#pragma once\n", encoding="utf-8")
    (generated / "defines.h").write_text(
        '#pragma once\n#define PROGRAM_MD5 "x"\n', encoding="utf-8"
    )
    return generated


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_make_omits_the_define_for_a_legacy_header_set(tmp_path):
    """The regression that shipped, and then very nearly shipped twice."""
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=False), tmp_path / "build")

    assert "runtime_v4_entry.cpp" in recipe
    assert "-DSTRUCPP_SHIM_HAS_RETAIN" not in recipe


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_make_adds_the_define_for_a_capable_header_set(tmp_path):
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=True), tmp_path / "build")

    assert "-DSTRUCPP_SHIM_HAS_RETAIN" in recipe


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_the_legacy_path_says_so_in_the_build_log(tmp_path):
    """The user is watching this log in the editor's console.

    A program whose RETAIN variables silently do nothing is worse than one that
    fails, so the build has to name the reason and the version that fixes it.
    """
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=False), tmp_path / "build")

    assert "RETAIN:" in recipe
    assert "v0.6.5" in recipe, "the notice must name the STruC++ version that adds support"
    assert "v4.2.12" in recipe, "the notice must name the editor version that adds support"


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_make_adds_only_the_v1_define_for_a_format_1_header_set(tmp_path):
    """Every editor released before format 2: keeps format 1, never fails."""
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=True), tmp_path / "build")

    assert "-DSTRUCPP_SHIM_HAS_RETAIN " in recipe or "-DSTRUCPP_SHIM_HAS_RETAIN\n" in recipe
    assert "-DSTRUCPP_SHIM_HAS_RETAIN_V2" not in recipe


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_make_adds_both_defines_for_a_format_2_header_set(tmp_path):
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=True, v2=True), tmp_path / "build")

    assert "-DSTRUCPP_SHIM_HAS_RETAIN " in recipe
    assert "-DSTRUCPP_SHIM_HAS_RETAIN_V2" in recipe


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_make_omits_the_v2_define_for_a_legacy_header_set(tmp_path):
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=False), tmp_path / "build")

    assert "-DSTRUCPP_SHIM_HAS_RETAIN_V2" not in recipe


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_the_format_1_path_names_what_it_lacks(tmp_path):
    """Retain works, but a changed layout resets it -- the user should know why."""
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=True), tmp_path / "build")

    assert "RETAIN:" in recipe
    assert "migration by" in recipe
    assert "v0.6.5" not in recipe, "the pre-retain notice is for legacy header sets only"


@pytest.mark.skipif(_MAKE is None or _CXX is None, reason="needs make and a C++ compiler")
def test_the_capable_path_stays_quiet(tmp_path):
    recipe = _shim_recipe(_generated_dir(tmp_path, capable=True, v2=True), tmp_path / "build")

    assert "RETAIN:" not in recipe
