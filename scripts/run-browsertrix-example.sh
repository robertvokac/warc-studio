#!/usr/bin/env bash
set -euo pipefail

# This script demonstrates the Browsertrix command shape used by warc-studio.
# It is not required by the application itself.

URL="${1:-https://example.com}"
COLLECTION="${2:-test}"
CRAWLS_DIR="${3:-$PWD/data/crawls}"
IMAGE="${WARC_STUDIO_BROWSERTRIX_IMAGE:-webrecorder/browsertrix-crawler:latest}"

mkdir -p "$CRAWLS_DIR"

docker run --rm \
  -v "$CRAWLS_DIR:/crawls" \
  -it "$IMAGE" \
  crawl \
  --url "$URL" \
  --generateWACZ \
  --text \
  --collection "$COLLECTION" \
  --crawlId "$COLLECTION"
