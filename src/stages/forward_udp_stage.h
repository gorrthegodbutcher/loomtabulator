#ifndef FORWARD_UDP_STAGE_H
#define FORWARD_UDP_STAGE_H

#include "../stage.h"

/* raw_record/validated/extracted -> PORT_TYPE_WIRE_FRAME - a terminal
 * stage (one of two now - see dump_binary_stage.h/dump_text_stage.h
 * for the file-writing alternatives). Transmits via a plain kernel UDP
 * socket, one per
 * stage instance (created in init(), closed in teardown()) - unlike an
 * earlier DPDK-NIC-based version of this file, there's no process-wide
 * runtime context to set up before graph_config_load() (no shared mbuf
 * pool, no bound NIC port id), so this stage now follows exactly the
 * same init(config)/process()/teardown() contract every other stage
 * type does. See this file's own .c for why a kernel socket instead of
 * DPDK TX - this project's input side still reads off a DPDK rte_ring
 * (and will read chrontabulator's real shared-memory ring in Phase 4),
 * but nothing about the *output* side ever needed a dedicated
 * DPDK-bound NIC port; the destination is just another host/container/
 * process on the network. */
void *forward_udp_stage_init(const struct json_value *config);
struct stage_result forward_udp_stage_process(void *state, const struct stage_record *in,
					       struct stage_record *out);
void forward_udp_stage_teardown(void *state);

#endif
