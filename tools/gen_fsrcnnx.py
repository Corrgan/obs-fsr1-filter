#!/usr/bin/env python3
"""Erzeugt aus einem FSRCNNX-Shader von mpv den OBS-Effect und die Pass-Tabelle für das Plugin.

Aufruf:
  python3 -I tools/gen_fsrcnnx.py <FSRCNNX_x2_8-0-4-1.glsl> data/effects/fsrcnnx_8.effect src/fsrcnnx_8_passes.h fsrcnnx_8

Quelle: https://github.com/igv/FSRCNN-TensorFlow (Releases) von igv, Lizenz LGPL-3.0-or-later.
Der erzeugte Effect enthält dessen trainierte Gewichte und steht deshalb ebenfalls unter
LGPL-3.0-or-later.

FSRCNNX ist ein kleines neuronales Netz, das die Helligkeit (Luma) auf die doppelte Größe
rechnet. Das Original besteht aus mehreren Hook-Passes (Merkmale → Abbildung → Residuen →
Subpixel-Faltung, alle in Eingabegröße, dann die Zusammensetzung auf 2x). Der Generator
übersetzt jeden Pass in eine eigene Technique und schreibt dazu eine C-Tabelle, welcher
Pass welche Zwischenbilder liest und schreibt. Zwischenbilder, die nicht mehr gebraucht
werden, teilen sich einen Speicherplatz (Lebensdauer-Analyse), damit das große Netz bei
1080p nicht unnötig Grafikspeicher belegt.

Anders als in mpv gibt es in OBS kein getrenntes Luma: Die Passes berechnen Luma selbst aus
dem RGB-Bild (BT.709, im Gamma-Raum wie Video-Y'). Ein letzter Pass (OUT) setzt das Bild in
Zielgröße zusammen: Farbe bilinear aus der Quelle, Helligkeit aus dem Netz (Catmull-Rom vom
2x-Raster auf die Zielgröße). Die Farbe bleibt dabei exakt erhalten, nur Y' ändert sich.

Wie bei gen_ravu.py muss jede Zeile einem bekannten Muster entsprechen, sonst bricht der
Generator ab. Und es gelten die OBS-Regeln: kein '#define NAME (...)', keine Exponenten-
Literale, kein ternärer Operator, keine Matrizen oder Arrays (die mat4-Gewichte werden als
vier float4-Spalten ausgeschrieben: GLSL mat4 * v = Spalte0*v.x + … + Spalte3*v.w).
"""
import re
import sys

LUMA = "float3(0.2126, 0.7152, 0.0722)"  # BT.709
NUM = r"-?\d+\.\d+"
MAX_BINDS = 5


def die(msg):
    raise SystemExit("gen_fsrcnnx: " + msg)


def num(s):
    if not re.fullmatch(NUM, s):
        die(f"unerwartete Zahl {s!r} (nur Dezimalzahlen ohne Exponent)")
    return s


def nums(text, n):
    parts = [p.strip() for p in text.split(",")]
    if len(parts) != n:
        die(f"{n} Zahlen erwartet, {len(parts)} gefunden: {text[:80]}")
    return [num(p) for p in parts]


def offset(text):
    """'0' oder 'vec2(-1,2)' → (dx, dy) als Strings mit .0"""
    text = text.strip()
    if text == "0":
        return ("0.0", "0.0")
    m = re.fullmatch(r"vec2\((-?\d+),\s*(-?\d+)\)", text)
    if not m:
        die(f"unerwarteter Versatz {text!r}")
    return (m.group(1) + ".0", m.group(2) + ".0")


def parse(text):
    """Hook-Blöcke → [{desc, binds, save, size2x, body:[Zeilen]}]"""
    blocks, cur, in_body = [], None, False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("//!HOOK"):
            if line != "//!HOOK LUMA":
                die(f"nur LUMA-Hooks erwartet: {line}")
            cur = {"desc": "", "binds": [], "save": None, "size2x": False, "body": []}
            blocks.append(cur)
            in_body = False
        elif cur is None:
            continue  # Lizenzkopf
        elif line.startswith("//!DESC "):
            cur["desc"] = line[8:]
        elif line.startswith("//!BIND "):
            cur["binds"].append(line[8:])
        elif line.startswith("//!SAVE "):
            cur["save"] = line[8:]
        elif line in ("//!WIDTH LUMA.w 2 *", "//!HEIGHT LUMA.h 2 *"):
            cur["size2x"] = True
        elif line.startswith("//!COMPONENTS 4") or line.startswith("//!WHEN "):
            continue
        elif line.startswith("//!"):
            die(f"unbekannte Direktive {line}")
        elif line == "vec4 hook()":
            in_body = True
        elif in_body and line not in ("{", "}", ""):
            cur["body"].append(line)
    if not blocks:
        die("keine Hook-Blöcke gefunden")
    return blocks


def translate(block):
    """Rumpf eines Rechen-Passes (alles außer der Zusammensetzung) → HLSL-Zeilen."""
    binds = block["binds"]
    if len(binds) > MAX_BINDS:
        die(f"{block['desc']}: mehr als {MAX_BINDS} Eingaben")
    slot = {name: f"t{i}" for i, name in enumerate(binds)}
    out = []
    for line in block["body"]:
        m = re.fullmatch(r"vec4 res = vec4\((.*)\);", line)
        if m:
            out.append(f"\tfloat4 res = float4({', '.join(nums(m.group(1), 4))});")
            continue
        m = re.fullmatch(r"res \+= vec4\((.*)\) \* float\(LUMA_texOff\((.*)\)\);", line)
        if m:
            w = nums(m.group(1), 4)
            dx, dy = offset(m.group(2))
            out.append(f"\tres += float4({', '.join(w)}) * Luma(uv + float2({dx}, {dy}) * texel);")
            continue
        m = re.fullmatch(r"res \+= mat4(?:x4)?\((.*)\) \* ([A-Z0-9]+)_texOff\((.*)\);", line)
        if m:
            w = nums(m.group(1), 16)
            name = m.group(2)
            if name not in slot:
                die(f"{block['desc']}: {name} nicht gebunden")
            dx, dy = offset(m.group(3))
            out.append(f"\tt = {slot[name]}.Sample(point_clamp, uv + float2({dx}, {dy}) * texel);")
            cols = [f"float4({', '.join(w[4 * j:4 * j + 4])}) * t.{'xyzw'[j]}" for j in range(4)]
            out.append(f"\tres += {' + '.join(cols)};")
            continue
        m = re.fullmatch(r"res \+= ([A-Z0-9]+)_texOff\(0\);", line)
        if m:
            if m.group(1) not in slot:
                die(f"{block['desc']}: {m.group(1)} nicht gebunden")
            out.append(f"\tres += {slot[m.group(1)]}.Sample(point_clamp, uv);")
            continue
        m = re.fullmatch(r"res = max\(res, vec4\(0\.0\)\) \+ vec4\((.*)\) \* min\(res, vec4\(0\.0\)\);", line)
        if m:  # PReLU
            out.append(f"\tres = max(res, float4(0.0, 0.0, 0.0, 0.0)) + float4({', '.join(nums(m.group(1), 4))}) "
                       "* min(res, float4(0.0, 0.0, 0.0, 0.0));")
            continue
        if line in ("return res;", "return vec4(res);"):
            out.append("\treturn res;")
            continue
        die(f"{block['desc']}: unbekannte Zeile {line[:100]!r}")
    if not any("float4 t =" in l or "\tt = " in l for l in out):
        return out
    return ["\tfloat4 t;"] + out


def check_aggregation(block):
    """Die Zusammensetzung ist im Original Code, keine Gewichte: nur prüfen, dass es die bekannte ist."""
    want = ["vec2 fcoord = fract(SUBCONV1_pos * SUBCONV1_size);",
            "vec2 base = SUBCONV1_pos + (vec2(0.5) - fcoord) * SUBCONV1_pt;",
            "ivec2 index = ivec2(fcoord * vec2(2));",
            "vec4 res = SUBCONV1_tex(base);",
            "return vec4(res[index.x * 2 + index.y], 0, 0, 1);"]
    if block["body"] != want or block["binds"] != ["SUBCONV1"] or not block["size2x"]:
        die("Zusammensetzung (aggregation) weicht vom bekannten Original ab")


def slots(blocks):
    """Lebensdauer-Analyse: Das Original benutzt Namen mehrfach (MODEL21 wird später neu
    geschrieben), deshalb zählt jede Schreib-Version eines Namens als eigenes Bild. Ein Platz
    wird frei, sobald seine Version zum letzten Mal gelesen wurde; die Ausgabe eines Passes
    bekommt einen Platz, der schon VOR dem Pass frei war (Lesen und Schreiben desselben Bilds
    in einem Pass geht nicht). Liefert (Ausgabeplatz je Pass, Eingabeplätze je Pass, Anzahl)."""
    version, writes, reads, last_read = {}, [], [], {}
    for i, b in enumerate(blocks):
        r = []
        for name in b["binds"]:
            if name == "LUMA":
                r.append(None)
                continue
            if name not in version:
                die(f"{b['desc']}: {name} wird gelesen, bevor es geschrieben wurde")
            r.append(version[name])
            last_read[version[name]] = i
        reads.append(r)
        if b["save"]:
            v = (b["save"], i)
            version[b["save"]] = v
            writes.append(v)
        else:
            writes.append(None)
    place, free, n, out_slot, in_slots = {}, [], 0, [], []
    for i, b in enumerate(blocks):
        in_slots.append([-1 if v is None else place[v] for v in reads[i]])
        v = writes[i]
        if v is None:
            out_slot.append(None)
        else:
            if v not in last_read:
                die(f"{b['desc']}: Ergebnis wird nie gelesen")
            if free:
                place[v] = free.pop(0)
            else:
                place[v], n = n, n + 1
            out_slot.append(place[v])
        for u in {u for u in reads[i] if u is not None}:
            if last_read[u] == i:
                free.append(place[u])
        free.sort()
    return out_slot, in_slots, n


HEADER = """\
// {name}: FSRCNNX ({variant}) für OBS Studio
// GENERIERT von tools/gen_fsrcnnx.py - nicht von Hand ändern.
// Original: {source} aus https://github.com/igv/FSRCNN-TensorFlow
// Autor des Originals: igv. Lizenz: LGPL-3.0-or-later, siehe THIRD_PARTY_NOTICES.txt.
//
// Neuronales Netz für Luma auf doppelte Größe. Passes P0..P{last} rechnen in Eingabegröße
// (Zwischenbilder RGBA16F in t0..t4), AGG setzt die Subpixel zu Y' in 2x zusammen, OUT
// bringt das Bild auf die Zielgröße (Farbe bilinear aus der Quelle, Y' aus dem Netz).
// COPY legt vorher das Quellbild in ein eigenes Ziel, weil es mehrere Passes lesen.

uniform float4x4 ViewProj;
uniform texture2d image;     // Quellbild (RGBA, Gamma-Raum)
uniform texture2d t0;
uniform texture2d t1;
uniform texture2d t2;
uniform texture2d t3;
uniform texture2d t4;
uniform texture2d y2x;       // Y' in doppelter Größe (Ergebnis von AGG)
uniform float2 texel;        // 1 / Eingabegröße
uniform float2 input_size;   // Eingabegröße in Pixeln
uniform float2 y2x_size;     // doppelte Eingabegröße in Pixeln

sampler_state point_clamp {{
	Filter    = Point;
	AddressU  = Clamp;
	AddressV  = Clamp;
}};

sampler_state linear_clamp {{
	Filter    = Linear;
	AddressU  = Clamp;
	AddressV  = Clamp;
}};

struct VertData {{
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
}};

VertData VSDefault(VertData v_in)
{{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}}

float Luma(float2 p)
{{
	return dot(image.Sample(point_clamp, p).rgb, {luma});
}}
"""

TAIL = """
// Quellbild unverändert in ein eigenes Ziel kopieren (die Passes lesen es mehrmals)
float4 PSCopy(VertData v_in) : TARGET
{
	return image.Sample(point_clamp, v_in.uv);
}

// Zusammensetzung (aggregation): jeder Eingabepixel trägt 2x2 Subpixel in .xyzw
float4 PSAgg(VertData v_in) : TARGET
{
	float2 fcoord = frac(v_in.uv * input_size);
	float2 base = v_in.uv + (float2(0.5, 0.5) - fcoord) * texel;
	float4 res = t0.Sample(point_clamp, base);
	float ix = floor(fcoord.x * 2.0);
	float iy = floor(fcoord.y * 2.0);
	float idx = ix * 2.0 + iy;
	float4 pick = float4(0.0, 0.0, 0.0, 0.0);
	if (idx < 0.5) {
		pick.x = 1.0;
	} else if (idx < 1.5) {
		pick.y = 1.0;
	} else if (idx < 2.5) {
		pick.z = 1.0;
	} else {
		pick.w = 1.0;
	}
	float y = dot(res, pick);
	return float4(y, y, y, 1.0);
}

// Catmull-Rom-Gewichte für einen Abstand x in [0, 2)
float CatRom(float x)
{
	x = abs(x);
	float x2 = x * x;
	float x3 = x2 * x;
	if (x < 1.0)
		return 1.5 * x3 - 2.5 * x2 + 1.0;
	if (x < 2.0)
		return -0.5 * x3 + 2.5 * x2 - 4.0 * x + 2.0;
	return 0.0;
}

// Y' aus dem 2x-Raster an beliebiger Stelle (Catmull-Rom, 4x4 Punkte, ausgeschrieben:
// Schleifen sind im Effect-Format von OBS nicht auf allen Backends sicher)
float SampleY2x(float2 uv)
{
	float2 p = uv * y2x_size - float2(0.5, 0.5);
	float2 f = floor(p);
	float2 d = p - f;
	float wy0 = CatRom(-1.0 - d.y);
	float wy1 = CatRom(0.0 - d.y);
	float wy2 = CatRom(1.0 - d.y);
	float wy3 = CatRom(2.0 - d.y);
	float sum = 0.0;
	float wsum = 0.0;
	float w;
	w = CatRom(-1.0 - d.x) * wy0;
	sum += y2x.Sample(point_clamp, (f + float2(-0.5, -0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(0.0 - d.x) * wy0;
	sum += y2x.Sample(point_clamp, (f + float2(0.5, -0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(1.0 - d.x) * wy0;
	sum += y2x.Sample(point_clamp, (f + float2(1.5, -0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(2.0 - d.x) * wy0;
	sum += y2x.Sample(point_clamp, (f + float2(2.5, -0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(-1.0 - d.x) * wy1;
	sum += y2x.Sample(point_clamp, (f + float2(-0.5, 0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(0.0 - d.x) * wy1;
	sum += y2x.Sample(point_clamp, (f + float2(0.5, 0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(1.0 - d.x) * wy1;
	sum += y2x.Sample(point_clamp, (f + float2(1.5, 0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(2.0 - d.x) * wy1;
	sum += y2x.Sample(point_clamp, (f + float2(2.5, 0.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(-1.0 - d.x) * wy2;
	sum += y2x.Sample(point_clamp, (f + float2(-0.5, 1.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(0.0 - d.x) * wy2;
	sum += y2x.Sample(point_clamp, (f + float2(0.5, 1.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(1.0 - d.x) * wy2;
	sum += y2x.Sample(point_clamp, (f + float2(1.5, 1.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(2.0 - d.x) * wy2;
	sum += y2x.Sample(point_clamp, (f + float2(2.5, 1.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(-1.0 - d.x) * wy3;
	sum += y2x.Sample(point_clamp, (f + float2(-0.5, 2.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(0.0 - d.x) * wy3;
	sum += y2x.Sample(point_clamp, (f + float2(0.5, 2.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(1.0 - d.x) * wy3;
	sum += y2x.Sample(point_clamp, (f + float2(1.5, 2.5)) / y2x_size).r * w;
	wsum += w;
	w = CatRom(2.0 - d.x) * wy3;
	sum += y2x.Sample(point_clamp, (f + float2(2.5, 2.5)) / y2x_size).r * w;
	wsum += w;
	return sum / wsum;
}

// Zielgröße: Farbe bilinear aus der Quelle, Y' aus dem Netz. Weil die Luma-Gewichte
// zusammen 1 ergeben, ändert ein gleicher Zuschlag auf R, G und B nur Y', nicht die Farbe.
float4 PSOut(VertData v_in) : TARGET
{
	float4 c = image.Sample(linear_clamp, v_in.uv);
	float y = SampleY2x(v_in.uv);
	float d = y - dot(c.rgb, %(luma)s);
	return float4(saturate(c.rgb + float3(d, d, d)), c.a);
}
""" % {"luma": LUMA}


def technique(name, ps):
    return f"""
technique {name}
{{
	pass
	{{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = {ps}(v_in);
	}}
}}
"""


def check_obs_rules(src):
    if re.search(r"#define\s+\w+\s*\(", src):
        die("'#define NAME (' ist in OBS verboten")
    if re.search(r"\d[eE][-+]?\d", src):
        die("Exponenten-Literal im Ergebnis")
    if "?" in src:
        die("ternärer Operator im Ergebnis")
    if re.search(r"\b(mat[234]|float[234]x[234])\b", src.replace("uniform float4x4 ViewProj", "")):
        die("Matrix im Ergebnis")


def main():
    if len(sys.argv) != 5:
        die("Aufruf: gen_fsrcnnx.py <eingabe.glsl> <ausgabe.effect> <ausgabe_passes.h> <name>")
    src_path, eff_path, hdr_path, name = sys.argv[1:]
    blocks = parse(open(src_path, encoding="utf-8").read())
    agg = blocks[-1]
    check_aggregation(agg)
    passes = blocks[:-1]
    if any(b["size2x"] or not b["save"] for b in passes):
        die("nur der letzte Pass darf die Größe ändern oder nichts speichern")
    out_slot, in_slots, nslots = slots(passes + [agg])

    variant = re.search(r"FSRCNNX_x2_([\d-]+)", src_path)
    parts = [HEADER.format(name=name, variant=variant.group(1) if variant else "?", source=src_path.split("/")[-1],
                           last=len(passes) - 1, luma=LUMA)]
    for i, b in enumerate(passes):
        parts.append(f"\n// P{i}: {b['desc']} ({' '.join(b['binds'])} -> {b['save']})\n"
                     f"float4 PS{i}(VertData v_in) : TARGET\n{{\n\tfloat2 uv = v_in.uv;\n"
                     + "\n".join(translate(b)) + "\n}\n")
    parts.append(TAIL)
    for i in range(len(passes)):
        parts.append(technique(f"P{i}", f"PS{i}"))
    parts.append(technique("COPY", "PSCopy"))
    parts.append(technique("AGG", "PSAgg"))
    parts.append(technique("OUT", "PSOut"))
    effect = "".join(parts)
    check_obs_rules(effect)
    open(eff_path, "w", encoding="utf-8").write(effect)

    guard = name.upper() + "_PASSES_H"
    rows = []
    for i, b in enumerate(passes):
        ins = in_slots[i] + [-2] * (MAX_BINDS - len(in_slots[i]))
        rows.append(f"\t{{\"P{i}\", {out_slot[i]}, {len(in_slots[i])}, {{{', '.join(map(str, ins))}}}}}, /* {b['desc']} */")
    agg_in = in_slots[-1][0]
    open(hdr_path, "w", encoding="utf-8").write(f"""\
/* GENERIERT von tools/gen_fsrcnnx.py aus {src_path.split('/')[-1]} - nicht von Hand ändern. */
#ifndef {guard}
#define {guard}

/* Rechen-Passes in Eingabegröße: Technique, Ausgabeplatz, Anzahl Eingaben, Eingabeplätze
 * (-1 = Quellbild "image", -2 = unbenutzt). Vorher COPY (Quellbild in ein eigenes Ziel), danach
 * AGG (liest Platz {name.upper()}_AGG_IN, schreibt Y' in 2x) und OUT (Zielgröße). */
#define {name.upper()}_SLOTS {nslots}
#define {name.upper()}_PASSES {len(passes)}
#define {name.upper()}_AGG_IN {agg_in}

static const struct fsrcnnx_pass {name}_passes[{name.upper()}_PASSES] = {{
{chr(10).join(rows)}
}};

#endif
""")
    print(f"{name}: {len(passes)} Passes + AGG + OUT, {nslots} Zwischenbilder, {len(effect)} Zeichen")


if __name__ == "__main__":
    main()
