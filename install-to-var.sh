#!/usr/bin/env bash
# Baut das Plugin im OBS-Flatpak-SDK und kopiert es in den var-Ordner von OBS
# (~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-fsr1-filter).
# Aufruf im Projektordner:  ./install-to-var.sh
set -euo pipefail

cd "$(dirname "$0")"

MANIFEST=flatpak/com.obsproject.Studio.Plugin.Fsr1Filter.json
DST="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-fsr1-filter"

flatpak-builder --force-clean build-dir "$MANIFEST"

SO=$(find build-dir -name obs-fsr1-filter.so | head -n1)
FX=$(find build-dir -type d -path '*share/obs/obs-plugins/obs-fsr1-filter' | head -n1)

if [ -z "$SO" ] || [ -z "$FX" ]; then
    echo "Build-Ergebnis nicht gefunden (SO='$SO', FX='$FX')" >&2
    exit 1
fi

rm -rf "$DST"
mkdir -p "$DST/bin/64bit" "$DST/data"
cp "$SO" "$DST/bin/64bit/"
cp -r "$FX/." "$DST/data/"

echo
echo "Installiert nach: $DST"
find "$DST" -type f
echo
echo "OBS jetzt komplett beenden und neu starten."
