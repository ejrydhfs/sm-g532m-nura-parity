// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 Ethernet netdev.
 *
 * The WLAN function presents itself as a plain Ethernet interface.  RX: the
 * firmware normally decapsulates data frames to 802.3 and the buffer is fed
 * straight to eth_type_trans() (os/linux/gl_kal.c:1509-1612
 * kalRxIndicatePkts); 802.11 format buffers are converted in wlan_txrx.c.
 * TX: the driver builds the 802.11 data frame itself (wlan_txrx.c), because
 * this port has no firmware STA record and therefore cannot ask the
 * firmware to convert an 802.3 frame.
 *
 * Ported from Samsung/MTK downstream:
 *   os/linux/gl_init.c        (netdev allocation, registration, netdev ops)
 *   os/linux/gl_kal.c         (kalRxIndicatePkts)
 *   os/linux/gl_hook_api.c    (interface state glue)
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>

#include "wlan_mac.h"

/**
 * mtk_wlan_netdev_rx - hand a received Ethernet frame to the stack
 * @mac: MAC instance
 * @skb: Ethernet frame starting at the destination address
 *
 * Frames are delivered with netif_rx() because the HIF drain runs in a
 * workqueue (process context), not in the interrupt handler.
 */
void mtk_wlan_netdev_rx(struct mtk_wlan_mac *mac, struct sk_buff *skb)
{
	struct net_device *ndev = mac->ndev;
	unsigned int len = skb->len;

	if (!ndev || !READ_ONCE(mac->registered) || !netif_running(ndev)) {
		atomic_inc(&mac->rx_dropped);
		dev_kfree_skb(skb);
		return;
	}

	skb->dev = ndev;
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;

	if (netif_rx(skb) == NET_RX_DROP) {
		ndev->stats.rx_dropped++;
		atomic_inc(&mac->rx_dropped);
	}
}

/*
 * Transmit.  The frame cannot be written to the HIF from this context
 * because the PDMA path takes a mutex, so it is queued and drained by
 * &mtk_wlan_mac.tx_work (the downstream driver has a dedicated TX thread for
 * the same reason, nic_tx.c:916-960 nicTxMsduInfoList).
 */
static netdev_tx_t mtk_wlan_netdev_start_xmit(struct sk_buff *skb,
					      struct net_device *ndev)
{
	struct mtk_wlan_mac *mac = netdev_priv(ndev);

	if (!READ_ONCE(mac->fw_up) || !READ_ONCE(mac->connected)) {
		atomic_inc(&mac->tx_dropped);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	skb_queue_tail(&mac->txq, skb);
	schedule_work(&mac->tx_work);

	if (skb_queue_len(&mac->txq) >= MTK_WLAN_TXQ_DEPTH)
		netif_tx_stop_all_queues(ndev);

	return NETDEV_TX_OK;
}

static int mtk_wlan_netdev_open(struct net_device *ndev)
{
	struct mtk_wlan_mac *mac = netdev_priv(ndev);

	if (!READ_ONCE(mac->fw_up))
		return -ENETDOWN;

	if (READ_ONCE(mac->connected)) {
		netif_carrier_on(ndev);
		netif_tx_start_all_queues(ndev);
	} else {
		netif_carrier_off(ndev);
		netif_tx_stop_all_queues(ndev);
	}

	return 0;
}

static int mtk_wlan_netdev_stop(struct net_device *ndev)
{
	struct mtk_wlan_mac *mac = netdev_priv(ndev);
	struct sk_buff *skb;

	netif_tx_stop_all_queues(ndev);
	netif_carrier_off(ndev);

	/* Frames still queued belong to this interface: drop them. */
	while ((skb = skb_dequeue(&mac->txq)))
		dev_kfree_skb(skb);

	return 0;
}

static void mtk_wlan_netdev_set_multicast(struct net_device *ndev)
{
	/*
	 * The firmware has no host-visible multicast filter register in this
	 * HIF (the MCR set in mtreg.h:81-143 has none) and the packet filter
	 * OID that downstream uses here (wlanoidSetCurrentPacketFilter() from
	 * wlanSetMulticastList(), gl_init.c:1698-1770) needs the firmware
	 * command queue this port does not implement.  The receiver keeps its
	 * firmware default, so there is nothing to program; the list is
	 * accepted for the interface to come up.
	 */
}

static const struct net_device_ops mtk_wlan_netdev_ops = {
	.ndo_open		= mtk_wlan_netdev_open,
	.ndo_stop		= mtk_wlan_netdev_stop,
	.ndo_start_xmit		= mtk_wlan_netdev_start_xmit,
	.ndo_set_rx_mode	= mtk_wlan_netdev_set_multicast,
};

/**
 * mtk_wlan_netdev_create - allocate the Ethernet netdev and its wireless_dev
 * @mac: MAC instance
 *
 * Return: 0 on success, negative errno on failure.
 */
int mtk_wlan_netdev_create(struct mtk_wlan_mac *mac)
{
	struct wireless_dev *wdev;
	struct net_device *ndev;

	ndev = alloc_netdev(sizeof(*wdev), "wlan%d", NET_NAME_ENUM, ether_setup);
	if (!ndev)
		return -ENOMEM;

	ndev->netdev_ops = &mtk_wlan_netdev_ops;
	SET_NETDEV_DEV(ndev, mac->dev);

	eth_hw_addr_set(ndev, mac->mac_addr);
	ndev->addr_len = ETH_ALEN;
	if (!is_valid_ether_addr(ndev->dev_addr)) {
		eth_hw_addr_random(ndev);
		ether_addr_copy(mac->mac_addr, ndev->dev_addr);
		dev_info(mac->dev,
			 "no MAC address in the device tree, using random %pM\n",
			 ndev->dev_addr);
	}

	netif_carrier_off(ndev);
	netif_tx_stop_all_queues(ndev);

	wdev = netdev_priv(ndev);
	wdev->wiphy = mac->wiphy;
	wdev->iftype = NL80211_IFTYPE_STATION;
	wdev->netdev = ndev;
	ndev->ieee80211_ptr = wdev;

	mac->ndev = ndev;
	mac->wdev = wdev;

	return 0;
}

/**
 * mtk_wlan_netdev_register - publish the netdev while the firmware runs
 * @mac: MAC instance
 *
 * Return: 0 on success, negative errno on failure.
 */
int mtk_wlan_netdev_register(struct mtk_wlan_mac *mac)
{
	int ret;

	if (READ_ONCE(mac->registered))
		return 0;

	/* cfg80211 picks the wireless_dev up from NETDEV_REGISTER. */
	ret = register_netdevice(mac->ndev);
	if (ret) {
		dev_err(mac->dev, "cannot register %s: %d\n",
			mac->ndev->name, ret);
		return ret;
	}

	WRITE_ONCE(mac->registered, true);
	dev_info(mac->dev, "%s registered, address %pM\n", mac->ndev->name,
		 mac->ndev->dev_addr);

	return 0;
}

/*
 * Drop the netdev.  unregister_netdevice() closes it first, i.e. ndo_stop()
 * runs while the cfg80211 wdev is still registered.
 */
void mtk_wlan_netdev_unregister(struct mtk_wlan_mac *mac)
{
	if (!READ_ONCE(mac->registered))
		return;

	unregister_netdevice(mac->ndev);
	WRITE_ONCE(mac->registered, false);
}

void mtk_wlan_netdev_destroy(struct mtk_wlan_mac *mac)
{
	if (!mac->ndev)
		return;

	free_netdev(mac->ndev);
	mac->ndev = NULL;
	mac->wdev = NULL;
}

/**
 * mtk_wlan_netdev_link_changed - reflect the connection state on the netdev
 * @mac: MAC instance
 * @up: %true when the STA is associated
 */
void mtk_wlan_netdev_link_changed(struct mtk_wlan_mac *mac, bool up)
{
	struct net_device *ndev = mac->ndev;

	if (!ndev || !READ_ONCE(mac->registered))
		return;

	if (up) {
		netif_carrier_on(ndev);
		if (netif_running(ndev))
			netif_tx_wake_all_queues(ndev);
	} else {
		netif_tx_stop_all_queues(ndev);
		netif_carrier_off(ndev);
	}
}

/* Wake the TX queue after the firmware returned buffers. */
void mtk_wlan_netdev_wake_tx(struct mtk_wlan_mac *mac)
{
	struct net_device *ndev = mac->ndev;

	if (!ndev || !READ_ONCE(mac->registered) ||
	    !READ_ONCE(mac->connected))
		return;

	if (!netif_running(ndev))
		return;

	netif_tx_wake_all_queues(ndev);
}
