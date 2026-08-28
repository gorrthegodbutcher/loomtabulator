# Build ../spdk-dpdk-ubuntu's Dockerfile-single first and tag it
# spdk-dpdk-ubuntu:26.05-local - see this project's README.
FROM spdk-dpdk-ubuntu:26.05-local

# Editor/debug tooling on top of the base image's build toolchain. bear
# records the exact compile commands `make` uses so VS Code's IntelliSense
# can resolve DPDK headers without a hand-maintained include path list.
# Same set dpdk-app-example's own Dockerfile installs - this project is
# architecturally its closest sibling (plain DPDK, no SPDK/NVMe).
RUN apt-get update && apt-get install -y --no-install-recommends \
    gdb \
    bear \
    clang-tools \
    && rm -rf /var/lib/apt/lists/*

# This project links against the standalone DPDK build (/usr/local/lib/...)
# via pkg-config, same as dpdk-app-example - see that project's own
# Dockerfile comment for the full story on why LD_LIBRARY_PATH is set
# directly here rather than inherited from the base image (SPDK's own
# internal DPDK build, at spdk_package/lib, has its own same-soname but
# not-guaranteed-same-ABI copy of every librte_*.so).
ENV LD_LIBRARY_PATH="/workspace/spdk/isa-l-install/lib:/usr/local/lib/x86_64-linux-gnu"

# Node/npm are Phase 3 (React Flow web UI) build-time-only tooling - not
# needed for v1, added here when that phase starts. When it does: install
# via NodeSource's setup script (verify Ubuntu 24.04's own nodejs/npm
# version is new enough for Vite/@xyflow/react first), network access at
# build time only, same airgap contract apt/git/pip already rely on. The
# built web/dist/ gets baked into the image; npm/Node are never a runtime
# dependency of the deployed container - see the project plan's Phase 3
# notes for the full design.
