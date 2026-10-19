#!/usr/bin/env python3
"""
gen_profiles.py - Generate the built-in profile tables from Geriatrix.

Input : <geriatrix>/profiles/<name>/{age,size,dir}_distribution.txt
Output: FS_AGING_CORE/src/fs_aging_profiles.c (default)

Usage (from the LIB_FS_AGING folder):
    py -3 TOOLS/gen_profiles.py [--geriatrix <dir>] [--out <file>]
                                [--mount /fs0]

The output is deterministic. Any malformed input line stops the script
with an error. Uses only the Python standard library.

Original: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
"""

import argparse
import math
import os
import re
import sys

# Profile order. Must match fs_aging_profiles.h.
PROFILES = (
    "agrawal",
    "dabre",
    "douceur",
    "grundman",
    "meyer",
    "pramod",
    "wang_lanl",
    "wang_os",
)

# Known extra rows in the original files: (profile, file) -> count.
# They are ignored, as Geriatrix does. Any other mismatch is an error.
KNOWN_EXTRA_ROWS = {
    ("grundman", "size_distribution.txt"): 1,
}

MAX_SIBLINGS = 0xFFFF
U32_MAX = 0xFFFFFFFF
U64_MAX = 0xFFFFFFFFFFFFFFFF

RE_UINT = re.compile(r"^[0-9]+$")
RE_FLOAT = re.compile(r"^[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?$")
RE_IDENT = re.compile(r"^[a-z_][a-z0-9_]*$")
RE_DEFINE = r"^\s*#\s*define\s+{name}\s+([0-9]+)u?\s*$"


class ProfileError(Exception):
    """Input error with file and line information."""


# ---------------------------------------------------------------- reading

def parse_uint(tok, where, what, limit):
    if not RE_UINT.match(tok):
        raise ProfileError("%s: %s is not an unsigned integer: '%s'" % (where, what, tok))
    val = int(tok, 10)
    if val > limit:
        raise ProfileError("%s: %s too large (%d > %d)" % (where, what, val, limit))
    return val


def parse_float(tok, where, what):
    """Return (value, C text). Integer-looking text gets '.0'."""
    if not RE_FLOAT.match(tok):
        raise ProfileError("%s: %s is not a number: '%s'" % (where, what, tok))
    val = float(tok)
    if not math.isfinite(val):
        raise ProfileError("%s: %s is not finite: '%s'" % (where, what, tok))
    text = tok[1:] if tok.startswith("+") else tok
    if ("." not in text) and ("e" not in text) and ("E" not in text):
        text = text + ".0"
    return val, text


def read_table(path, ncols, allowed_extra=0, warnings=None):
    """Read one distribution file and return its data rows."""
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise ProfileError("%s: cannot read (%s)" % (path, exc))
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ProfileError("%s: non-ASCII character (%s)" % (path, exc))

    lines = [ln.rstrip("\r") for ln in text.split("\n")]
    if (len(lines) == 0) or (lines[0].strip() == ""):
        raise ProfileError("%s:1: row count expected" % path)
    count_tok = lines[0].split()
    if len(count_tok) != 1:
        raise ProfileError("%s:1: a single row count expected: '%s'"
                           % (path, lines[0]))
    count = parse_uint(count_tok[0], "%s:1" % path, "row count", U32_MAX)
    if count == 0:
        raise ProfileError("%s:1: row count must not be 0" % path)

    rows = []
    for i in range(count):
        lineno = i + 2
        if (i + 1) >= len(lines):
            raise ProfileError("%s: expected %d data rows, found %d"
                               % (path, count, i))
        toks = lines[i + 1].split()
        if len(toks) == 0 or toks[0].startswith("Format:"):
            raise ProfileError("%s:%d: expected %d data rows, found %d"
                               % (path, lineno, count, i))
        if len(toks) != ncols:
            raise ProfileError("%s:%d: expected %d fields, found %d: '%s'"
                               % (path, lineno, ncols, len(toks), lines[i + 1]))
        rows.append((lineno, toks))

    # Known extra rows: check and skip them.
    j = count + 1
    extra = []
    while (j < len(lines)) and (len(extra) < allowed_extra):
        toks = lines[j].split()
        if (len(toks) != ncols) or toks[0].startswith("Format:"):
            break
        extra.append(lines[j].strip())
        j += 1
    if len(extra) != allowed_extra:
        raise ProfileError("%s: expected %d extra rows, found %d"
                           % (path, allowed_extra, len(extra)))
    if (allowed_extra > 0) and (warnings is not None):
        warnings.append("%s: header says %d, %d extra row(s) ignored "
                        "as in the original: %s"
                        % (path, count, allowed_extra, " | ".join(extra)))

    # Only blank lines and the "Format:" block may follow.
    for k in range(j, len(lines)):
        stripped = lines[k].strip()
        if stripped == "":
            continue
        if stripped.startswith("Format:"):
            break
        raise ProfileError("%s:%d: data does not match row count (%d), "
                           "extra line: '%s'" % (path, k + 1, count, lines[k]))
    return rows


def check_weights(path, weights):
    if not any(w > 0.0 for w in weights):
        raise ProfileError("%s: all weights are zero" % path)


def load_ages(path, rows):
    out = []
    prev = None
    for lineno, toks in rows:
        where = "%s:%d" % (path, lineno)
        cut, cut_txt = parse_float(toks[0], where, "age limit")
        wgt, wgt_txt = parse_float(toks[1], where, "weight")
        if not cut > 0.0:
            raise ProfileError("%s: age limit must be positive: %s" % (where, toks[0]))
        if (prev is not None) and not (cut > prev):
            raise ProfileError("%s: age limits must be strictly increasing (%s <= %r)"
                               % (where, toks[0], prev))
        if wgt < 0.0:
            raise ProfileError("%s: negative weight: %s" % (where, toks[1]))
        prev = cut
        out.append({"cut": cut_txt, "w": wgt, "w_txt": wgt_txt})
    check_weights(path, [r["w"] for r in out])
    return out


def load_sizes(path, rows):
    out = []
    for lineno, toks in rows:
        where = "%s:%d" % (path, lineno)
        size = parse_uint(toks[0], where, "size", U64_MAX)
        wgt, wgt_txt = parse_float(toks[1], where, "weight")
        if wgt < 0.0:
            raise ProfileError("%s: negative weight: %s" % (where, toks[1]))
        out.append({"size": size, "w": wgt, "w_txt": wgt_txt})
    check_weights(path, [r["w"] for r in out])
    return out


def load_dirs(path, rows, path_max):
    out = []
    for lineno, toks in rows:
        where = "%s:%d" % (path, lineno)
        depth = parse_uint(toks[0], where, "depth", U32_MAX)
        # A depth above FS_AGING_PATH_MAX can never fit in a path.
        if depth > path_max:
            raise ProfileError("%s: depth %d cannot fit in FS_AGING_PATH_MAX "
                               "(%d)" % (where, depth, path_max))
        wgt, wgt_txt = parse_float(toks[1], where, "weight")
        sib = parse_uint(toks[2], where, "sibling dirs", MAX_SIBLINGS)
        if wgt < 0.0:
            raise ProfileError("%s: negative weight: %s" % (where, toks[1]))
        out.append({"depth": depth, "w": wgt, "w_txt": wgt_txt, "sib": sib})
    check_weights(path, [r["w"] for r in out])
    return out


def load_profile(root, name, warnings, path_max):
    pdir = os.path.join(root, "profiles", name)

    def table(fname, ncols, loader):
        path = os.path.join(pdir, fname)
        extra = KNOWN_EXTRA_ROWS.get((name, fname), 0)
        return loader(path, read_table(path, ncols, extra, warnings))

    prof = {
        "name": name,
        "ages": table("age_distribution.txt", 2, load_ages),
        "sizes": table("size_distribution.txt", 2, load_sizes),
        "dirs": table("dir_distribution.txt", 3,
                      lambda path, rows: load_dirs(path, rows, path_max)),
    }
    if len(prof["ages"]) < 2:
        raise ProfileError("%s: at least 2 age rows required" % name)
    return prof


# ---------------------------------------------------------------- checks

def max_path_len(prof, mount):
    """Longest path for this profile, including '\\0' (same as fs_aging.c)."""
    best = 0
    for d in prof["dirs"]:
        depth = d["depth"]
        length = len(mount)
        if depth > 0:
            leaf = d["sib"] if d["sib"] != 0 else depth
            for lvl in range(1, depth):
                length += 2 + len(str(lvl))      # "/d<lvl>"
            length += 2 + len(str(leaf))         # "/d<leaf>"
        length += 2 + 20 + 1                     # "/f<tick>\0"
        if length > best:
            best = length
    return best


def read_header_define(header, name):
    try:
        with open(header, "r", encoding="ascii") as fh:
            text = fh.read()
    except OSError as exc:
        raise ProfileError("%s: cannot read (%s)" % (header, exc))
    m = re.search(RE_DEFINE.format(name=name), text, re.MULTILINE)
    if m is None:
        raise ProfileError("%s: #define %s not found" % (header, name))
    return int(m.group(1), 10)


# ---------------------------------------------------------------- output

def emit(profiles):
    o = []
    o.append("/*")
    o.append(" * fs_aging_profiles.c - Built-in Geriatrix aging profiles.")
    o.append(" *")
    o.append(" * AUTO-GENERATED by TOOLS/gen_profiles.py - DO NOT EDIT BY HAND.")
    o.append(" *")
    o.append(" * Source: geriatrix/profiles/<name>/age_distribution.txt,")
    o.append(" *         size_distribution.txt, dir_distribution.txt")
    o.append(" * To regenerate (from the LIB_FS_AGING folder):")
    o.append(" *   py -3 TOOLS/gen_profiles.py")
    o.append(" *")
    o.append(" * Profile data comes from the original Geriatrix repository.")
    o.append(" * Original: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)")
    o.append(" *")
    o.append(" * This file does not use libc.")
    o.append(" */")
    o.append("")
    o.append('#include "fs_aging_profiles.h"')
    o.append("")

    for p in profiles:
        n = p["name"]
        o.append("/* " + "-" * 66 + " */")
        o.append("/* %-66s */" % ("%s (ages %d, sizes %d, dirs %d)"
                                   % (n, len(p["ages"]), len(p["sizes"]),
                                      len(p["dirs"]))))
        for (qname, qfile), qextra in sorted(KNOWN_EXTRA_ROWS.items()):
            if qname == n:
                o.append("/* %-66s */" % ("Note: %s has %d extra row(s); ignored,"
                                          % (qfile, qextra)))
                o.append("/* %-66s */" % "as in the original.")
        o.append("/* " + "-" * 66 + " */")
        o.append("")
        o.append("static const fs_aging_age_bin_t %s_ages[%du] = {" % (n, len(p["ages"])))
        for r in p["ages"]:
            o.append("    { %s, %s }," % (r["cut"], r["w_txt"]))
        o.append("};")
        o.append("")
        o.append("static const fs_aging_size_bin_t %s_sizes[%du] = {" % (n, len(p["sizes"])))
        for r in p["sizes"]:
            o.append("    { %du, %s }," % (r["size"], r["w_txt"]))
        o.append("};")
        o.append("")
        o.append("static const fs_aging_dir_bin_t %s_dirs[%du] = {" % (n, len(p["dirs"])))
        for r in p["dirs"]:
            o.append("    { %du, %s, %du }," % (r["depth"], r["w_txt"], r["sib"]))
        o.append("};")
        o.append("")
        o.append("const fs_aging_profile_t fs_aging_profile_%s = {" % n)
        o.append('    "%s",' % n)
        o.append("    %s_ages,  %du," % (n, len(p["ages"])))
        o.append("    %s_sizes, %du," % (n, len(p["sizes"])))
        o.append("    %s_dirs,  %du" % (n, len(p["dirs"])))
        o.append("};")
        o.append("")

    o.append("/* " + "-" * 66 + " */")
    o.append("/* %-66s */" % "Profile list and lookup")
    o.append("/* " + "-" * 66 + " */")
    o.append("")
    o.append("const fs_aging_profile_t *const fs_aging_profiles[FS_AGING_PROFILE_COUNT] = {")
    for i, p in enumerate(profiles):
        sep = "," if (i + 1) < len(profiles) else ""
        o.append("    &fs_aging_profile_%s%s" % (p["name"], sep))
    o.append("};")
    o.append("")
    o.append("/* Return 1 if the two strings are equal (strcmp replacement). */")
    o.append("static int str_equal(const char *a, const char *b)")
    o.append("{")
    o.append("    while ((*a != '\\0') && (*a == *b)) {")
    o.append("        a++;")
    o.append("        b++;")
    o.append("    }")
    o.append("    return (*a == *b) ? 1 : 0;")
    o.append("}")
    o.append("")
    o.append("const fs_aging_profile_t *fs_aging_profile_find(const char *name)")
    o.append("{")
    o.append("    uint32_t i;")
    o.append("")
    o.append("    if (name == NULL) {")
    o.append("        return NULL;")
    o.append("    }")
    o.append("    for (i = 0u; i < FS_AGING_PROFILE_COUNT; i++) {")
    o.append("        if (str_equal(fs_aging_profiles[i]->name, name) != 0) {")
    o.append("            return fs_aging_profiles[i];")
    o.append("        }")
    o.append("    }")
    o.append("    return NULL;")
    o.append("}")
    return "\n".join(o) + "\n"


def find_geriatrix(repo):
    for cand in (os.path.join(repo, "..", "geriatrix"), os.path.join(repo, "geriatrix")):
        if os.path.isdir(os.path.join(cand, "profiles")):
            return os.path.normpath(cand)
    raise ProfileError("geriatrix folder not found (%s/../geriatrix or "
                       "%s/geriatrix); use --geriatrix" % (repo, repo))


def main(argv):
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Generate "
                                 "FS_AGING_CORE/src/fs_aging_profiles.c from Geriatrix profiles.")
    ap.add_argument("--geriatrix", help="Geriatrix clone (default: "
                    "<repo>/../geriatrix, then <repo>/geriatrix)")
    ap.add_argument("--out", default=os.path.join(repo, "FS_AGING_CORE", "src",
                                                  "fs_aging_profiles.c"),
                    help="output file (default: FS_AGING_CORE/src/fs_aging_profiles.c)")
    ap.add_argument("--mount", default="/fs0",
                    help="mount point for the path length check (default: /fs0)")
    args = ap.parse_args(argv)

    try:
        root = args.geriatrix if args.geriatrix else find_geriatrix(repo)
        header = os.path.join(repo, "FS_AGING_CORE", "include", "fs_aging.h")
        max_bins = read_header_define(header, "FS_AGING_MAX_BINS")
        path_max = read_header_define(header, "FS_AGING_PATH_MAX")

        profiles = []
        warnings = []
        for name in PROFILES:
            if not RE_IDENT.match(name):
                raise ProfileError("%s: invalid profile name" % name)
            profiles.append(load_profile(root, name, warnings, path_max))

        print("geriatrix : %s" % os.path.abspath(root))
        print("mount     : %s" % args.mount)
        for w in warnings:
            print("WARNING   : %s" % w)
        worst_len, worst_name = -1, ""
        worst_bins, worst_bins_name = -1, ""
        for p in profiles:
            plen = max_path_len(p, args.mount)
            rows = max(len(p["ages"]), len(p["sizes"]), len(p["dirs"]))
            if rows > max_bins:
                raise ProfileError("%s: %d rows exceed FS_AGING_MAX_BINS (%d)"
                                   % (p["name"], rows, max_bins))
            print("%-10s ages=%2d sizes=%2d dirs=%2d  longest_path=%3d"
                  % (p["name"], len(p["ages"]), len(p["sizes"]),
                     len(p["dirs"]), plen))
            if plen > worst_len:
                worst_len, worst_name = plen, p["name"]
            if rows > worst_bins:
                worst_bins, worst_bins_name = rows, p["name"]
        print("longest path   : %d bytes (with '\\0'), profile %s, mount %s "
              "(FS_AGING_PATH_MAX=%d: %s)"
              % (worst_len, worst_name, args.mount, path_max,
                 "fits" if worst_len <= path_max else "TOO LONG"))
        print("FS_AGING_MAX_BINS: %d, most rows %d (%s): all profiles fit"
              % (max_bins, worst_bins, worst_bins_name))

        text = emit(profiles)
        try:
            with open(args.out, "w", encoding="ascii", newline="\n") as fh:
                fh.write(text)
        except OSError as exc:
            raise ProfileError("%s: cannot write (%s)" % (args.out, exc))
        print("written   : %s" % os.path.abspath(args.out))
    except ProfileError as exc:
        sys.stderr.write("gen_profiles: ERROR: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
