#!/usr/bin/env python3
"""Erzeugt aus einem RAVU-Zoom-AR-Shader von mpv (RGB-Variante) den OBS-Effect
und die beiden Gewichtstabellen (Lookup-Texturen).

Aufruf:
  python3 -I tools/gen_ravu.py <ravu-zoom-ar-r3-rgb.hook> data/effects/ravu.effect data/ravu

Quelle: https://github.com/bjin/mpv-prescalers von Bin Jin (LGPL-3.0-or-later).
Der erzeugte Effect und die Tabellen stehen deshalb ebenfalls unter LGPL-3.0-or-later.

Der Generator übersetzt das Original Zeile für Zeile. Jede Zeile muss einem bekannten
Muster entsprechen, sonst bricht er ab: Lieber ein Fehler beim Generieren als ein
stiller Fehler im Shader. Dabei gelten die Regeln, die beim FSR-Shader in OBS gelernt wurden:

  * kein '#define NAME (...)'  (OBS' Präprozessor liest die Klammer als Parameterliste)
  * keine Exponenten-Literale wie 1.0e-7
  * kein ternärer Operator und kein mix(a, b, bool): stattdessen if/else
  * keine Arrays und keine Matrizen (mat4x3/matrixCompMult): einzelne float3-Werte
  * Bildzugriffe als Punkt-Abtastung, nur die Gewichtstabellen werden linear gefiltert
    (genau wie im Original, die Filterung interpoliert dort zwischen Subpixel-Phasen)

Abweichungen vom Original (beide nur in Grenzfällen wirksam):
  * sqrt(L2) wird als sqrt(max(L2, 0)) gerechnet. L2 ist mathematisch nie negativ,
    durch Rundung aber manchmal minimal; das Original erzeugt dann NaN, und was daraus
    wird, hängt vom Treiber ab.
  * Der Winkelindex wird auf 0..23 begrenzt, damit Rundung nie aus der Tabelle führt.
"""
import os
import re
import struct
import sys

PI = "3.141592653589793"


def die(msg):
    raise SystemExit("gen_ravu: " + msg)


def parse_hook(text):
    lines = text.split("\n")
    # Kopf und Körper der Shader-Funktion
    try:
        start = lines.index("vec4 hook() {")
    except ValueError:
        die("'vec4 hook() {' nicht gefunden")
    end = start + 1
    while lines[end] != "}":
        end += 1
    body = lines[start + 1:end]

    header = lines[:start]
    if "//!HOOK MAIN" not in header:
        die("erwartet die RGB-Variante (//!HOOK MAIN)")
    if "const vec3 color_primary = vec3(0.2126, 0.7152, 0.0722);" not in header:
        die("unbekannte Luma-Gewichte")

    # Lookup-Texturen
    textures = {}
    i = end + 1
    while i < len(lines):
        m = re.fullmatch(r"//!TEXTURE (\w+)", lines[i])
        if m:
            name = m.group(1)
            size = re.fullmatch(r"//!SIZE (\d+) (\d+)", lines[i + 1])
            if not size or lines[i + 2] != "//!FORMAT rgba16f" or lines[i + 3] != "//!FILTER LINEAR":
                die("unerwarteter Texturkopf bei " + name)
            w, h = int(size.group(1)), int(size.group(2))
            raw = bytes.fromhex(lines[i + 4].strip())
            # Die Daten stehen als 32-Bit-Floats im Hook (16 Bytes je Texel); die Grafikkarte
            # hält sie als RGBA16F. Wir wandeln deshalb schon hier in Half-Floats um.
            if len(raw) != w * h * 4 * 4:
                die("%s: %d Bytes statt %d" % (name, len(raw), w * h * 16))
            count = w * h * 4
            values = struct.unpack("<%df" % count, raw)
            for v in values:
                if not (-60000.0 < v < 60000.0) or v != v:
                    die("%s: Wert %r passt nicht in Half-Float" % (name, v))
            data = struct.pack("<%de" % count, *values)
            textures[name] = (w, h, data)
            i += 5
        else:
            i += 1
    return body, textures


# Feste Blöcke des Originals, die von Hand portiert sind. Der Generator prüft, dass das
# Original sie wörtlich enthält, damit eine geänderte Vorlage nicht unbemerkt durchrutscht.
EIGEN_SRC = """float a = abd.x, b = abd.y, d = abd.z;
float T = a + d, D = a * d - b * b;
float delta = sqrt(max(T * T / 4.0 - D, 0.0));
float L1 = T / 2.0 + delta, L2 = T / 2.0 - delta;
float sqrtL1 = sqrt(L1), sqrtL2 = sqrt(L2);
float theta = mix(mod(atan(L1 - a, b) + 3.141592653589793, 3.141592653589793), 0.0, abs(b) < 1.192092896e-7);
float lambda = sqrtL1;
float mu = mix((sqrtL1 - sqrtL2) / (sqrtL1 + sqrtL2), 0.0, sqrtL1 + sqrtL2 < 1.192092896e-7);
float angle = floor(theta * 24.0 / 3.141592653589793);
float strength = mix(mix(0.0, 1.0, lambda >= 0.004), mix(2.0, 3.0, lambda >= 0.05), lambda >= 0.016);
float coherence = mix(mix(0.0, 1.0, mu >= 0.25), 2.0, mu >= 0.5);
float coord_y = ((angle * 4.0 + strength) * 3.0 + coherence) / 288.0;""".split("\n")

EIGEN_DST = """	// Strukturtensor -> Winkel, Stärke, Kohärenz (Eigenwerte der 2x2-Matrix)
	float a = abd.x;
	float b = abd.y;
	float d = abd.z;
	float T = a + d;
	float D = a * d - b * b;
	float delta = sqrt(max(T * T / 4.0 - D, 0.0));
	float L1 = T / 2.0 + delta;
	float L2 = T / 2.0 - delta;
	float sqrtL1 = sqrt(L1);
	float sqrtL2 = sqrt(max(L2, 0.0));
	float theta = 0.0;
	if (abs(b) >= 0.0000001192092896) {
		theta = fmod(atan2(L1 - a, b) + PI_VAL, PI_VAL);
	}
	float lambda = sqrtL1;
	float mu = 0.0;
	if (sqrtL1 + sqrtL2 >= 0.0000001192092896) {
		mu = (sqrtL1 - sqrtL2) / (sqrtL1 + sqrtL2);
	}
	float angle = min(floor(theta * 24.0 / PI_VAL), 23.0);
	float strength = 0.0;
	if (lambda >= 0.016) {
		strength = 2.0;
		if (lambda >= 0.05) {
			strength = 3.0;
		}
	} else {
		if (lambda >= 0.004) {
			strength = 1.0;
		}
	}
	float coherence = 0.0;
	if (mu >= 0.25) {
		coherence = 1.0;
	}
	if (mu >= 0.5) {
		coherence = 2.0;
	}
	float coord_y = ((angle * 4.0 + strength) * 3.0 + coherence) / 288.0;""".replace("PI_VAL", PI).split("\n")

DECL_SRC = [
    "vec3 res = vec3(0.0);",
    "vec4 w;",
    "mat4x3 cg, cg1;",
    "vec3 lo = vec3(0.0), hi = vec3(0.0);",
    "vec3 lo2 = vec3(0.0), hi2 = vec3(0.0);",
]
DECL_DST = [
    "\tfloat3 res = float3(0.0, 0.0, 0.0);",
    "\tfloat3 lo = float3(0.0, 0.0, 0.0);",
    "\tfloat3 hi = float3(0.0, 0.0, 0.0);",
    "\tfloat3 lo2 = float3(0.0, 0.0, 0.0);",
    "\tfloat3 hi2 = float3(0.0, 0.0, 0.0);",
    "\tfloat4 w;",
]

LUT_PARAM = {"ravu_zoom_lut3": "lut", "ravu_zoom_lut3_ar": "lut_ar",
             "ravu_zoom_lut2": "lut", "ravu_zoom_lut2_ar": "lut_ar"}
COMP = "xyzw"
NUM = r"-?\d+\.\d+"


def vec(s):
    """vec2(...)/vec3(...) -> float2/float3; einargumentige Konstruktoren sind verboten."""
    return re.sub(r"\bvec([234])\(", r"float\1(", s)


def translate_body(body):
    out = []
    ar_exp = None
    i = 0
    n = len(body)

    def expect(k, pattern):
        if k >= n or not re.fullmatch(pattern, body[k]):
            die("Zeile %d passt nicht: %r (erwartet %s)" % (k, body[k] if k < n else None, pattern))
        return re.fullmatch(pattern, body[k])

    # Kopf: Position und Subpixel
    m = expect(0, r"vec2 pos = HOOKED_pos \* HOOKED_size;")
    out.append("\t// Position im Quellbild (in Pixeln), auf Pixelmitte gerundet")
    out.append("\tfloat2 pos = v_in.uv * input_size;")
    expect(1, r"vec2 subpix = fract\(pos - 0\.5\);")
    out.append("\tfloat2 subpix = frac(pos - 0.5);")
    expect(2, r"pos -= subpix;")
    out.append("\tpos -= subpix;")
    m = expect(3, r"subpix = LUTPOS\(subpix, vec2\((\d+\.\d+)\)\);")
    lut_n = m.group(1)
    out.append("\t// LUTPOS: Phase auf die Texelmitten der Tabelle abbilden")
    # Bewusst wörtlich wie im Original (mix -> lerp): Das Anti-Ringing reagiert extrem
    # empfindlich auf winzige Rundungsunterschiede in dieser Koordinate.
    lo_s = "0.5 / %s" % lut_n
    hi_s = "1.0 - 0.5 / %s" % lut_n
    out.append("\tsubpix = lerp(float2(%s, %s), float2(%s, %s), subpix);" % (lo_s, lo_s, hi_s, hi_s))
    expect(4, r"vec2 subpix_inv = 1\.0 - subpix;")
    out.append("\tfloat2 subpix_inv = float2(1.0, 1.0) - subpix;")
    k = 5
    for name in ("subpix_ar", "subpix_inv_ar"):
        m = expect(k, r"vec2 %s = (subpix(?:_inv)?) / vec2\((%s), (%s)\);" % (name, NUM, NUM))
        out.append("\tfloat2 %s = %s / float2(%s, %s);" % (name, m.group(1), m.group(2), m.group(3)))
        k += 1
    for name in ("subpix", "subpix_inv"):
        m = expect(k, r"%s /= vec2\((%s), (%s)\);" % (name, NUM, NUM))
        out.append("\t%s /= float2(%s, %s);" % (name, m.group(1), m.group(2)))
        k += 1

    i = k
    samples = 0
    out.append("")
    out.append("\t// 6x6 Bildpunkte (RGB)")
    while i < n and body[i].startswith("vec3 sample"):
        m = expect(i, r"vec3 sample(\d+) = HOOKED_tex\(\(pos \+ vec2\((%s),(%s)\)\) \* HOOKED_pt\)\.xyz;" % (NUM, NUM))
        out.append("\tfloat3 sample%s = Fetch(pos + float2(%s, %s));" % m.groups())
        samples += 1
        i += 1
    if samples != 36:
        die("%d statt 36 Abtastungen" % samples)

    out.append("")
    out.append("\t// Helligkeit")
    out.append("\tfloat3 color_primary = float3(0.2126, 0.7152, 0.0722);")
    while i < n and body[i].startswith("float luma"):
        m = expect(i, r"float luma(\d+) = dot\(sample(\d+), color_primary\);")
        if m.group(1) != m.group(2):
            die("luma/sample-Index verschieden: " + body[i])
        out.append("\t" + body[i])
        i += 1

    expect(i, r"vec3 abd = vec3\(0\.0\);")
    out.append("")
    out.append("\t// Strukturtensor aus den Gradienten (gewichtet)")
    out.append("\tfloat3 abd = float3(0.0, 0.0, 0.0);")
    i += 1
    expect(i, r"float gx, gy;")
    out.append("\tfloat gx;")
    out.append("\tfloat gy;")
    i += 1
    grad_re = r"g[xy] = \(-?[-+*0-9.luma ]+\)/(2|12)\.0;"
    while i < n and (body[i].startswith("gx =") or body[i].startswith("gy =") or body[i].startswith("abd +=")):
        if body[i].startswith("abd"):
            m = expect(i, r"abd \+= vec3\(gx \* gx, gx \* gy, gy \* gy\) \* (\d\.\d+);")
            out.append("\tabd += float3(gx * gx, gx * gy, gy * gy) * %s;" % m.group(1))
        else:
            expect(i, grad_re)
            out.append("\t" + body[i])
        i += 1

    # Eigenwerte (von Hand portiert)
    if body[i:i + len(EIGEN_SRC)] != EIGEN_SRC:
        die("Eigenwert-Block weicht vom erwarteten Original ab (Zeile %d)" % i)
    out.append("")
    out.extend(EIGEN_DST)
    i += len(EIGEN_SRC)

    if body[i:i + len(DECL_SRC)] != DECL_SRC:
        die("Deklarationsblock weicht ab (Zeile %d)" % i)
    out.append("")
    out.extend(DECL_DST)
    i += len(DECL_SRC)

    taps = 0
    ar_taps = 0
    while i < n:
        line = body[i]
        m = re.fullmatch(r"w = texture\((\w+), vec2\((%s), coord_y\) \+ (subpix(?:_inv)?(?:_ar)?)\);" % NUM, line)
        if m:
            if m.group(1) not in LUT_PARAM:
                die("unbekannte Tabelle " + m.group(1))
            out.append("\tw = %s.Sample(linear_clamp, float2(%s, coord_y) + %s);"
                       % (LUT_PARAM[m.group(1)], m.group(2), m.group(3)))
            i += 1
            continue
        m = re.fullmatch(r"res \+= sample(\d+) \* w\[([0-3])\];", line)
        if m:
            out.append("\tres += sample%s * w.%s;" % (m.group(1), COMP[int(m.group(2))]))
            taps += 1
            i += 1
            continue
        m = re.fullmatch(r"cg = mat4x3\(0\.1 \+ sample(\d+), 1\.1 - sample\1, 0\.1 \+ sample(\d+), 1\.1 - sample\2\);", line)
        if m:
            sa, sb = m.group(1), m.group(2)
            expect(i + 1, r"cg1 = cg;")
            sq = body[i + 2]
            cnt = sq.count("cg = matrixCompMult(cg, cg);")
            if sq != "cg = matrixCompMult(cg, cg);" * cnt or cnt < 1:
                die("unerwartete Potenzzeile: " + sq)
            if ar_exp is None:
                ar_exp = cnt
            elif ar_exp != cnt:
                die("uneinheitliche Potenz im Anti-Ringing")
            m1 = expect(i + 3, r"hi \+= cg\[0\] \* w\[([0-3])\] \+ cg\[2\] \* w\[([0-3])\];")
            wa, wb = m1.group(1), m1.group(2)
            expect(i + 4, r"lo \+= cg\[1\] \* w\[%s\] \+ cg\[3\] \* w\[%s\];" % (wa, wb))
            expect(i + 5, r"cg = matrixCompMult\(cg, cg1\);")
            expect(i + 6, r"hi2 \+= cg\[0\] \* w\[%s\] \+ cg\[2\] \* w\[%s\];" % (wa, wb))
            expect(i + 7, r"lo2 \+= cg\[1\] \* w\[%s\] \+ cg\[3\] \* w\[%s\];" % (wa, wb))
            out.append("\tArAcc(hi, lo, hi2, lo2, sample%s, w.%s);" % (sa, COMP[int(wa)]))
            out.append("\tArAcc(hi, lo, hi2, lo2, sample%s, w.%s);" % (sb, COMP[int(wb)]))
            ar_taps += 2
            i += 8
            continue
        if line == "hi = hi2 / hi - 0.1;":
            expect(i + 1, r"lo = 1\.1 - lo2 / lo;")
            m = expect(i + 2, r"res = mix\(res, clamp\(res, lo, hi\), (\d\.\d+)\);")
            expect(i + 3, r"return vec4\(res, 1\.0\);")
            if i + 4 != n:
                die("unerwartete Zeilen nach return")
            out.append("")
            out.append("\t// Anti-Ringing: Ergebnis teilweise auf die lokale weiche Min/Max-Grenze begrenzen")
            out.append("\thi = hi2 / hi - 0.1;")
            out.append("\tlo = 1.1 - lo2 / lo;")
            out.append("\tres = lerp(res, clamp(res, lo, hi), %s);" % m.group(1))
            out.append("\treturn float4(saturate(res), 1.0);")
            i = n
            break
        die("Zeile %d unbekannt: %r" % (i, line))

    if taps != 36:
        die("%d statt 36 gewichtete Abtastungen" % taps)
    if ar_exp is None or ar_taps == 0:
        die("kein Anti-Ringing-Block gefunden")
    return out, ar_exp, ar_taps


def build_effect(body_out, ar_exp, desc):
    L = []
    a = L.append
    a("// %s für OBS Studio" % desc)
    a("// GENERIERT von tools/gen_ravu.py - nicht von Hand ändern.")
    a("// Original: RAVU aus https://github.com/bjin/mpv-prescalers")
    a("// Autor des Originals: Bin Jin. Lizenz: LGPL-3.0-or-later, siehe THIRD_PARTY_NOTICES.txt.")
    a("//")
    a("// Skaliert in beliebigem Verhältnis hoch. Die Gewichte stehen in zwei Tabellen-")
    a("// texturen (RGBA16F), die das Plugin aus data/ravu/*.bin lädt und linear filtert.")
    a("")
    a("uniform float4x4 ViewProj;")
    a("uniform texture2d image;")
    a("uniform texture2d lut;       // Gewichte des Skalierers")
    a("uniform texture2d lut_ar;    // Gewichte des Anti-Ringing")
    a("uniform float2 input_size;   // Quellgröße in Pixeln")
    a("")
    a("sampler_state point_clamp {")
    a("\tFilter    = Point;")
    a("\tAddressU  = Clamp;")
    a("\tAddressV  = Clamp;")
    a("};")
    a("")
    a("sampler_state linear_clamp {")
    a("\tFilter    = Linear;")
    a("\tAddressU  = Clamp;")
    a("\tAddressV  = Clamp;")
    a("};")
    a("")
    a("struct VertData {")
    a("\tfloat4 pos : POSITION;")
    a("\tfloat2 uv  : TEXCOORD0;")
    a("};")
    a("")
    a("VertData VSDefault(VertData v_in)")
    a("{")
    a("\tVertData vert_out;")
    a("\tvert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);")
    a("\tvert_out.uv  = v_in.uv;")
    a("\treturn vert_out;")
    a("}")
    a("")
    a("// Bildpunkt an einer Pixelmitte (pos ist bereits die Mitte in Pixeln)")
    a("float3 Fetch(float2 p)")
    a("{")
    a("\treturn image.Sample(point_clamp, p / input_size).rgb;")
    a("}")
    a("")
    a("// Anti-Ringing-Beitrag eines Bildpunkts (im Original als mat4x3/matrixCompMult):")
    a("// hi/lo sammeln (0.1 + s)^%d bzw. (1.1 - s)^%d, hi2/lo2 die nächsthöhere Potenz." % (2 ** ar_exp, 2 ** ar_exp))
    a("void ArAcc(inout float3 hi, inout float3 lo, inout float3 hi2, inout float3 lo2, float3 s, float wt)")
    a("{")
    a("\tfloat3 c_hi = float3(0.1, 0.1, 0.1) + s;")
    a("\tfloat3 c_lo = float3(1.1, 1.1, 1.1) - s;")
    a("\tfloat3 p_hi = c_hi;")
    a("\tfloat3 p_lo = c_lo;")
    for _ in range(ar_exp):
        a("\tp_hi = p_hi * p_hi;")
        a("\tp_lo = p_lo * p_lo;")
    a("\thi += p_hi * wt;")
    a("\tlo += p_lo * wt;")
    a("\thi2 += p_hi * c_hi * wt;")
    a("\tlo2 += p_lo * c_lo * wt;")
    a("}")
    a("")
    a("float4 PSRavu(VertData v_in) : TARGET")
    a("{")
    L.extend(body_out)
    a("}")
    a("")
    a("technique RAVU")
    a("{")
    a("\tpass")
    a("\t{")
    a("\t\tvertex_shader = VSDefault(v_in);")
    a("\t\tpixel_shader  = PSRavu(v_in);")
    a("\t}")
    a("}")
    a("")
    return "\n".join(L)


def check_obs_rules(src):
    """Die Stolpersteine aus der FSR-Entwicklung, maschinell geprüft."""
    for no, line in enumerate(src.split("\n"), 1):
        code = line.split("//")[0]
        if re.search(r"#\s*define", code):
            die("Zeile %d: #define" % no)
        if re.search(r"\d[eE][-+]?\d", code):
            die("Zeile %d: Exponenten-Literal" % no)
        if "?" in code:
            die("Zeile %d: ternärer Operator" % no)
        if re.search(r"\b(vec[234]|mat\d|mix|fract|texture|matrixCompMult|mod)\s*\(", code):
            die("Zeile %d: GLSL-Konstrukt übrig: %s" % (no, line.strip()))
        if re.search(r"\[", code):
            die("Zeile %d: Array-/Indexzugriff" % no)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    hook_path, effect_path, lut_dir = sys.argv[1:]
    with open(hook_path, "r", encoding="utf-8") as f:
        text = f.read()
    desc_m = re.search(r"^//!DESC (.+)$", text, re.M)
    desc = desc_m.group(1) if desc_m else "RAVU-Zoom-AR"

    body, textures = parse_hook(text)
    body_out, ar_exp, ar_taps = translate_body(body)
    effect = build_effect(body_out, ar_exp, desc)
    check_obs_rules(effect)

    # Tabellen: Name im Effect -> Datei
    names = {}
    for tex_name, param in LUT_PARAM.items():
        if tex_name in textures:
            names[param] = tex_name
    if set(names) != {"lut", "lut_ar"}:
        die("Tabellen fehlen: gefunden " + ", ".join(textures))

    with open(effect_path, "w", encoding="utf-8") as f:
        f.write(effect)
    os.makedirs(lut_dir, exist_ok=True)
    for param, tex_name in sorted(names.items()):
        w, h, data = textures[tex_name]
        path = os.path.join(lut_dir, "%s_%dx%d.bin" % (param, w, h))
        with open(path, "wb") as f:
            f.write(data)
        print("Tabelle %s (%s): %d x %d RGBA16F -> %s" % (param, tex_name, w, h, path))
    print("Effect: %s (%d Anti-Ringing-Abtastungen, Potenz %d)" % (effect_path, ar_taps, 2 ** ar_exp))


if __name__ == "__main__":
    main()
