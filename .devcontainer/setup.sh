#!/bin/bash
# Runs once when the dev container is first created (devcontainer.json's
# postCreateCommand). Builds standalone DPDK (skipped if already
# configured, so reopening the container after the first time is fast),
# then builds this project and runs its two-tier test suite.
#
# Like dpdk-app-example, this project doesn't touch SPDK at all - it's
# plain DPDK (reads a ring, writes UDP - no NVMe involved), so only the
# standalone DPDK build is needed here.
set -euo pipefail

WORKSPACE_DIR="$PWD"

echo "==> Building DPDK (first-time setup takes a few minutes)..."
cd /workspace/spdk/dpdk
[ -f /workspace/dpdk_build/build.ninja ] || meson setup /workspace/dpdk_build
ninja -C /workspace/dpdk_build
ninja -C /workspace/dpdk_build install
ldconfig

echo "==> Building loomtabulator..."
cd "$WORKSPACE_DIR/src"
bear -- make

echo "==> Running unit tests (stage chain + graph config)..."
make test

# Phase 3 adds a step here: cd ../web && npm ci && npm run build - see
# the Dockerfile's own comment on why that's not part of v1.
