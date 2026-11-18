# obs-fsr1-filter

Videofilter für OBS Studio, der ein Bild hochskaliert. Fünf Algorithmen stehen zur Wahl:
**AMD FidelityFX Super Resolution 1.0** (EASU + RCAS), **NVIDIA Image Scaling (NIS)**,
**RAVU-Zoom-AR** (aus den mpv-prescalers), **Snapdragon Game Super Resolution 1 (SGSR)** und
das neuronale Netz **FSRCNNX** (aus mpv, zwei Größen). Gedacht für Webcams, Capture-Karten und
Spiele. Alle sind reine Shader und laufen auf jeder Grafikkarte, die OBS unterstützt (NVIDIA,
AMD, Intel) – auch FSRCNNX braucht keine KI-Hardware.

Der Filter heißt in OBS **„Hochskalierung (FSR 1 / NIS / RAVU / SGSR / FSRCNNX)"**. Bestehende
Szenen mit dem alten FSR-Filter funktionieren weiter (die Filter-ID ist unverändert).

Welcher Algorithmus wofür (Faustregel):

| Inhalt | Empfehlung |
|---|---|
| Kamera, IRL, Gesichter, Text im Bild | **FSRCNNX 8** (bei Leistung übrig: FSRCNNX 16) |
| Spiele, Capture-Karte | **SGSR** oder FSR 1 (beide schnell, scharfe Kanten) |
| Glatte Faktoren 1×–2× mit wenig Rechenzeit | NIS |
| Beliebige Faktoren, weiche Kanten ohne Überschwinger | RAVU-Zoom-AR |

## Wie es funktioniert

1. **EASU** rechnet die Quelle auf die Zielauflösung hoch (kantenadaptiv).
2. **RCAS** schärft das Ergebnis, optional mit Rauschunterdrückung.

Der Filter ändert die Größe der Quelle. Eine 1280×720-Webcam mit Faktor 1,5 ist danach
eine 1920×1080-Quelle und wird bei einer 1080p-Leinwand 1:1 dargestellt. Die Skalierung,
die OBS am Szenenelement selbst einstellt (Bilinear, Lanczos …), lässt sich von Plugins
nicht um „FSR" erweitern. Deshalb läuft FSR als Filter auf der Quelle.

FSR 1 ist ein rein räumlicher Upscaler: Er sieht nur das aktuelle Bild und erfindet keine
neuen Details. Er liefert schärfere Kanten als bilineares Skalieren, ersetzt aber keine
KI-Hochskalierung.

## Einstellungen

| Einstellung | Bedeutung |
|---|---|
| Algorithmus | FSR 1 (Standard), NIS, RAVU-Zoom-AR, SGSR, FSRCNNX 8 oder FSRCNNX 16 |
| Skalierungsfaktor | 1,0–4,0 (Standard 1,5, passend für 720p → 1080p) |
| Feste Ausgabegröße verwenden | statt Faktor eine feste Breite × Höhe |
| Schärfe (%) | nur FSR 1: RCAS-Stärke, Standard 80 % |
| Rauschen beim Schärfen reduzieren | nur FSR 1: RCAS-Denoise, Standard an (Webcams rauschen) |
| Schärfe (NIS, %) | nur NIS: Standard 50 % |
| Kantenschärfe (SGSR, %) | nur SGSR: 0 % = Kantenschärfe 1,0, 100 % = 2,0 wie im Original (Standard) |

### NIS: Status und Grenzen

- NIS skaliert nur **1× bis 2×** je Achse (wie im Original). Liegt die Zielgröße außerhalb,
  nutzt der Filter automatisch FSR 1 und schreibt einen Hinweis ins Log.
- NIS läuft in einem einzigen Pass, mit eigener Effect-Datei (`data/effects/nis.effect`).
  Lässt sie sich nicht kompilieren, steht der Grund im OBS-Log (Zeilen mit
  `obs-fsr1-filter`) und FSR 1 funktioniert unverändert weiter.
- Der NIS-Shader übersetzt und rendert im OBS-Lauftest (siehe unten); seine Pixel sind nicht
  mit dem Original verglichen.
- Der Shader und die Koeffiziententabellen entstehen aus Generatoren in `tools/`
  (`gen_nis_effect.py`, `extract_nis_coefs.py`); nicht von Hand ändern.

### RAVU-Zoom-AR: Status und Grenzen

- RAVU ist ein kleiner, vorab trainierter Upscaler: Für jede Kantenrichtung, -stärke und
  -schärfe hat er gelernte Filtergewichte (zwei Tabellen in `data/ravu/`). Er vergrößert in
  **beliebigem Verhältnis** (z. B. 1080p → 1440p) und begrenzt Überschwinger an harten Kanten
  („AR" = Anti-Ringing). Es gibt keine Regler.
- Nur echte Vergrößerungen: Ist die Zielgröße auf einer Achse nicht größer als die Quelle,
  nutzt der Filter FSR 1 und schreibt einen Hinweis ins Log.
- Lädt der Shader oder eine der Tabellen nicht, steht der Grund im OBS-Log und FSR 1 läuft
  weiter.
- Der Shader entsteht mit `tools/gen_ravu.py` aus dem Original `ravu-zoom-ar-r3-rgb.hook`.
  Der Generator übersetzt Zeile für Zeile und bricht bei allem ab, was er nicht kennt.
- **Geprüft:** In einem Software-WebGL-Test liefern Original und Übersetzung auf einem
  Testbild (Kanten, Verläufe, Rauschen, weiße und schwarze Flächen) bei 1,33×, 1,5× und 2×
  dieselben Pixel (Abweichung < 0,00005). Im OBS-Lauftest übersetzt und rendert er (siehe unten).
- Rechenaufwand ähnlich NIS (36 Bildpunkte plus 14 Tabellenzugriffe pro Ausgabepixel).

### SGSR: Status und Grenzen

- Snapdragon Game Super Resolution 1 von Qualcomm (BSD-3-Clause), Variante mit
  Kantenrichtung: ein einziger Pass, 12-Punkt-Lanczos-ähnliche Interpolation der Helligkeit mit
  adaptiver Kantenschärfung, auf das bilinear vergrößerte Bild aufgeschlagen. Sehr günstig, für
  Spiele gedacht, beliebige Vergrößerung.
- Nur echte Vergrößerungen (sonst FSR 1, mit Hinweis im Log).
- Kantenschwelle fest 8/255 (Original für Mobilgeräte); die strengere VR-Schwelle 4/255 würde
  Webcam-Rauschen mitschärfen.
- Abweichungen vom Original: Gather4 ist durch vier Punkt-Abtastungen in der Reihenfolge von
  Direct3D ersetzt, Alpha kommt aus der Quelle, die Kantenschärfe ist ein Regler.

### FSRCNNX: Status und Grenzen

- FSRCNNX von igv (LGPL-3.0-or-later) ist ein kleines neuronales Netz aus mpv, trainiert auf
  echten Fotos: Es rechnet die Helligkeit auf die doppelte Größe und erfindet dabei feine
  Details (Haare, Text, Stoff), die klassische Verfahren nur weichzeichnen. Zwei Größen:
  **FSRCNNX 8** (8 Kanäle, 13 Passes) und **FSRCNNX 16** (16 Kanäle, 25 Passes, etwa viermal so
  viel Rechenzeit).
- Das Netz rechnet immer auf 2×; ein letzter Pass bringt das Ergebnis auf die Zielgröße
  (Catmull-Rom). Bei 2× ist es am besten, 1,5× (720p → 1080p) geht ebenfalls gut, bei mehr als
  2× wird das 2×-Bild klassisch weiter vergrößert. Nur echte Vergrößerungen, Quelle höchstens
  4096 Pixel breit/hoch (sonst FSR 1, mit Hinweis im Log).
- Farbe: Das Netz arbeitet nur auf Y' (BT.709, Gamma-Raum). Die Farbe kommt bilinear aus der
  Quelle und bleibt exakt erhalten, nur die Helligkeit wird ersetzt – wie bei mpv.
- Grafikspeicher: Zwischenbilder RGBA16F in Eingabegröße (FSRCNNX 8: 6, FSRCNNX 16: 12, dank
  Lebensdauer-Analyse) plus ein Bild in doppelter Größe. Bei einer 720p-Webcam etwa 75 MB
  (FSRCNNX 8) bzw. 120 MB (FSRCNNX 16), bei 1080p gut das Doppelte.
- Die Effects (bis 320 KB) werden erst geladen, wenn FSRCNNX gewählt ist, damit das Anlegen der
  Filter nicht langsamer wird.
- Effects und Pass-Tabellen entstehen mit `tools/gen_fsrcnnx.py` aus den Originalen
  `FSRCNNX_x2_8-0-4-1.glsl` und `FSRCNNX_x2_16-0-4-1.glsl` (Releases von FSRCNN-TensorFlow).

### Geprüft in OBS (Lauftest)

Ein Lauftest startet libobs 30.2 ohne Fenster (OpenGL über Xvfb und Mesa llvmpipe), lädt das
gebaute Plugin, schickt ein Testbild (Kanten, Verläufe, Kreis, Streifen, Schachbrett, Rauschen)
durch jeden Algorithmus und liest das Ergebnis zurück (liegt bei der Nachtschicht, nicht in
diesem Repo). Stand 18.11.2026:

| Algorithmus | Ergebnis |
|---|---|
| FSRCNNX 8 / 16 (2×) | gleich der Nachrechnung des mpv-Originals: höchstens 1,9/255, im Mittel 0,3/255 |
| SGSR (1,5× und 2×) | gleich der Nachrechnung des Qualcomm-Originals: im Mittel 0,3/255; einzelne Ausreißer nur dort, wo die Kantenprüfung genau auf der Schwelle liegt oder das Original selbst numerisch instabil ist |
| FSR 1, NIS, RAVU, FSRCNNX 1,5× | übersetzt, rendert, plausibel (kein Pixelvergleich mit dem Original) |

Nicht geprüft: Direct3D (Windows) und echte Grafikkarten; Rechenzeiten auf echter Hardware.

Tipp: Passt die Leinwand nicht zu einem glatten Faktor (z. B. 1080p-Kamera, 1440p-Leinwand),
„Feste Ausgabegröße verwenden" aktivieren und die Leinwandgröße eintragen (2560 × 1440).
Dann muss OBS die Quelle nicht noch einmal herunterskalieren. Der Filter gehört ans Ende der
Filterkette (nach Farbkorrektur o. Ä.).

## Installation (Flatpak-OBS, empfohlen: var-Ordner)

Voraussetzungen: `flatpak`, `flatpak-builder`, OBS Studio als Flatpak und das SDK, mit dem
dieses OBS gebaut wurde:

```bash
# SDK-Version nachsehen (Zeile "Sdk:") und installieren:
flatpak info com.obsproject.Studio | grep -i sdk
flatpak install flathub org.freedesktop.Sdk//26.08
```

Stimmt die Version nicht, `"sdk"` in
`flatpak/com.obsproject.Studio.Plugin.Fsr1Filter.json` anpassen.

Dann im Projektordner:

```bash
./install-to-var.sh
```

Das Skript baut das Plugin im Flatpak-SDK (damit es zur libobs im OBS-Flatpak passt) und
kopiert es nach

```
~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-fsr1-filter/
├── bin/64bit/obs-fsr1-filter.so
└── data/{effects,locale}/…
```

Danach OBS komplett neu starten. Im OBS-Log (Hilfe → Protokolldateien) sollte stehen:

```
[obs-fsr1-filter] geladen
```

Verwendung: Rechtsklick auf die Webcam-Quelle → **Filter** → unter „Effektfilter" auf **+** →
**FSR 1 Hochskalierung**.

### Alternative: als Flatpak-Erweiterung

```bash
flatpak-builder --user --install --force-clean build-dir \
    flatpak/com.obsproject.Studio.Plugin.Fsr1Filter.json
```

Dieser Weg baut fehlerfrei, das Laden als Erweiterung wurde aber nicht getestet.

### Native OBS-Installation (ohne Flatpak)

```bash
cmake -S . -B build && cmake --build build
mkdir -p ~/.config/obs-studio/plugins/obs-fsr1-filter/bin/64bit
cp build/obs-fsr1-filter.so ~/.config/obs-studio/plugins/obs-fsr1-filter/bin/64bit/
cp -r data ~/.config/obs-studio/plugins/obs-fsr1-filter/data
```

Dafür werden die libobs-Entwicklungsdateien benötigt (z. B. `libobs-dev`).

## Teststand

Läuft mit OBS 33.0.1 als Flatpak (Flathub) unter Fedora mit NVIDIA-Treiber und OpenGL
(Webcam-Quelle: DroidCam). Andere Grafik-Backends (Direct3D, Metal) und andere Treiber
wurden nicht ausprobiert.

## Hinweise und bekannte Grenzen

- Im OBS-Log (Hilfe → Protokolldateien) stehen alle Meldungen des Plugins mit
  `[obs-fsr1-filter]`. Beim Start zeigt jeder Filter, welche Algorithmen geladen sind
  („Filter angelegt, verfügbar: FSR 1, NIS, RAVU, SGSR, FSRCNNX bei Bedarf"; FSRCNNX meldet sich
  beim ersten Einschalten mit „effects/fsrcnnx_8.effect geladen"). Beim Umschalten oder wenn sich Kamera- oder
  Zielgröße ändern, folgt eine Zeile wie `'Zencam': RAVU 1920x1080 -> 2560x1440`. Weicht der
  Filter auf FSR 1 aus, steht dort zusätzlich „(gewählt: …, Rückfall)".

- FSR 1 rechnet im Gamma-Raum. Der Filter setzt bewusst nicht das Flag `OBS_SOURCE_SRGB`,
  damit OBS ihn so ausführt.
- Die Alpha-Information der Quelle wird nicht erhalten (Ausgabe ist deckend). Für Webcams
  ist das unproblematisch.
- Bei stark komprimierten Webcam-Bildern (MJPEG) wird Blockrauschen mitverstärkt. Dann Schärfe
  senken und „Rauschen reduzieren" aktiv lassen.
- Annahme im Flatpak-Build: Die Laufzeit `com.obsproject.Studio` bringt libobs-Header und
  CMake-Dateien mit. Das hat sich beim Bauen bestätigt.
- Das Plugin liegt im klassischen Layout (`lib/obs-plugins`, `share/obs/obs-plugins/<name>`
  bzw. im var-Ordner `bin/64bit` + `data`). OBS 33 lädt das weiterhin als „Legacy"-Plugin.

## Entwicklungshinweise zum Shader

OBS übersetzt `.effect`-Dateien selbst und ist dabei empfindlicher als ein normaler
HLSL-/GLSL-Compiler:

- **Fehlerpuffer übergeben.** Ohne Puffer an `gs_effect_create_from_file` verwirft OBS die
  Compiler-Meldung. Das Plugin gibt sie deshalb selbst ins OBS-Log aus.
- **Kein `#define NAME (…)`.** Der Präprozessor hält die Klammer nach dem Namen für eine
  Parameterliste (Fehler „Expected identifier"). Konstanten direkt hinschreiben.
- **Keine Exponenten-Literale** wie `1.0e-8`, stattdessen `0.00000001`.
- **Kein ternärer Operator.** `if`/`else` verwenden.
- **Nichts davon gilt nur für FSR:** `nis.effect` und `ravu.effect` folgen denselben Regeln
  und sind eigene Dateien, damit ein Shaderfehler dort FSR nicht mitreißt. `gen_ravu.py`
  prüft die Regeln beim Erzeugen maschinell.
- **Keine Matrizen oder Arrays.** Das Original von RAVU nutzt `mat4x3`/`matrixCompMult`;
  die Übersetzung rechnet stattdessen mit einzelnen `float3`-Werten.
- Statt Gather4 werden einzelne Punkt-Abtastungen verwendet (läuft auf allen Backends).
- **`gs_technique_end()` leert alle Parameter eines Effects** (libobs `effect.c`). Wer mit
  einem Effect mehrere Passes nacheinander rechnet (FSRCNNX), muss Texturen und Werte vor jedem
  Pass neu setzen – sonst rechnet ab dem zweiten Pass alles mit Nullen.
- **Keine Schleifen** in den generierten Shadern (Catmull-Rom ist ausgeschrieben).

## Lizenz

GPL-2.0-or-later (wie libobs), siehe `LICENSE`. Die Lizenztexte der verwendeten
Drittanbieter-Shader stehen in `THIRD_PARTY_NOTICES.txt` und werden mit installiert.
Die Shader basieren auf AMDs `ffx_fsr1.h`
(MIT-Lizenz, Copyright © 2021 Advanced Micro Devices, Inc.) und NVIDIAs NIS
(`NIS_Scaler.h`, `NIS_Config.h`; MIT-Lizenz, Copyright © 2022 NVIDIA Corporation & Affiliates).

Ausnahmen: Die RAVU-Dateien (`data/effects/ravu.effect`, `data/ravu/*.bin`) sind aus RAVU von
Bin Jin übersetzt, die FSRCNNX-Dateien (`data/effects/fsrcnnx_*.effect`,
`src/fsrcnnx_*_passes.h`) aus FSRCNNX von igv; beide stehen wie die Originale unter
**LGPL-3.0-or-later** (Volltexte in `LICENSES/`). Der SGSR-Shader (`data/effects/sgsr.effect`)
basiert auf Qualcomms Snapdragon GSR 1 (BSD-3-Clause, Text in `THIRD_PARTY_NOTICES.txt`).
