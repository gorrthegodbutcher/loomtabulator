# SPDX-License-Identifier: BSD-3-Clause

APP = loomtabulator

SRCS-y := main.c common.c port_init.c ring_input.c json.c pipeline.c graph_config.c \
	  stage_registry.c testgen.c web_status.c epoch_barrier.c pipeline_worker.c \
	  stages/validate_stage.c stages/extract_stage.c stages/convert_stage.c \
	  stages/forward_udp_stage.c

PKGCONF ?= pkg-config

ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

all: shared

.PHONY: shared static test clean

shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O3 -Wall -Wextra -pthread -I. $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = -pthread $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = -pthread $(shell $(PKGCONF) --static --libs libdpdk)

CFLAGS += -DALLOW_EXPERIMENTAL_API

build/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

build:
	@mkdir -p $@

# Four tiers, matching dpdk-app-example's own test convention:
#  - test_stage_chain: validate/extract/convert are deliberately mbuf-free
#    (see stage.h) - linked here with zero DPDK involvement, runs as a
#    plain host binary, no hugepages/EAL needed.
#  - test_graph_config: graph_config.c pulls in stage_registry.c, which
#    (for its forward_udp table entry) pulls in forward_udp_stage.c and
#    therefore DPDK's headers/shared libs - but the test never actually
#    calls into EAL-dependent code (only forward_udp's process(), never
#    exercised here, touches rte_mbuf/rte_eth_tx_burst), so it still runs
#    as a plain process with no special EAL flags, just linked against
#    DPDK's .so files.
#  - test_epoch_barrier (Phase 2): epoch_barrier.c is deliberately
#    DPDK-free (see epoch_barrier.h) - real pthreads, zero EAL
#    involvement, same posture as test_stage_chain.
#  - test_pipeline_workers (Phase 2): the epoch_barrier/pipeline_worker
#    integration test against a real rte_ring - needs EAL init
#    (--no-huge --no-pci, no port/mbuf-pool setup at all) but no
#    hardware, same "no hardware needed" posture as the project plan's
#    v1 datapath smoke test.
test: build
	$(CC) -Wall -Wextra -I. -o build/test_stage_chain ../tests/test_stage_chain.c \
		json.c stages/validate_stage.c stages/extract_stage.c stages/convert_stage.c
	./build/test_stage_chain
	$(CC) $(CFLAGS) -o build/test_graph_config ../tests/test_graph_config.c \
		json.c pipeline.c graph_config.c stage_registry.c common.c port_init.c \
		stages/validate_stage.c stages/extract_stage.c stages/convert_stage.c \
		stages/forward_udp_stage.c $(LDFLAGS) $(LDFLAGS_SHARED)
	./build/test_graph_config
	$(CC) -Wall -Wextra -I. -pthread -o build/test_epoch_barrier ../tests/test_epoch_barrier.c \
		epoch_barrier.c
	./build/test_epoch_barrier
	$(CC) $(CFLAGS) -o build/test_pipeline_workers ../tests/test_pipeline_workers.c \
		pipeline.c pipeline_worker.c epoch_barrier.c ring_input.c \
		$(LDFLAGS) $(LDFLAGS_SHARED)
	./build/test_pipeline_workers

clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared build/test_stage_chain \
		build/test_graph_config build/test_epoch_barrier build/test_pipeline_workers
	test -d build && rmdir -p build || true
