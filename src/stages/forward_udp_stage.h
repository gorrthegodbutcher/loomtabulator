#ifndef FORWARD_UDP_STAGE_H
#define FORWARD_UDP_STAGE_H

#include <rte_mempool.h>
#include "../stage.h"

/* forward_udp is the one stage type in this whole pipeline that touches
 * DPDK/hardware directly (every other stage type is deliberately
 * mbuf-free - see stage.h's header comment) - it's what actually builds
 * and transmits the outbound UDP frame. Because of that, it needs
 * process-wide context (the mbuf pool, the NIC port id) that the
 * generic stage.h init(config) contract has no way to pass through -
 * every other stage's state is fully determined by its own graph JSON
 * config block alone. main.c calls this once, after port bring-up and
 * before graph_config_load()/pipeline_build(), same shape as
 * chrontabulator's own g_ctx singleton being set up before its reactor
 * starts. */
void forward_udp_stage_set_runtime(struct rte_mempool *mbuf_pool, uint16_t port_id,
				    const uint8_t src_mac[6]);

void *forward_udp_stage_init(const struct json_value *config);
struct stage_result forward_udp_stage_process(void *state, const struct stage_record *in,
					       struct stage_record *out);
void forward_udp_stage_teardown(void *state);

#endif
