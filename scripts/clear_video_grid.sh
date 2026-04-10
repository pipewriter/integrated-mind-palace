#!/bin/bash
# Delete all nodes in the video grid region (matches spawn_video_grid.sh layout)

CENTER_X=512
CENTER_Z=512
SPACING=25
COLS=10
ROWS=10
MARGIN=15  # extra margin around the grid edges

HALF_X=$(( (COLS - 1) * SPACING / 2 ))
HALF_Z=$(( (ROWS - 1) * SPACING / 2 ))

X1=$(( CENTER_X - HALF_X - MARGIN ))
Z1=$(( CENTER_Z - HALF_Z - MARGIN ))
X2=$(( CENTER_X + HALF_X + MARGIN ))
Z2=$(( CENTER_Z + HALF_Z + MARGIN ))

echo "Clearing video grid region ($X1,$Z1) to ($X2,$Z2)"
./objectcli --delete-region "$X1" "$Z1" "$X2" "$Z2" "$@"
