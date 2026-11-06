# obs-fsr1-filter

Videofilter für OBS Studio, der ein Bild hochskaliert. Zwei Algorithmen stehen zur Wahl:
**AMD FidelityFX Super Resolution 1.0** (EASU + RCAS) und **NVIDIA Image Scaling (NIS)**.
Gedacht für Webcams.

Der Filter heißt in OBS **„Hochskalierung (FSR 1 / NIS)"**. Bestehende Szenen mit dem
alten FSR-Filter funktionieren weiter (die Filter-ID ist unverändert).

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
| Algorithmus | FSR 1 (Standard) oder NIS |
| Skalierungsfaktor | 1,0–4,0 (Standard 1,5, passend für 720p → 1080p) |
| Feste Ausgabegröße verwenden | statt Faktor eine feste Breite × Höhe |
| Schärfe (%) | nur FSR 1: RCAS-Stärke, Standard 80 % |
| Rauschen beim Schärfen reduzieren | nur FSR 1: RCAS-Denoise, Standard an (Webcams rauschen) |
| Schärfe (NIS, %) | nur NIS: Standard 50 % |

### NIS: Status und Grenzen

- NIS skaliert nur **1× bis 2×** je Achse (wie im Original). Liegt die Zielgröße außerhalb,
  nutzt der Filter automatisch FSR 1 und schreibt einen Hinweis ins Log.
- NIS läuft in einem einzigen Pass, mit eigener Effect-Datei (`data/effects/nis.effect`).
  Lässt sie sich nicht kompilieren, steht der Grund im OBS-Log (Zeilen mit
  `obs-fsr1-filter`) und FSR 1 funktioniert unverändert weiter.
- **Der NIS-Shader ist noch nicht in OBS getestet.** Das Plugin-C ist gegen die libobs-Header
  geprüft, der Shader gegen die bekannten OBS-Parser-Stolpersteine durchgesehen, mehr nicht.
- Der Shader und die Koeffiziententabellen entstehen aus Generatoren in `tools/`
  (`gen_nis_effect.py`, `extract_nis_coefs.py`); nicht von Hand ändern.

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
- **Nichts davon gilt nur für FSR:** `nis.effect` folgt denselben Regeln und ist eine eigene
  Datei, damit ein Shaderfehler dort FSR nicht mitreißt.
- Statt Gather4 werden einzelne Punkt-Abtastungen verwendet (läuft auf allen Backends).

## Lizenz

GPL-2.0-or-later (wie libobs), siehe `LICENSE`. Die Lizenztexte der verwendeten
Drittanbieter-Shader stehen in `THIRD_PARTY_NOTICES.txt` und werden mit installiert.
Die Shader basieren auf AMDs `ffx_fsr1.h`
(MIT-Lizenz, Copyright © 2021 Advanced Micro Devices, Inc.) und NVIDIAs NIS
(`NIS_Scaler.h`, `NIS_Config.h`; MIT-Lizenz, Copyright © 2022 NVIDIA Corporation & Affiliates).
