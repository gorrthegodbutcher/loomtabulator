#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include "forward_udp_stage.h"
#include "../common.h"

/* PORT_TYPE_ENGINEERING -> PORT_TYPE_WIRE_FRAME (and, unlike every other
 * stage, actually transmits - see this file's own header comment in
 * forward_udp_stage.h for why this is the one stage type with
 * process-wide DPDK context). v1 sends the engineering-units double as
 * an 8-byte big-endian UDP payload, one packet per record, no batching -
 * a known, deliberate v1 simplification (see the project plan's Phase 2
 * notes on multi-core batching); correctness first, throughput later. */

static struct rte_mempool *g_mbuf_pool;
static uint16_t g_port_id;
static uint8_t g_src_mac[6];

void
forward_udp_stage_set_runtime(struct rte_mempool *mbuf_pool, uint16_t port_id,
			       const uint8_t src_mac[6])
{
	g_mbuf_pool = mbuf_pool;
	g_port_id = port_id;
	memcpy(g_src_mac, src_mac, 6);
}

struct forward_config {
	uint8_t dst_mac[6];
	uint8_t src_ip[4];
	uint8_t dst_ip[4];
	uint16_t src_port;
	uint16_t dst_port;
	bool hw_checksum;
};

static int
parse_mac(const char *s, uint8_t out[6])
{
	unsigned int b[6];
	if (s == NULL || sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2],
				 &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		out[i] = (uint8_t)b[i];
	return 0;
}

static void
put_be64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--) {
		p[i] = (uint8_t)v;
		v >>= 8;
	}
}

void *
forward_udp_stage_init(const struct json_value *config)
{
	struct forward_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;

	const char *dst_mac_str = json_as_string(json_object_get(config, "dst_mac"), NULL);
	if (dst_mac_str == NULL || parse_mac(dst_mac_str, st->dst_mac) != 0) {
		free(st);
		return NULL;
	}

	const char *src_ip_str = json_as_string(json_object_get(config, "src_ip"), NULL);
	const char *dst_ip_str = json_as_string(json_object_get(config, "dst_ip"), NULL);
	if (src_ip_str == NULL || app_parse_ipv4(src_ip_str, st->src_ip) != 0 ||
	    dst_ip_str == NULL || app_parse_ipv4(dst_ip_str, st->dst_ip) != 0) {
		free(st);
		return NULL;
	}

	st->src_port = (uint16_t)json_as_number(json_object_get(config, "src_port"), 0);
	st->dst_port = (uint16_t)json_as_number(json_object_get(config, "dst_port"), 0);
	if (st->src_port == 0 || st->dst_port == 0) {
		free(st);
		return NULL;
	}
	st->hw_checksum = json_as_bool(json_object_get(config, "hw_checksum"), true);

	return st;
}

struct stage_result
forward_udp_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct forward_config *cfg = state;

	if (in->len != 8)
		return (struct stage_result){ .ok = false, .drop_reason = "expected 8-byte engineering value" };

	uint8_t payload[8];
	/* in->data holds a host-order double (see convert_stage.c) - the
	 * bit pattern, reinterpreted as a uint64_t, is what actually goes
	 * on the wire, big-endian, same as every other multi-byte field
	 * this project family ever puts on the wire. */
	uint64_t bits;
	memcpy(&bits, in->data, sizeof(bits));
	put_be64(payload, bits);

	uint32_t total_len = app_hdr_len(false) + sizeof(payload);
	if (app_build_packet(out->data, STAGE_SCRATCH_BYTES, total_len, cfg->dst_mac, g_src_mac,
			      cfg->src_ip, cfg->dst_ip, cfg->src_port, cfg->dst_port,
			      false /* with_seq - real telemetry traffic never carries
				     * dpdk-app-example's test-only sequence prefix */,
			      0, payload, sizeof(payload), cfg->hw_checksum) != 0)
		return (struct stage_result){ .ok = false, .drop_reason = "app_build_packet failed" };

	out->type = PORT_TYPE_WIRE_FRAME;
	out->len = total_len;
	out->capture_tsc = in->capture_tsc;

	struct rte_mbuf *m = rte_pktmbuf_alloc(g_mbuf_pool);
	if (m == NULL)
		return (struct stage_result){ .ok = false, .drop_reason = "mbuf pool exhausted" };

	uint8_t *mdata = (uint8_t *)rte_pktmbuf_append(m, total_len);
	if (mdata == NULL) {
		rte_pktmbuf_free(m);
		return (struct stage_result){ .ok = false, .drop_reason = "mbuf too small for frame" };
	}
	memcpy(mdata, out->data, total_len);

	if (cfg->hw_checksum) {
		m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
		m->l2_len = ETH_HDR_LEN;
		m->l3_len = IPV4_HDR_LEN;
		if (rte_eth_tx_prepare(g_port_id, 0, &m, 1) != 1) {
			rte_pktmbuf_free(m);
			return (struct stage_result){ .ok = false, .drop_reason = "tx_prepare failed" };
		}
	}

	if (rte_eth_tx_burst(g_port_id, 0, &m, 1) != 1) {
		rte_pktmbuf_free(m);
		return (struct stage_result){ .ok = false, .drop_reason = "tx_burst dropped the packet" };
	}

	return (struct stage_result){ .ok = true };
}

void
forward_udp_stage_teardown(void *state)
{
	free(state);
}
