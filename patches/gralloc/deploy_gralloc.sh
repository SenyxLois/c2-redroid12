#!/bin/bash
set -e

CONTAINER="${1:-redroid-kenji}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Deploying patched gralloc.gbm to container: $CONTAINER"
docker cp "$DIR/gralloc.gbm.so" "$CONTAINER:/vendor/lib64/hw/gralloc.gbm.so"
docker cp "$DIR/gralloc.gbm.32.so" "$CONTAINER:/vendor/lib/hw/gralloc.gbm.so"
docker exec "$CONTAINER" chmod 644 /vendor/lib64/hw/gralloc.gbm.so /vendor/lib/hw/gralloc.gbm.so

echo "Restarting graphics and media services"
docker exec "$CONTAINER" sh -c "
pkill -9 -f android.hardware.graphics.allocator@2.0-service || true
pkill -9 -f android.hardware.graphics.composer@2.1-service || true
pkill -9 -f mediaswcodec || true
pkill -9 -f android.hardware.media.c2@1.2-service.redroid || true
pkill -9 -f media.codec || true
pkill -9 -f surfaceflinger || true
"
echo "Patched gralloc.gbm"
