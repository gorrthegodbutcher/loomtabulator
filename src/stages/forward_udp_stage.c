#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "forward_udp_stage.h"
#include "../common.h"

/* PORT_TYPE_ENGINEERING -> PORT_TYPE_WIRE_FRAME (and, unlike every other
 * stage, actually transmits). v1 sends the engineering-units double as
 * an 8-byte big-endian UDP payload, one packet per record, no batching -
 * a known, deliberate v1 simplification (see the project plan's Phase 2
 * notes on multi-core batching); correctness first, throughput later.
 *
 * A plain kernel UDP socket, not DPDK TX: the output/forward mechanism
 * was always flagged as a genuinely open decision (see CLAUDE.md's
 * history), and nothing about it actually needed a dedicated
 * DPDK-bound NIC port - this project's *input* side reads off a DPDK
 * rte_ring (chrontabulator's real shared-memory ring in Phase 4), but
 * the destination here is just another host/container/process on the
 * network (or the same box), which the kernel's own IP stack already
 * knows how to reach without this process owning any hardware. This
 * also resolves the one concurrency gap CLAUDE.md flagged from Phase 2
 * (concurrent rte_eth_tx_burst() calls on one TX queue with no locking,
 * once multiple workers could call it): POSIX guarantees sendto() on a
 * shared socket fd from multiple threads is safe, and each call is
 * atomic with respect to the datagram's own contents - no locking
 * needed here, unlike the DPDK TX path this replaces.
 *
 * Each stage instance owns its own socket fd (init()/teardown()) -
 * there's no process-wide runtime context to set up first, unlike the
 * DPDK version this replaces (no shared mbuf pool, no bound port id),
 * so this now follows the exact same per-instance contract every other
 * stage type already does. */

struct forward_config {
	int sock_fd;
	struct sockaddr_in dst_addr;
};

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

	const char *dst_ip_str = json_as_string(json_object_get(config, "dst_ip"), NULL);
	uint8_t dst_ip[4];
	if (dst_ip_str == NULL || app_parse_ipv4(dst_ip_str, dst_ip) != 0) {
		free(st);
		return NULL;
	}

	uint16_t dst_port = (uint16_t)json_as_number(json_object_get(config, "dst_port"), 0);
	if (dst_port == 0) {
		free(st);
		return NULL;
	}

	uint16_t src_port = (uint16_t)json_as_number(json_object_get(config, "src_port"), 0);
	const char *src_ip_str = json_as_string(json_object_get(config, "src_ip"), NULL);
	uint8_t src_ip[4] = {0};
	if (src_ip_str != NULL && app_parse_ipv4(src_ip_str, src_ip) != 0) {
		free(st);
		return NULL;
	}

	st->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (st->sock_fd < 0) {
		free(st);
		return NULL;
	}

	/* Only bind if the graph actually asked for a specific source port
	 * or address (multi-homed host, or matching a receiver's expected
	 * source) - otherwise let the kernel pick an ephemeral port and the
	 * default route's own address, same as any ordinary UDP client. */
	if (src_port != 0 || src_ip_str != NULL) {
		struct sockaddr_in src_addr = {
			.sin_family = AF_INET,
			.sin_port = htons(src_port),
		};
		memcpy(&src_addr.sin_addr, src_ip, sizeof(src_ip));
		if (bind(st->sock_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) != 0) {
			close(st->sock_fd);
			free(st);
			return NULL;
		}
	}

	st->dst_addr.sin_family = AF_INET;
	memcpy(&st->dst_addr.sin_addr, dst_ip, sizeof(dst_ip));
	st->dst_addr.sin_port = htons(dst_port);

	return st;
}

struct stage_result
forward_udp_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct forward_config *cfg = state;

	if (in->len != 8)
		return (struct stage_result){ .ok = false, .drop_reason = "expected 8-byte engineering value" };

	/* in->data holds a host-order double (see convert_stage.c) - the
	 * bit pattern, reinterpreted as a uint64_t, is what actually goes
	 * on the wire, big-endian, same as every other multi-byte field
	 * this project family ever puts on the wire. */
	uint8_t payload[8];
	uint64_t bits;
	memcpy(&bits, in->data, sizeof(bits));
	put_be64(payload, bits);

	memcpy(out->data, payload, sizeof(payload));
	out->type = PORT_TYPE_WIRE_FRAME;
	out->len = sizeof(payload);
	out->capture_tsc = in->capture_tsc;

	ssize_t sent = sendto(cfg->sock_fd, payload, sizeof(payload), 0,
			       (struct sockaddr *)&cfg->dst_addr, sizeof(cfg->dst_addr));
	if (sent != (ssize_t)sizeof(payload))
		return (struct stage_result){ .ok = false, .drop_reason = "sendto() failed" };

	return (struct stage_result){ .ok = true };
}

void
forward_udp_stage_teardown(void *state)
{
	struct forward_config *cfg = state;
	if (cfg != NULL)
		close(cfg->sock_fd);
	free(cfg);
}
