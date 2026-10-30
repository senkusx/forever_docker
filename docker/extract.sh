#!/bin/bash
# Extracts the game data (dbc, gt, cameras, maps, vmaps and optionally mmaps) from the WoW client
# mounted at /wow into /opt/trinitycore/data.
#   EXTRACT_MMAPS=1 also generates mmaps (long, ~1.6 GB; only needed with MMAP_PATHFINDING=1)
set -euo pipefail

WOW=/wow
OUT=/opt/trinitycore/data
BIN=/opt/trinitycore/bin

if [ ! -f "$WOW/.build.info" ] || [ ! -d "$WOW/Data" ]; then
    echo "[extract] ERROR: $WOW must be the 'World of Warcraft' folder (with .build.info and Data). Check WOW_DIR in .env."
    exit 1
fi

# Battle.net product of the client in .build.info: wowf (folder _forever_) since 1.60.1.70685, wow_classic_beta
# (_classic_beta_) before. CASC_PRODUCT forces one.
listed() { awk -F'|' -v p="$1" '{ sub(/\r$/, "") } NR > 1 && $NF == p { found = 1 } END { exit !found }' "$WOW/.build.info"; }
PRODUCT="${CASC_PRODUCT:-}"
if [ -z "$PRODUCT" ]; then
    for p in wowf wow_classic_beta; do
        if listed "$p"; then PRODUCT=$p; break; fi
    done
fi
if [ -z "$PRODUCT" ] || ! listed "$PRODUCT"; then
    echo "[extract] ERROR: $WOW/.build.info lists no ${PRODUCT:-WoW Forever product (wowf or wow_classic_beta)}."
    echo "[extract] Copy the hidden .build.info file next to Data too when you upload the client: Battle.net rewrites"
    echo "[extract] it at every update, and Data alone does not say which build is installed."
    exit 1
fi
version="$(awk -F'|' -v p="$PRODUCT" '{ sub(/\r$/, "") } NR == 1 { for (i = 1; i <= NF; i++) if ($i ~ /^Version!/) c = i } NR > 1 && $NF == p { print $c }' "$WOW/.build.info")"
echo "[extract] client: product $PRODUCT, version ${version:-unknown}"

cd "$OUT"
echo "[extract] === 1/3 mapextractor (dbc, gt, cameras, maps)"
"$BIN/mapextractor" -i "$WOW" -o "$OUT" -p "$PRODUCT"
# mapextractor exits 0 even when it cannot open the CASC storage
if [ ! -d "$OUT/dbc" ] || [ ! -d "$OUT/maps" ]; then
    echo "[extract] ERROR: mapextractor produced no dbc/maps, see the messages above."
    exit 1
fi

echo "[extract] === 2/3 vmap4extractor"
rm -rf "$OUT/.work" && mkdir -p "$OUT/.work" && cd "$OUT/.work"
locale_dir="$(find "$OUT/dbc" -mindepth 1 -maxdepth 1 -type d -print -quit)"
if [ -z "$locale_dir" ]; then
    echo "[extract] ERROR: mapextractor produced no DBC locale directory."
    exit 1
fi
"$BIN/vmap4extractor" -d "$WOW" -p "$PRODUCT" -dl "${locale_dir##*/}"

echo "[extract] === 3/3 vmap4assembler"
rm -rf "$OUT/vmaps" && mkdir -p "$OUT/vmaps"
"$BIN/vmap4assembler" Buildings "$OUT/vmaps"
[ -f "$OUT/vmaps/GameObjectModels.dtree" ] || { echo "[extract] ERROR: vmaps incomplete (no GameObjectModels.dtree)"; exit 1; }
cd "$OUT" && rm -rf "$OUT/.work"

if [ "${EXTRACT_MMAPS:-0}" = 1 ]; then
    echo "[extract] === mmaps_generator (long)"
    "$BIN/mmaps_generator" --threads "$(nproc)"
fi

echo "[extract] done:"
du -sh "$OUT"/* 2>/dev/null
