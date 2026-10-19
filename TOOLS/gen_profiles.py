#!/usr/bin/env python3
"""
gen_profiles.py - Geriatrix profil dosyalarindan C tablosu uretir.

Girdi : <geriatrix>/profiles/<ad>/{age,size,dir}_distribution.txt
Cikti : CORE/src/fs_aging_profiles.c (varsayilan)

Dosya formati (her uc dosya icin):
    <N>                      ilk satir: veri satiri sayisi
    N adet veri satiri       age : "<yas_esigi> <agirlik>"
                             size: "<boyut_byte> <agirlik>"
                             dir : "<derinlik> <agirlik> <kardes_dizin>"
    (bos satir)
    Format: ...              aciklama blogu, yok sayilir

Girdi kati kurallarla okunur; hatali bir satirda script sifirdan farkli
kodla ve acik bir mesajla cikar. Cikti deterministiktir: ayni girdi her
zaman bayt bayt ayni dosyayi uretir (sabit profil sirasi, zaman damgasi
yok, isletim sisteminden bagimsiz '\\n' satir sonu, ASCII).

Kullanim (LIB_FS_AGING klasorunde):
    py -3 TOOLS/gen_profiles.py [--geriatrix <dizin>] [--out <dosya>]
                                [--mount /fs0]

Orijinal: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
Yalnizca Python standart kutuphanesi kullanilir.
"""

import argparse
import math
import os
import re
import sys

# Sabit profil sirasi (alfabetik). fs_aging_profiles.h'deki
# FS_AGING_PROFILE_COUNT ve fs_aging_profiles[] sirasi ile ayni olmali.
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

# Orijinal profil dosyalarindaki bilinen tutarsizliklar: (profil, dosya) ->
# ilk satirdaki sayidan sonra gelen fazla veri satiri adedi. Orijinal
# Geriatrix (geriatrix.cpp, "infile >> count" dongusu) yalnizca ilk
# <count> satiri okur ve fazlasini sessizce yok sayar; burada da ayni
# davranis uygulanir ama uyari basilir. Listede olmayan her uyusmazlik
# ve buradaki adetten farkli her fazlalik hata sayilir.
KNOWN_EXTRA_ROWS = {
    ("grundman", "size_distribution.txt"): 1,   # basta 22 yazar, 23 satir var
}

MAX_SIBLINGS = 0xFFFF
U32_MAX = 0xFFFFFFFF
U64_MAX = 0xFFFFFFFFFFFFFFFF

RE_UINT = re.compile(r"^[0-9]+$")
RE_FLOAT = re.compile(r"^[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?$")
RE_IDENT = re.compile(r"^[a-z_][a-z0-9_]*$")
RE_DEFINE = r"^\s*#\s*define\s+{name}\s+([0-9]+)u?\s*$"


class ProfileError(Exception):
    """Girdi hatasi: mesaj dosya ve satir bilgisini icerir."""


# ---------------------------------------------------------------- okuma

def parse_uint(tok, where, what, limit):
    if not RE_UINT.match(tok):
        raise ProfileError("%s: %s isaretsiz tamsayi degil: '%s'" % (where, what, tok))
    val = int(tok, 10)
    if val > limit:
        raise ProfileError("%s: %s cok buyuk (%d > %d)" % (where, what, val, limit))
    return val


def parse_float(tok, where, what):
    """(deger, C_metni) doner. Metin girdiye sadik kalir; tamsayi
    gorunumlu degerlere '.0' eklenir (31 -> 31.0)."""
    if not RE_FLOAT.match(tok):
        raise ProfileError("%s: %s sayi degil: '%s'" % (where, what, tok))
    val = float(tok)
    if not math.isfinite(val):
        raise ProfileError("%s: %s sonlu degil: '%s'" % (where, what, tok))
    text = tok[1:] if tok.startswith("+") else tok
    if ("." not in text) and ("e" not in text) and ("E" not in text):
        text = text + ".0"
    return val, text


def read_table(path, ncols, allowed_extra=0, warnings=None):
    """Dosyayi okur, her veri satiri icin token listesi ve satir no doner.
    allowed_extra > 0 ise sayidan sonra tam olarak o kadar fazla veri
    satiri kabul edilir ve (orijinaldeki gibi) yok sayilir."""
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise ProfileError("%s: okunamadi (%s)" % (path, exc))
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ProfileError("%s: ASCII olmayan karakter (%s)" % (path, exc))

    lines = [ln.rstrip("\r") for ln in text.split("\n")]
    if (len(lines) == 0) or (lines[0].strip() == ""):
        raise ProfileError("%s:1: satir sayisi bekleniyordu" % path)
    count_tok = lines[0].split()
    if len(count_tok) != 1:
        raise ProfileError("%s:1: tek bir satir sayisi bekleniyordu: '%s'"
                           % (path, lines[0]))
    count = parse_uint(count_tok[0], "%s:1" % path, "satir sayisi", U32_MAX)
    if count == 0:
        raise ProfileError("%s:1: satir sayisi 0 olamaz" % path)

    rows = []
    for i in range(count):
        lineno = i + 2
        if (i + 1) >= len(lines):
            raise ProfileError("%s: %d veri satiri bekleniyordu, %d bulundu"
                               % (path, count, i))
        toks = lines[i + 1].split()
        if len(toks) == 0 or toks[0].startswith("Format:"):
            raise ProfileError("%s:%d: %d veri satiri bekleniyordu, %d bulundu"
                               % (path, lineno, count, i))
        if len(toks) != ncols:
            raise ProfileError("%s:%d: %d alan bekleniyordu, %d bulundu: '%s'"
                               % (path, lineno, ncols, len(toks), lines[i + 1]))
        rows.append((lineno, toks))

    # Bilinen tutarsizlik: veri satirlarinin hemen ardindan gelen fazla
    # satirlar ayni alan sayisiyla dogrulanir ve atlanir.
    j = count + 1
    extra = []
    while (j < len(lines)) and (len(extra) < allowed_extra):
        toks = lines[j].split()
        if (len(toks) != ncols) or toks[0].startswith("Format:"):
            break
        extra.append(lines[j].strip())
        j += 1
    if len(extra) != allowed_extra:
        raise ProfileError("%s: %d fazla veri satiri bekleniyordu, %d bulundu"
                           % (path, allowed_extra, len(extra)))
    if (allowed_extra > 0) and (warnings is not None):
        warnings.append("%s: ilk satir %d diyor, %d fazla satir orijinaldeki "
                        "gibi yok sayildi: %s"
                        % (path, count, allowed_extra, " | ".join(extra)))

    # Veri satirlarindan sonra yalnizca bos satirlar ve (varsa) "Format:"
    # ile baslayan aciklama blogu gelebilir.
    for k in range(j, len(lines)):
        stripped = lines[k].strip()
        if stripped == "":
            continue
        if stripped.startswith("Format:"):
            break
        raise ProfileError("%s:%d: satir sayisi (%d) ile veri uyusmuyor, "
                           "fazla satir: '%s'" % (path, k + 1, count, lines[k]))
    return rows


def check_weights(path, weights):
    if not any(w > 0.0 for w in weights):
        raise ProfileError("%s: tum agirliklar sifir" % path)


def load_ages(path, rows):
    out = []
    prev = None
    for lineno, toks in rows:
        where = "%s:%d" % (path, lineno)
        cut, cut_txt = parse_float(toks[0], where, "yas esigi")
        wgt, wgt_txt = parse_float(toks[1], where, "agirlik")
        if not cut > 0.0:
            raise ProfileError("%s: yas esigi pozitif olmali: %s" % (where, toks[0]))
        if (prev is not None) and not (cut > prev):
            raise ProfileError("%s: yas esikleri kesin artan olmali (%s <= %r)"
                               % (where, toks[0], prev))
        if wgt < 0.0:
            raise ProfileError("%s: agirlik negatif: %s" % (where, toks[1]))
        prev = cut
        out.append({"cut": cut_txt, "w": wgt, "w_txt": wgt_txt})
    check_weights(path, [r["w"] for r in out])
    return out


def load_sizes(path, rows):
    out = []
    for lineno, toks in rows:
        where = "%s:%d" % (path, lineno)
        size = parse_uint(toks[0], where, "boyut", U64_MAX)
        wgt, wgt_txt = parse_float(toks[1], where, "agirlik")
        if wgt < 0.0:
            raise ProfileError("%s: agirlik negatif: %s" % (where, toks[1]))
        out.append({"size": size, "w": wgt, "w_txt": wgt_txt})
    check_weights(path, [r["w"] for r in out])
    return out


def load_dirs(path, rows, path_max):
    out = []
    for lineno, toks in rows:
        where = "%s:%d" % (path, lineno)
        depth = parse_uint(toks[0], where, "derinlik", U32_MAX)
        # Her seviye en az 3 byte ("/d<n>") ekler: path_max'tan derin bir
        # satir hicbir yola sigamaz. Erken reddedilir; yol hesabi da boylece
        # sinirli kalir (dev bir derinlikte script takilmaz).
        if depth > path_max:
            raise ProfileError("%s: derinlik %d, FS_AGING_PATH_MAX (%d) ile "
                               "hicbir yola sigamaz" % (where, depth, path_max))
        wgt, wgt_txt = parse_float(toks[1], where, "agirlik")
        sib = parse_uint(toks[2], where, "kardes dizin", MAX_SIBLINGS)
        if wgt < 0.0:
            raise ProfileError("%s: agirlik negatif: %s" % (where, toks[1]))
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
        raise ProfileError("%s: en az 2 yas satiri gerekir" % name)
    return prof


# ---------------------------------------------------------------- hesap

def max_path_len(prof, mount):
    """fs_aging.c icindeki max_path_len() ile ayni formul ('\\0' dahil)."""
    best = 0
    for d in prof["dirs"]:
        depth = d["depth"]
        length = len(mount)
        if depth > 0:
            leaf = d["sib"] if d["sib"] != 0 else depth
            for lvl in range(1, depth):
                length += 2 + len(str(lvl))      # "/d<lvl>"
            length += 2 + len(str(leaf))         # "/d<yaprak>"
        length += 2 + 20 + 1                     # "/f<tick>\0"
        if length > best:
            best = length
    return best


def read_header_define(header, name):
    try:
        with open(header, "r", encoding="ascii") as fh:
            text = fh.read()
    except OSError as exc:
        raise ProfileError("%s: okunamadi (%s)" % (header, exc))
    m = re.search(RE_DEFINE.format(name=name), text, re.MULTILINE)
    if m is None:
        raise ProfileError("%s: #define %s bulunamadi" % (header, name))
    return int(m.group(1), 10)


# ---------------------------------------------------------------- uretim

def emit(profiles):
    o = []
    o.append("/*")
    o.append(" * fs_aging_profiles.c - Hazir Geriatrix aging profilleri")
    o.append(" *")
    o.append(" * OTOMATIK URETILDI - ELLE DUZENLEMEYIN.")
    o.append(" * Uretici: TOOLS/gen_profiles.py")
    o.append(" * Kaynak : geriatrix/profiles/<ad>/age_distribution.txt,")
    o.append(" *          size_distribution.txt, dir_distribution.txt")
    o.append(" * Yeniden uretmek icin (LIB_FS_AGING klasorunde):")
    o.append(" *   py -3 TOOLS/gen_profiles.py")
    o.append(" *")
    o.append(" * Profil verileri orijinal Geriatrix reposundan alinmistir.")
    o.append(" * Orijinal: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)")
    o.append(" *")
    o.append(" * Bu dosya libc kullanmaz.")
    o.append(" */")
    o.append("")
    o.append('#include "fs_aging_profiles.h"')
    o.append("")

    for p in profiles:
        n = p["name"]
        o.append("/* " + "-" * 66 + " */")
        o.append("/* %-66s */" % ("%s (yas %d, boyut %d, dizin %d satir)"
                                   % (n, len(p["ages"]), len(p["sizes"]),
                                      len(p["dirs"]))))
        for (qname, qfile), qextra in sorted(KNOWN_EXTRA_ROWS.items()):
            if qname == n:
                o.append("/* %-66s */" % ("Not: %s ilk satirdaki sayidan sonra %d fazla"
                                          % (qfile, qextra)))
                o.append("/* %-66s */" % "satir iceriyor; orijinaldeki gibi yok sayildi.")
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
    o.append("/* %-66s */" % "Profil listesi ve arama")
    o.append("/* " + "-" * 66 + " */")
    o.append("")
    o.append("const fs_aging_profile_t *const fs_aging_profiles[FS_AGING_PROFILE_COUNT] = {")
    for i, p in enumerate(profiles):
        sep = "," if (i + 1) < len(profiles) else ""
        o.append("    &fs_aging_profile_%s%s" % (p["name"], sep))
    o.append("};")
    o.append("")
    o.append("/* Iki dizgi bire bir ayni mi (strcmp yerine) */")
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
    raise ProfileError("geriatrix klasoru bulunamadi (%s/../geriatrix veya "
                       "%s/geriatrix); --geriatrix ile verin" % (repo, repo))


def main(argv):
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Geriatrix profillerinden "
                                 "CORE/src/fs_aging_profiles.c uretir.")
    ap.add_argument("--geriatrix", help="Geriatrix klonu (varsayilan: "
                    "<repo>/../geriatrix, sonra <repo>/geriatrix)")
    ap.add_argument("--out", default=os.path.join(repo, "CORE", "src",
                                                  "fs_aging_profiles.c"),
                    help="cikti dosyasi (varsayilan: CORE/src/fs_aging_profiles.c)")
    ap.add_argument("--mount", default="/fs0",
                    help="yol uzunlugu raporu icin mount point (varsayilan: /fs0)")
    args = ap.parse_args(argv)

    try:
        root = args.geriatrix if args.geriatrix else find_geriatrix(repo)
        header = os.path.join(repo, "CORE", "include", "fs_aging.h")
        max_bins = read_header_define(header, "FS_AGING_MAX_BINS")
        path_max = read_header_define(header, "FS_AGING_PATH_MAX")

        profiles = []
        warnings = []
        for name in PROFILES:
            if not RE_IDENT.match(name):
                raise ProfileError("%s: gecersiz profil adi" % name)
            profiles.append(load_profile(root, name, warnings, path_max))

        print("geriatrix : %s" % os.path.abspath(root))
        print("mount     : %s" % args.mount)
        for w in warnings:
            print("UYARI     : %s" % w)
        worst_len, worst_name = -1, ""
        worst_bins, worst_bins_name = -1, ""
        for p in profiles:
            plen = max_path_len(p, args.mount)
            rows = max(len(p["ages"]), len(p["sizes"]), len(p["dirs"]))
            if rows > max_bins:
                raise ProfileError("%s: %d satir FS_AGING_MAX_BINS (%d) sinirini asiyor"
                                   % (p["name"], rows, max_bins))
            print("%-10s yas=%2d boyut=%2d dizin=%2d  en_uzun_yol=%3d"
                  % (p["name"], len(p["ages"]), len(p["sizes"]),
                     len(p["dirs"]), plen))
            if plen > worst_len:
                worst_len, worst_name = plen, p["name"]
            if rows > worst_bins:
                worst_bins, worst_bins_name = rows, p["name"]
        print("en uzun yol    : %d byte ('\\0' dahil), profil %s, mount %s "
              "(FS_AGING_PATH_MAX=%d: %s)"
              % (worst_len, worst_name, args.mount, path_max,
                 "sigar" if worst_len <= path_max else "ASIYOR"))
        print("FS_AGING_MAX_BINS: %d, en buyuk satir sayisi %d (%s): tum profiller sigar"
              % (max_bins, worst_bins, worst_bins_name))

        text = emit(profiles)
        try:
            with open(args.out, "w", encoding="ascii", newline="\n") as fh:
                fh.write(text)
        except OSError as exc:
            raise ProfileError("%s: yazilamadi (%s)" % (args.out, exc))
        print("yazildi   : %s" % os.path.abspath(args.out))
    except ProfileError as exc:
        sys.stderr.write("gen_profiles: HATA: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
