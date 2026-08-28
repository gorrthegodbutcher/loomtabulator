/* Copied verbatim from dpdk-app-example's src/port_init.h - the
 * forward_udp stage's NIC bring-up is identical to that project's, no
 * reason to reinvent it. Keep in sync by hand if it changes there. */
#ifndef PORT_INIT_H
#define PORT_INIT_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_ether.h>
#include <rte_mempool.h>

/* Silent record of what app_port_init() actually did, for the caller to
 * fold into a summary printed once at the very end - nothing in this
 * path prints anything itself except genuine DPDK function errors. */
struct port_init_result {
	struct rte_ether_addr mac_addr;
	uint16_t actual_mtu;
	bool link_up;
	uint32_t link_speed_mbps;
	bool full_duplex;
	uint32_t link_wait_ms;
	int attempts_used;
	int reset_attempts;
	int reset_failures;
	bool tx_checksum_capable; /* device advertises RTE_ETH_TX_OFFLOAD_
				    * IPV4_CKSUM and _UDP_CKSUM - if false,
				    * hw_checksum requests silently have no
				    * hardware behind them (rte_eth_tx_prepare()
				    * degrades to whatever the driver's default
				    * no-op prep does; the checksum fields
				    * app_build_packet() left at 0 for hw_checksum
				    * would go out on the wire literally zero,
				    * which is only valid per RFC 768's "not
				    * computed" exception - see common.c's own
				    * comment on why this project avoids relying
				    * on that in the first place). */
};

/* Configures port for 1 RX + 1 TX queue, starts it, and enables
 * promiscuous mode (needed since we're not ARP-resolved - traffic may
 * arrive addressed to broadcast or a peer's MAC we don't own).
 *
 * mtu: requested MTU in bytes, or 0 to leave it at the DPDK/device
 * default (standard Ethernet, 1500). Frames larger than the configured
 * MTU are dropped by the NIC before your app ever sees them - this is
 * NOT about mbuf buffer size (the default mbuf pool already comfortably
 * fits a "slightly jumbo" frame; this is a real hardware/driver-level
 * cutoff, separate from that).
 *
 * reset_and_retry: if the link is still DOWN after the normal wait
 * window, call rte_eth_dev_reset() (a full close + hardware reset +
 * reinit, exercising a driver code path that properly surfaces reset
 * failures - unlike the plain rte_eth_dev_stop() teardown, which the
 * atlantic PMD's atl_dev_stop() discards the result of internally) and
 * redo the entire bring-up sequence once before giving up.
 *
 * force_10g: restrict the advertised link speed to 10G only, instead of
 * the driver's default of advertising the full 100M-10G ladder. Doesn't
 * use DPDK's RTE_ETH_LINK_SPEED_FIXED (atl_dev_start() explicitly
 * rejects that on this PMD) - just sets link_speeds to the single 10G
 * bit, which the driver's own atl_dev_set_link_up() turns into a
 * firmware speed mask containing only that one rate. Useful on hardware
 * that's physically only capable of one speed anyway (e.g. a 10G-only
 * fiber transceiver), to rule out autonegotiation itself as a factor.
 *
 * result: filled in on success (ignored if NULL) with what happened,
 * for the caller to report later - see struct port_init_result above.
 *
 * Returns 0 on success. Adapted from DPDK's examples/skeleton/basicfwd.c.
 * Nothing in this call prints anything except a genuine DPDK function
 * error (a nonzero/negative return from an rte_* call), to stderr. */
int app_port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint16_t mtu,
		   bool reset_and_retry, bool force_10g,
		   struct port_init_result *result);

/* Checks whether the SFP+ transceiver is physically present, by attempting
 * a real read of its management EEPROM (rte_eth_dev_get_eeprom(), backed on
 * the atlantic PMD by an SMBus read to the module itself). Confirmed
 * empirically (see project memory) that this succeeds/fails based on the
 * module's physical presence specifically, independent of link/fiber
 * state - a module that's seated but has no fiber connected still reads
 * fine; only physically removing the module makes the read fail.
 *
 * Not a standard "is a module inserted" API - DPDK's own module-EEPROM op
 * (rte_eth_dev_get_module_eeprom()) isn't implemented by this PMD, so this
 * repurposes the plain EEPROM read that is. Returns true if the read
 * succeeds (module present), false otherwise (module absent, or any other
 * read failure - the two aren't distinguishable through this API). */
bool app_module_present(uint16_t port);

#endif
