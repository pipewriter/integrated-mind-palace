#!/bin/bash
# Spawn a 10x10 grid of videos centered on the game world
#
# Usage:
#   ./scripts/spawn_video_grid.sh <video_file>          # same video x100
#   ./scripts/spawn_video_grid.sh <directory>            # one per file in dir

ARG="${1:?Usage: $0 <video_file|directory>}"

CENTER_X=512
CENTER_Z=512
SPACING=25
Y=55
COLS=10
ROWS=10
TOTAL=$((COLS * ROWS))

HALF_X=$(( (COLS - 1) * SPACING / 2 ))
HALF_Z=$(( (ROWS - 1) * SPACING / 2 ))

# Build file list
FILES=()
if [ -d "$ARG" ]; then
    while IFS= read -r f; do
        FILES+=("$f")
    done < <(find "$ARG" -maxdepth 1 -type f \( -name '*.mp4' -o -name '*.mkv' -o -name '*.webm' -o -name '*.avi' -o -name '*.mov' \) | sort)
    if [ ${#FILES[@]} -eq 0 ]; then echo "No video files found in $ARG"; exit 1; fi
    echo "Found ${#FILES[@]} videos in $ARG"
else
    if [ ! -f "$ARG" ]; then echo "File not found: $ARG"; exit 1; fi
    FILES=("$ARG")
fi

echo "Spawning ${COLS}x${ROWS} video grid at center ($CENTER_X, $CENTER_Z), spacing=$SPACING"

for row in $(seq 0 $((ROWS - 1))); do
    for col in $(seq 0 $((COLS - 1))); do
        n=$(( row * COLS + col ))
        x=$(echo "$CENTER_X - $HALF_X + $col * $SPACING" | bc)
        z=$(echo "$CENTER_Z - $HALF_Z + $row * $SPACING" | bc)

        # Cycle through available files
        vidx=$(( n % ${#FILES[@]} ))
        video="${FILES[$vidx]}"

        echo "[$((n+1))/$TOTAL] $(basename "$video") at ($x, $Y, $z)"
        ./objectcli "$x" "$Y" "$z" "$video"
    done
done

echo "Done — $TOTAL videos placed."
