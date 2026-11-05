#!/usr/bin/env python3
"""Übernimmt die NIS-Koeffiziententabellen (coef_scale, coef_usm) aus NIS_Config.h
von NVIDIA Image Scaling (MIT-Lizenz) in src/nis_coef.h.

Aufruf:  python3 -I tools/extract_nis_coefs.py <Pfad/zu/NIS_Config.h> src/nis_coef.h

Das Skript parst nur Text und führt keinen fremden Code aus.
"""
import re
import sys

PHASES = 64
FILTER = 8


def extract(text, name):
    m = re.search(r"constexpr\s+float\s+%s\s*\[kPhaseCount\]\[kFilterSize\]\s*=\s*\{(.*?)\n\s*\};" % name,
                  text, re.S)
    if not m:
        raise SystemExit("Tabelle %s nicht gefunden" % name)
    rows = re.findall(r"\{([^{}]*)\}", m.group(1))
    table = []
    for row in rows:
        vals = [float(v.strip().rstrip("fF")) for v in row.split(",") if v.strip()]
        if len(vals) != FILTER:
            raise SystemExit("%s: Zeile mit %d statt %d Werten" % (name, len(vals), FILTER))
        table.append(vals)
    if len(table) != PHASES:
        raise SystemExit("%s: %d statt %d Zeilen" % (name, len(table), PHASES))
    return table


def emit(name, table):
    out = ["static const float %s[%d][%d] = {" % (name, PHASES, FILTER)]
    for row in table:
        out.append("\t{" + ", ".join("%.6ff" % v for v in row) + "},")
    out.append("};")
    return "\n".join(out)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        text = f.read()
    scale = extract(text, "coef_scale")
    usm = extract(text, "coef_usm")
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write("/* Koeffiziententabellen von NVIDIA Image Scaling (NIS_Config.h).\n"
                " * MIT-Lizenz, Copyright (c) 2022 NVIDIA Corporation & Affiliates.\n"
                " * Erzeugt mit tools/extract_nis_coefs.py, nicht von Hand ändern. */\n\n")
        f.write("#pragma once\n\n")
        f.write(emit("nis_coef_scale", scale) + "\n\n")
        f.write(emit("nis_coef_usm", usm) + "\n")
    print("OK: %d Phasen x %d Werte je Tabelle -> %s" % (PHASES, FILTER, sys.argv[2]))


if __name__ == "__main__":
    main()
