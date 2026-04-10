#!/bin/bash
# Generate 100 unique test videos (5 seconds each, 320x240, 24fps)
# Each has a different background color and number overlay

OUT="test_videos"
mkdir -p "$OUT"
COUNT=${1:-100}
DURATION=5
RES="320x240"

echo "Generating $COUNT test videos in $OUT/ ..."

for i in $(seq 1 "$COUNT"); do
    # Cycle through distinct hues
    r=$(( (i * 37) % 256 ))
    g=$(( (i * 73 + 100) % 256 ))
    b=$(( (i * 131 + 50) % 256 ))
    hex=$(printf '0x%02x%02x%02x' $r $g $b)

    ffmpeg -y -loglevel error \
        -f lavfi -i "color=c=$hex:s=$RES:d=$DURATION:r=24" \
        -f lavfi -i "sine=frequency=$((200 + i * 10)):duration=$DURATION" \
        -vf "drawtext=text='Video $i':fontsize=48:fontcolor=white:x=(w-text_w)/2:y=(h-text_h)/2:borderw=2:bordercolor=black" \
        -c:v libx264 -preset ultrafast -crf 28 \
        -c:a aac -b:a 64k \
        -shortest \
        "$OUT/test_$(printf '%03d' $i).mp4"

    printf "\r[%d/%d]" "$i" "$COUNT"
done

echo ""
echo "Done — $COUNT videos in $OUT/"
