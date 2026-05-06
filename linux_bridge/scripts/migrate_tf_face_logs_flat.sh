#!/bin/sh
# 将旧版目录 .../YYYY-MM-DD/events.jsonl 合并为扁平 .../YYYY-MM-DD.jsonl，并删除旧目录。
# 在小核 Linux 上执行，默认 BASE=/mnt/tf/face_logs；慎用：请先备份 TF。
#
#   sh migrate_tf_face_logs_flat.sh
#   sh migrate_tf_face_logs_flat.sh /mnt/tf/face_logs

set -eu
BASE="${1:-/mnt/tf/face_logs}"
cd "$BASE" || exit 1

for d in "$BASE"/*/; do
    [ -d "$d" ] || continue
    name="$(basename "$d")"
    echo "$name" | grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}$' || continue
    LEG="$BASE/$name/events.jsonl"
    FLAT="$BASE/$name.jsonl"
    if [ -f "$LEG" ]; then
        echo "merge $LEG -> $FLAT"
        cat "$LEG" >> "$FLAT"
        rm -rf "$BASE/$name"
    else
        echo "skip dir $d (no events.jsonl)"
    fi
done

echo "done. ls:"
ls -la "$BASE"
