# SPDX-License-Identifier: BSD-3-Clause

APP = loomtabulator

SRCS-y := main.c ring_input.c json.c pipeline.c graph_config.c \
	  plugin_loader.c testgen.c web_status.c epoch_barrier.c pipeline_worker.c
# common.c and stages/*.c are NOT part of the host binary anymore - every
# stage is a dlopen()'d .so plugin now (see plugin_loader.c/stage_abi.h),
# built separately below by the `plugins` target. common.c (app_parse_ipv4())
# is only ever needed by forward_udp_stage.c, so it's compiled into that
# one plugin's .so instead.

PKGCONF ?= pkg-config

ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

all: shared plugins

.PHONY: shared static plugins plugin-sdk test clean

shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O3 -Wall -Wextra -pthread -I. $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = -pthread -ldl $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = -pthread -ldl $(shell $(PKGCONF) --static --libs libdpdk)

CFLAGS += -DALLOW_EXPERIMENTAL_API

build/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

build:
	@mkdir -p $@

# Stage plugins: built-in stages, rebuilt as .so files loaded through the
# exact same dlopen() mechanism a third-party plugin uses - see
# plugin_loader.c/stage_abi.h. Deliberately NOT $(CFLAGS): no DPDK
# pkg-config flags, no -include rte_config.h - this is what makes "a stage
# plugin doesn't need DPDK installed to build" literally true, and it's
# exactly what a third party building against plugin-sdk/ gets too.
PLUGIN_CFLAGS = -O3 -Wall -Wextra -fPIC -I.
PLUGINS_DIR := ../plugins

plugins: $(PLUGINS_DIR)/validate.so $(PLUGINS_DIR)/extract.so \
	 $(PLUGINS_DIR)/convert.so $(PLUGINS_DIR)/forward_udp.so \
	 $(PLUGINS_DIR)/dump_binary.so $(PLUGINS_DIR)/dump_text.so \
	 $(PLUGINS_DIR)/null_sink.so $(PLUGINS_DIR)/ring_output.so \
	 $(PLUGINS_DIR)/byte_match.so

$(PLUGINS_DIR):
	@mkdir -p $@

$(PLUGINS_DIR)/validate.so: stages/validate_stage.c stages/validate_stage_plugin.c json.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)
$(PLUGINS_DIR)/extract.so: stages/extract_stage.c stages/extract_stage_plugin.c json.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)
$(PLUGINS_DIR)/convert.so: stages/convert_stage.c stages/convert_stage_plugin.c json.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)
$(PLUGINS_DIR)/forward_udp.so: stages/forward_udp_stage.c stages/forward_udp_stage_plugin.c json.c common.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)
$(PLUGINS_DIR)/dump_binary.so: stages/dump_binary_stage.c stages/dump_binary_stage_plugin.c json.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)
$(PLUGINS_DIR)/dump_text.so: stages/dump_text_stage.c stages/dump_text_stage_plugin.c json.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)
$(PLUGINS_DIR)/null_sink.so: stages/null_sink_stage.c stages/null_sink_stage_plugin.c json.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)
$(PLUGINS_DIR)/byte_match.so: stages/byte_match_stage.c stages/byte_match_stage_plugin.c json.c Makefile | $(PLUGINS_DIR)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^)

# ring_output is the one exception to "a stage plugin doesn't need DPDK
# installed to build" above - it genuinely produces an rte_ring, so it
# needs libdpdk's cflags/libs same as the main binary itself (see
# ring_output_stage.c's own header comment). Built with $(CFLAGS)/
# $(LDFLAGS_SHARED), not $(PLUGIN_CFLAGS) - still -shared like every
# other plugin (with an explicit -fPIC $(CFLAGS) doesn't carry, since
# the main binary it's normally used for is an executable, not a
# shared object), still loaded through the exact same dlopen()
# mechanism, just linked against libdpdk like main.c is.
$(PLUGINS_DIR)/ring_output.so: stages/ring_output_stage.c stages/ring_output_stage_plugin.c json.c Makefile $(PC_FILE) | $(PLUGINS_DIR)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(filter %.c,$^) $(LDFLAGS_SHARED)

# What a third-party plugin developer vendors into their own repo - see
# plugin-sdk/README.md. These 4 files are copies of this same repo's own
# src/ originals (not a different repo on its own release cadence, unlike
# common.c/record.h's hand-synced vendoring), so keeping them
# build-generated instead of hand-copied removes an entire "shipped a
# stale SDK" failure class for free.
plugin-sdk:
	@mkdir -p ../plugin-sdk
	cp stage.h stage_abi.h json.h json.c ../plugin-sdk/

# Five tiers, matching dpdk-app-example's own test convention:
#  - test_stage_chain: validate/extract/convert are deliberately mbuf-free
#    (see stage.h), and pipeline.c's tree-walking execution is pure C too
#    (see pipeline.h) - both linked here with zero DPDK involvement, runs
#    as a plain host binary, no hugepages/EAL needed.
#  - test_graph_config: links tests/stub_stage_registry.c, a frozen copy
#    of the pre-plugin-conversion static registry, instead of the real
#    plugin_loader.c - this tier tests graph_config_load()'s chain-
#    building/validation logic against a known-good registry, not the
#    dlopen() machinery itself (that's test_plugin_loader's job, below).
#    forward_udp_stage.c is a plain kernel UDP socket (no DPDK NIC/mbuf
#    involvement, see that file's own header comment), so this whole tier
#    links with zero DPDK involvement, same posture as test_stage_chain.
#  - test_epoch_barrier (Phase 2): epoch_barrier.c is deliberately
#    DPDK-free (see epoch_barrier.h) - real pthreads, zero EAL
#    involvement, same posture as test_stage_chain.
#  - test_plugin_loader: exercises the real dlopen()/dlsym()/ABI-version-
#    check/collision logic in plugin_loader.c against small fixture
#    plugins (tests/plugin_fixtures/) built into throwaway directories
#    (never the real plugins/) - zero DPDK involvement, same posture as
#    test_stage_chain.
#  - test_pipeline_workers (Phase 2): the epoch_barrier/pipeline_worker
#    integration test against a real rte_ring - needs EAL init
#    (--no-huge --no-pci, no mbuf-pool setup at all) but no hardware,
#    same "no hardware needed" posture as the project plan's v1 datapath
#    smoke test. This is the only test tier that still needs DPDK at
#    all, since it's the only one exercising the real rte_ring +
#    lcore-launch machinery main.c itself uses.
test: build
	$(CC) -Wall -Wextra -I. -o build/test_stage_chain ../tests/test_stage_chain.c \
		json.c pipeline.c stages/validate_stage.c stages/extract_stage.c stages/convert_stage.c \
		stages/dump_binary_stage.c stages/dump_text_stage.c stages/null_sink_stage.c
	./build/test_stage_chain
	$(CC) -Wall -Wextra -I. -o build/test_graph_config ../tests/test_graph_config.c \
		json.c pipeline.c graph_config.c ../tests/stub_stage_registry.c common.c \
		stages/validate_stage.c stages/extract_stage.c stages/convert_stage.c \
		stages/forward_udp_stage.c stages/dump_binary_stage.c stages/dump_text_stage.c
	./build/test_graph_config
	$(CC) -Wall -Wextra -I. -pthread -o build/test_epoch_barrier ../tests/test_epoch_barrier.c \
		epoch_barrier.c
	./build/test_epoch_barrier
	@mkdir -p build/test_plugin_fixtures_ok build/test_plugin_fixtures_collision
	$(CC) $(PLUGIN_CFLAGS) -shared -o build/test_plugin_fixtures_ok/plugin_ok.so \
		../tests/plugin_fixtures/plugin_ok.c
	$(CC) $(PLUGIN_CFLAGS) -shared -o build/test_plugin_fixtures_ok/plugin_bad_version.so \
		../tests/plugin_fixtures/plugin_bad_version.c
	$(CC) $(PLUGIN_CFLAGS) -shared -o build/test_plugin_fixtures_ok/plugin_missing_version.so \
		../tests/plugin_fixtures/plugin_missing_version.c
	$(CC) $(PLUGIN_CFLAGS) -shared -o build/test_plugin_fixtures_ok/plugin_loomlet.so \
		../tests/plugin_fixtures/plugin_loomlet.c
	$(CC) $(PLUGIN_CFLAGS) -shared -o build/test_plugin_fixtures_collision/plugin_dup_a.so \
		../tests/plugin_fixtures/plugin_dup_a.c
	$(CC) $(PLUGIN_CFLAGS) -shared -o build/test_plugin_fixtures_collision/plugin_dup_b.so \
		../tests/plugin_fixtures/plugin_dup_b.c
	$(CC) -Wall -Wextra -I. -o build/test_plugin_loader ../tests/test_plugin_loader.c \
		plugin_loader.c -ldl
	./build/test_plugin_loader
	$(CC) $(CFLAGS) -o build/test_pipeline_workers ../tests/test_pipeline_workers.c \
		pipeline.c pipeline_worker.c epoch_barrier.c ring_input.c \
		$(LDFLAGS) $(LDFLAGS_SHARED)
	./build/test_pipeline_workers

clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared build/test_stage_chain \
		build/test_graph_config build/test_epoch_barrier build/test_plugin_loader \
		build/test_pipeline_workers
	rm -rf build/test_plugin_fixtures_ok build/test_plugin_fixtures_collision
	rm -f $(PLUGINS_DIR)/*.so
	test -d build && rmdir -p build || true
