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

echo "==> Building SPDK against it..."
cd /workspace/spdk
[ -f mk/config.mk ] || ./configure --prefix=/workspace/spdk_package --disable-tests \
	--disable-unit-tests --with-dpdk=/usr/local
make -j"$(nproc)"
make install

echo "==> Deduplicating DPDK shared libraries SPDK's install copied..."
cd /workspace/spdk_package/lib
for f in librte_*.so.*; do
	[ -L "$f" ] && continue
	target="/usr/local/lib/x86_64-linux-gnu/$f"
	[ -f "$target" ] && ln -sf "$target" "$f"
done
ldconfig

echo "==> Building loomtabulator..."
cd "$WORKSPACE_DIR/src"
bear -- make

echo "==> Running unit tests (four tiers - see CLAUDE.md's Testing section)..."
make test

echo "==> Building web UI (Phase 3)..."
cd "$WORKSPACE_DIR/web"
npm ci
npm run build
