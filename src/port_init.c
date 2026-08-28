/* Copied verbatim from dpdk-app-example's src/port_init.c - see
 * port_init.h's own note. */
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include "port_init.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define MAX_ATTEMPTS 2

int
app_port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint16_t mtu,
	      bool reset_and_retry, bool force_10g,
	      struct port_init_result *result)
{
	int reset_attempts = 0;
	int reset_failures = 0;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
		struct rte_eth_conf port_conf;
		uint16_t nb_rxd = RX_RING_SIZE;
		uint16_t nb_txd = TX_RING_SIZE;
		int retval;
		struct rte_eth_dev_info dev_info;
		struct rte_eth_txconf txconf;

		memset(&port_conf, 0, sizeof(port_conf));

		if (force_10g)
			port_conf.link_speeds = RTE_ETH_LINK_SPEED_10G;

		retval = rte_eth_dev_info_get(port, &dev_info);
		if (retval != 0) {
			fprintf(stderr, "Error getting device (port %u) info: %s\n",
				port, strerror(-retval));
			return retval;
		}

		if (mtu != 0) {
			if (mtu < dev_info.min_mtu || mtu > dev_info.max_mtu) {
				fprintf(stderr, "Requested MTU %u outside device range [%u, %u]\n",
					mtu, dev_info.min_mtu, dev_info.max_mtu);
				return -1;
			}
			port_conf.rxmode.mtu = mtu;

			/* Some PMDs need this explicitly requested to properly
			 * receive frames above the standard 1500-byte MTU, even if
			 * a single mbuf's buffer is technically large enough to
			 * hold one - setting rxmode.mtu alone isn't always enough. */
			if (mtu > RTE_ETHER_MTU && (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_SCATTER))
				port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_SCATTER;

			/* DPDK's own jumbo-frame test plan calls this out as required
			 * for jumbo TX on some PMDs (atlantic included) - without it,
			 * frames above the standard MTU get accepted into the TX ring
			 * but hardware transmission never actually completes. */
			if (mtu > RTE_ETHER_MTU && (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS))
				port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
		}

		if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
			port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

		/* IPv4 and UDP checksum offload - enabled here whenever the
		 * device supports both (regardless of sender/receiver mode;
		 * this function doesn't distinguish, and a receiver simply
		 * never sets the per-mbuf flags that would make use of it).
		 * Enabling the capability doesn't force anything - individual
		 * packets only get hardware-computed checksums if the caller
		 * sets RTE_MBUF_F_TX_IP_CKSUM/_UDP_CKSUM on that mbuf (see
		 * sender.c/interactive_sender.c's hw_checksum handling) and
		 * calls rte_eth_tx_prepare() before rte_eth_tx_burst(). */
		bool tx_checksum_capable =
			(dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) &&
			(dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM);
		if (tx_checksum_capable)
			port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
						      RTE_ETH_TX_OFFLOAD_UDP_CKSUM;

		retval = rte_eth_dev_configure(port, 1, 1, &port_conf);
		if (retval != 0)
			return retval;

		retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
		if (retval != 0)
			return retval;

		retval = rte_eth_rx_queue_setup(port, 0, nb_rxd,
						 rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;

		txconf = dev_info.default_txconf;
		txconf.offloads = port_conf.txmode.offloads;
		retval = rte_eth_tx_queue_setup(port, 0, nb_txd,
						 rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;

		retval = rte_eth_dev_start(port);
		if (retval < 0)
			return retval;

		struct rte_ether_addr addr;
		retval = rte_eth_macaddr_get(port, &addr);
		if (retval != 0)
			return retval;

		uint16_t actual_mtu;
		rte_eth_dev_get_mtu(port, &actual_mtu);

		/* rte_eth_link_get() is documented as blocking until link training
		 * settles, but at least the atlantic PMD returns an immediate DOWN
		 * reading instead - autonegotiation on this hardware genuinely takes
		 * a few seconds, so poll for it ourselves rather than trust that. */
		struct rte_eth_link link;
		const uint32_t max_wait_ms = 10000, poll_interval_ms = 200;
		uint32_t waited_ms = 0;
		do {
			(void)rte_eth_link_get_nowait(port, &link);
			if (link.link_status == RTE_ETH_LINK_UP)
				break;
			rte_delay_us(poll_interval_ms * 1000);
			waited_ms += poll_interval_ms;
		} while (waited_ms < max_wait_ms);

		if (link.link_status != RTE_ETH_LINK_UP && reset_and_retry &&
		    attempt < MAX_ATTEMPTS) {
			/* atl_dev_stop()'s internal hardware reset call discards
			 * its own return value, so a failed reset there is
			 * invisible to us. rte_eth_dev_reset() drives the same
			 * underlying reset through a path that does check it
			 * (the same one rte_eth_dev_start()/probe use) - if the
			 * card is genuinely stuck, this is the tool that would
			 * actually show us why, and gives the hardware a real
			 * close+reinit cycle instead of just trying again on
			 * top of whatever state it was already in. */
			reset_attempts++;
			int reset_ret = rte_eth_dev_reset(port);
			if (reset_ret != 0) {
				reset_failures++;
				fprintf(stderr, "Port %u rte_eth_dev_reset failed: %s\n",
					port, rte_strerror(-reset_ret));
			}
			continue;
		}

		/* We're not ARP-resolved, so incoming frames may be addressed to
		 * broadcast or a peer MAC that isn't ours - promiscuous mode is
		 * required to see them at all. */
		retval = rte_eth_promiscuous_enable(port);
		if (retval != 0)
			return retval;

		if (result != NULL) {
			result->mac_addr = addr;
			result->actual_mtu = actual_mtu;
			result->link_up = (link.link_status == RTE_ETH_LINK_UP);
			result->link_speed_mbps = link.link_speed;
			result->full_duplex = (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX);
			result->link_wait_ms = waited_ms;
			result->attempts_used = attempt;
			result->reset_attempts = reset_attempts;
			result->reset_failures = reset_failures;
			result->tx_checksum_capable = tx_checksum_capable;
		}

		return 0;
	}

	return 0;
}

bool
app_module_present(uint16_t port)
{
	uint8_t buf[16];
	struct rte_dev_eeprom_info info = {
		.data = buf,
		.offset = 0,
		.length = sizeof(buf),
		.magic = 0,
	};

	return rte_eth_dev_get_eeprom(port, &info) == 0;
}
