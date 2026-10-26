// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 STA management: software scan and open-system
 * authentication/association.
 *
 * Ported from Samsung/MTK downstream:
 *   mgmt/auth.c          (authComposeAuthFrameHeaderAndFF, authSendAuthFrame,
 *                         authSendDeauthFrame)
 *   mgmt/assoc.c         (assocComposeReAssocReqFrameHeaderAndFF,
 *                         assocBuildReAssocReqFrameCommonIEs,
 *                         assocBuildCapabilityInfo)
 *   mgmt/ais_fsm.c       (AIS auth/assoc state machine, deauth on disconnect)
 *   mgmt/scan.c          (BSS result parsing from beacons/probe responses,
 *                         scanUpdateBssDesc(): trust the DS/HT channel IE)
 *   mgmt/scan_fsm.c      (scan request handling; downstream sends
 *                         CMD_ID_SCAN_REQ to the firmware, which this port
 *                         cannot do)
 *   os/linux/gl_kal.c    (kalIndicateStatusAndComplete: connect/disconnect
 *                         notifications to cfg80211)
 *
 * The downstream driver lets the firmware run the scan and keeps the
 * firmware STA record in sync over CMD_ID_UPDATE_STA_RECORD and
 * CMD_ID_SET_BSS_INFO; this port drives the same management frames from the
 * host and has no firmware command queue, see the report for the resulting
 * boundary.
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "wlan_cmd.h"
#include "wlan_mac.h"

/* 2.4 GHz basic rate set advertised in probe/assoc requests (500 kb/s). */
static const u8 mtk_wlan_basic_rates[] = {
	MTK_WLAN_RATE_1M | MTK_WLAN_RATE_BASIC_BIT,
	MTK_WLAN_RATE_2M | MTK_WLAN_RATE_BASIC_BIT,
	MTK_WLAN_RATE_5_5M | MTK_WLAN_RATE_BASIC_BIT,
	MTK_WLAN_RATE_11M | MTK_WLAN_RATE_BASIC_BIT,
};

/* OFDM rates moved to the Extended Supported Rates element (8 octets). */
static const u8 mtk_wlan_ofdm_rates[] = {
	MTK_WLAN_RATE_6M,
	MTK_WLAN_RATE_9M,
	MTK_WLAN_RATE_12M,
	MTK_WLAN_RATE_18M,
	MTK_WLAN_RATE_24M,
	MTK_WLAN_RATE_36M,
	MTK_WLAN_RATE_48M,
	MTK_WLAN_RATE_54M,
};

/* Append one information element, returns the number of bytes written. */
static size_t mtk_wlan_ie_put(u8 *buf, size_t off, u8 id, const void *data,
			      size_t len)
{
	buf[off] = id;
	buf[off + 1] = (u8)len;
	if (len)
		memcpy(buf + off + 2, data, len);
	return len + 2;
}

/*
 * Build a probe request for one channel and one SSID.  The element set
 * (SSID, supported rates, DS parameter set, extended supported rates,
 * followed by the IEs requested by cfg80211) follows 802.11-2016 9.3.3.9;
 * the DS element is mandatory in a probe request and tells the AP which
 * channel the STA is probing (ELEM_ID_DS_PARAMS, mac.h:760).
 */
static int mtk_wlan_build_probe_req(struct mtk_wlan_mac *mac, u8 channel,
				    const u8 *ssid, size_t ssid_len,
				    const u8 *extra_ie, size_t extra_ie_len,
				    u8 *buf, size_t bufsz)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)buf;
	size_t len = MTK_WLAN_MAC_HDR_LEN;
	u8 ds = channel;

	if (MTK_WLAN_MAC_HDR_LEN + 2 + ssid_len + 2 + sizeof(mtk_wlan_basic_rates) +
	    2 + 1 + 2 + sizeof(mtk_wlan_ofdm_rates) + extra_ie_len > bufsz)
		return -EMSGSIZE;
	if (ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_PROBE_REQ);
	mgmt->duration = 0;
	eth_broadcast_addr(mgmt->da);
	ether_addr_copy(mgmt->sa, mac->mac_addr);
	eth_broadcast_addr(mgmt->bssid);
	mgmt->seq_ctrl = 0;

	len += mtk_wlan_ie_put(buf, len, WLAN_EID_SSID, ssid, ssid_len);
	len += mtk_wlan_ie_put(buf, len, WLAN_EID_SUPP_RATES,
			       mtk_wlan_basic_rates,
			       sizeof(mtk_wlan_basic_rates));
	len += mtk_wlan_ie_put(buf, len, WLAN_EID_DS_PARAMS, &ds, sizeof(ds));
	len += mtk_wlan_ie_put(buf, len, WLAN_EID_EXT_SUPP_RATES,
			       mtk_wlan_ofdm_rates,
			       sizeof(mtk_wlan_ofdm_rates));
	if (extra_ie_len) {
		memcpy(buf + len, extra_ie, extra_ie_len);
		len += extra_ie_len;
	}

	return len;
}

/* Scan channels are restricted to the 2.4 GHz band this chip supports. */
static bool mtk_wlan_channel_usable(const struct ieee80211_channel *chan)
{
	return chan->band == NL80211_BAND_2GHZ &&
	       !(chan->flags & IEEE80211_CHAN_DISABLED);
}

/*
 * Dwell time for one channel.  cfg80211 reports it in TUs (1 TU = 1024 us,
 * cfg80211.h:2860-2864) when the scan request sets it.  The value is bounded
 * so a malformed request cannot park a scan for minutes; the bound is a
 * driver policy, not a hardware limit.
 */
static unsigned int mtk_wlan_scan_dwell_ms(const struct cfg80211_scan_request *req)
{
	u64 us;

	if (!req->duration)
		return MTK_WLAN_SCAN_DWELL_MS;

	us = (u64)req->duration * 1024;
	return clamp_t(u64, DIV_ROUND_UP_ULL(us, 1000), 1,
		       MTK_WLAN_SCAN_DWELL_MAX_MS);
}

/*
 * Wait for the dwell time, polling the abort flag and the firmware state.
 * Return: %false when the scan was cancelled or the firmware went away.
 */
static bool mtk_wlan_scan_wait(struct mtk_wlan_mac *mac, unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);

	do {
		if (READ_ONCE(mac->scan_aborted) || !READ_ONCE(mac->fw_up))
			return false;
		msleep(MTK_WLAN_SCAN_POLL_MS);
	} while (time_before(jiffies, end));

	return !READ_ONCE(mac->scan_aborted) && READ_ONCE(mac->fw_up);
}

/**
 * mtk_wlan_mgmt_scan - start a software scan
 * @mac: MAC instance
 * @req: cfg80211 scan request, valid until cfg80211_scan_done()
 *
 * Return: 0 when the scan was started, negative errno otherwise.
 */
int mtk_wlan_mgmt_scan(struct mtk_wlan_mac *mac,
		       struct cfg80211_scan_request *req)
{
	if (!READ_ONCE(mac->fw_up))
		return -ENETDOWN;
	if (READ_ONCE(mac->state) == MTK_WLAN_STATE_CONNECTING)
		return -EBUSY;
	if (!req->n_channels)
		return -EINVAL;
	if (READ_ONCE(mac->scan_req))
		return -EBUSY;

	WRITE_ONCE(mac->scan_aborted, false);
	WRITE_ONCE(mac->scan_req, req);
	WRITE_ONCE(mac->state, MTK_WLAN_STATE_SCANNING);
	schedule_work(&mac->scan_work);

	return 0;
}

/**
 * mtk_wlan_mgmt_abort_scan - cancel a running scan
 * @mac: MAC instance
 *
 * The worker notices the flag within one dwell poll, stops probing and
 * reports the scan as aborted through cfg80211_scan_done(), as the cfg80211
 * abort_scan callback requires.  Completion is always done by the worker, so
 * the request is signalled exactly once.
 */
void mtk_wlan_mgmt_abort_scan(struct mtk_wlan_mac *mac)
{
	WRITE_ONCE(mac->scan_aborted, true);
}

/* Report a scan completion exactly once, from the worker or its fallback. */
static void mtk_wlan_scan_complete(struct mtk_wlan_mac *mac,
				   struct cfg80211_scan_request *req,
				   bool aborted)
{
	struct cfg80211_scan_info info = {
		.aborted = aborted,
	};

	WRITE_ONCE(mac->scan_req, NULL);
	if (READ_ONCE(mac->state) == MTK_WLAN_STATE_SCANNING)
		WRITE_ONCE(mac->state, READ_ONCE(mac->fw_up) ?
			   (READ_ONCE(mac->connected) ?
			    MTK_WLAN_STATE_CONNECTED : MTK_WLAN_STATE_IDLE) :
			   MTK_WLAN_STATE_DOWN);

	cfg80211_scan_done(req, &info);
}

/**
 * mtk_wlan_mgmt_scan_work - transmit probe requests on every requested channel
 * @work: &struct mtk_wlan_mac.scan_work
 *
 * The firmware owns the RF channel; without a firmware channel-switch or
 * scan command the host can only transmit probe requests and collect the
 * responses that the firmware receives on its current channel.  Passive
 * scans (no SSID in the request) send nothing and only collect beacons.
 * See the report for this hardware-scan boundary.
 */
void mtk_wlan_mgmt_scan_work(struct work_struct *work)
{
	struct mtk_wlan_mac *mac = container_of(work, struct mtk_wlan_mac,
						scan_work);
	struct cfg80211_scan_request *req = READ_ONCE(mac->scan_req);
	u8 frame[MTK_WLAN_MGMT_FRAME_MAX];
	bool aborted = false;
	unsigned int i, s;

	if (!req)
		return;

	for (i = 0; i < req->n_channels; i++) {
		const struct ieee80211_channel *chan = req->channels[i];
		u8 number;

		if (READ_ONCE(mac->scan_aborted) || !READ_ONCE(mac->fw_up)) {
			aborted = true;
			break;
		}
		if (!mtk_wlan_channel_usable(chan))
			continue;

		number = ieee80211_frequency_to_channel(chan->center_freq);

		/*
		 * No probe request on a no-IR channel (regulatory rule) and
		 * none at all for a passive scan, which cfg80211 expresses by
		 * n_ssids == 0.
		 */
		if (req->n_ssids && !(chan->flags & IEEE80211_CHAN_NO_IR)) {
			for (s = 0; s < req->n_ssids; s++) {
				const u8 *ssid = req->ssids[s].ssid;
				size_t ssid_len = req->ssids[s].ssid_len;
				int len;

				len = mtk_wlan_build_probe_req(mac, number, ssid,
							       ssid_len, req->ie,
							       req->ie_len, frame,
							       sizeof(frame));
				if (len < 0) {
					dev_warn_ratelimited(mac->dev,
							     "cannot build probe request: %d\n",
							     len);
					continue;
				}

				if (mtk_wlan_mgmt_tx(mac, frame, len)) {
					dev_warn_ratelimited(mac->dev,
							     "probe request TX failed on channel %u\n",
							     number);
					if (!READ_ONCE(mac->fw_up)) {
						aborted = true;
						break;
					}
				}
			}
		}

		if (aborted)
			break;
		if (!mtk_wlan_scan_wait(mac, mtk_wlan_scan_dwell_ms(req))) {
			aborted = true;
			break;
		}
	}

	mtk_wlan_scan_complete(mac, req, aborted);
}

/*
 * Compose an authentication frame, authComposeAuthFrameHeaderAndFF()
 * auth.c:258-325: address 1 is the BSSID, address 2 our MAC and address 3
 * the BSSID for the first transaction; the body carries algorithm,
 * transaction sequence number and status code.
 */
static size_t mtk_wlan_build_auth(struct mtk_wlan_mac *mac,
				  const u8 *peer, u16 seq, u16 status,
				  u8 *buf, size_t bufsz)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)buf;

	if (bufsz < MTK_WLAN_MAC_HDR_LEN + 6)
		return 0;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_AUTH);
	mgmt->duration = 0;
	ether_addr_copy(mgmt->da, peer);
	ether_addr_copy(mgmt->sa, mac->mac_addr);
	ether_addr_copy(mgmt->bssid, peer);
	mgmt->seq_ctrl = 0;
	mgmt->u.auth.auth_alg = cpu_to_le16(WLAN_AUTH_OPEN);
	mgmt->u.auth.auth_transaction = cpu_to_le16(seq);
	mgmt->u.auth.status_code = cpu_to_le16(status);

	return MTK_WLAN_MAC_HDR_LEN + 6;
}

/*
 * Compose an association request, assocComposeReAssocReqFrameHeaderAndFF()
 * assoc.c:747-823 and assocBuildReAssocReqFrameCommonIEs() assoc.c:587-731:
 * capability information (ESS only, no privacy, assocBuildCapabilityInfo()
 * assoc.c:505-575), listen interval, SSID, supported rates, extended
 * supported rates and the IEs requested by cfg80211.
 *
 * @ies receives the IEs starting at the SSID element for
 * cfg80211_connect_result().
 */
static int mtk_wlan_build_assoc_req(struct mtk_wlan_mac *mac, const u8 *peer,
				    const u8 *extra_ie, size_t extra_ie_len,
				    u8 *buf, size_t bufsz,
				    u8 *ies, size_t ies_sz, size_t *ies_len)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)buf;
	size_t len = MTK_WLAN_MAC_HDR_LEN + 4;
	size_t ie_start = len, ie_out_len;

	if (MTK_WLAN_MAC_HDR_LEN + 4 + 2 + mac->ssid_len +
	    2 + sizeof(mtk_wlan_basic_rates) + 2 +
	    sizeof(mtk_wlan_ofdm_rates) + extra_ie_len > bufsz)
		return -EMSGSIZE;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ASSOC_REQ);
	mgmt->duration = 0;
	ether_addr_copy(mgmt->da, peer);
	ether_addr_copy(mgmt->sa, mac->mac_addr);
	ether_addr_copy(mgmt->bssid, peer);
	mgmt->seq_ctrl = 0;

	/* CAP_INFO_ESS, station not CF-pollable, no privacy (open only). */
	mgmt->u.assoc_req.capab_info = cpu_to_le16(WLAN_CAPABILITY_ESS);
	mgmt->u.assoc_req.listen_interval =
		cpu_to_le16(MTK_WLAN_LISTEN_INTERVAL);

	len += mtk_wlan_ie_put(buf, len, WLAN_EID_SSID, mac->ssid,
			       mac->ssid_len);
	len += mtk_wlan_ie_put(buf, len, WLAN_EID_SUPP_RATES,
			       mtk_wlan_basic_rates,
			       sizeof(mtk_wlan_basic_rates));
	len += mtk_wlan_ie_put(buf, len, WLAN_EID_EXT_SUPP_RATES,
			       mtk_wlan_ofdm_rates,
			       sizeof(mtk_wlan_ofdm_rates));
	if (extra_ie_len) {
		memcpy(buf + len, extra_ie, extra_ie_len);
		len += extra_ie_len;
	}

	ie_out_len = min(len - ie_start, ies_sz);
	memcpy(ies, buf + ie_start, ie_out_len);
	*ies_len = ie_out_len;

	return len;
}

/* Compose a deauthentication frame, authSendDeauthFrame() auth.c:932-1139. */
static size_t mtk_wlan_build_deauth(struct mtk_wlan_mac *mac, const u8 *peer,
				    u16 reason, u8 *buf, size_t bufsz)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)buf;

	if (bufsz < MTK_WLAN_MAC_HDR_LEN + 2)
		return 0;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_DEAUTH);
	mgmt->duration = 0;
	ether_addr_copy(mgmt->da, peer);
	ether_addr_copy(mgmt->sa, mac->mac_addr);
	ether_addr_copy(mgmt->bssid, peer);
	mgmt->seq_ctrl = 0;
	mgmt->u.deauth.reason_code = cpu_to_le16(reason);

	return MTK_WLAN_MAC_HDR_LEN + 2;
}

/*
 * Transmit one management frame and wait for the matching response.  The RX
 * path stores the response and completes &mac->mgmt_done.  A firmware-down
 * or a locally requested disconnect releases the wait with an error, so the
 * caller never sleeps longer than one timeout after the exchange is over.
 */
static int mtk_wlan_mgmt_exchange(struct mtk_wlan_mac *mac, u16 stype,
				  const u8 *peer, const u8 *frame, size_t len,
				  struct mtk_wlan_mgmt_rsp *rsp)
{
	unsigned long timeout = msecs_to_jiffies(MTK_WLAN_MGMT_TIMEOUT_MS);
	int tries;

	for (tries = 0; tries < MTK_WLAN_MGMT_RETRIES; tries++) {
		int ret;

		if (!READ_ONCE(mac->fw_up))
			return -ENETDOWN;
		if (READ_ONCE(mac->connect_aborted))
			return -ECANCELED;
		if (READ_ONCE(mac->state) != MTK_WLAN_STATE_CONNECTING)
			return -ECANCELED;

		reinit_completion(&mac->mgmt_done);
		spin_lock_bh(&mac->rx_lock);
		mac->mgmt_stype = stype;
		ether_addr_copy(mac->mgmt_peer, peer);
		mac->mgmt_err = 0;
		mac->mgmt_rsp.ies_len = 0;
		spin_unlock_bh(&mac->rx_lock);

		ret = mtk_wlan_mgmt_tx(mac, frame, len);
		if (ret) {
			spin_lock_bh(&mac->rx_lock);
			mac->mgmt_stype = 0;
			spin_unlock_bh(&mac->rx_lock);
			return ret;
		}

		if (!wait_for_completion_timeout(&mac->mgmt_done, timeout)) {
			dev_dbg(mac->dev, "no response to subtype %#x, retry %d\n",
				stype, tries);
			spin_lock_bh(&mac->rx_lock);
			mac->mgmt_stype = 0;
			spin_unlock_bh(&mac->rx_lock);
			continue;
		}

		spin_lock_bh(&mac->rx_lock);
		mac->mgmt_stype = 0;
		ret = mac->mgmt_err;
		if (!ret)
			*rsp = mac->mgmt_rsp;
		spin_unlock_bh(&mac->rx_lock);

		return ret;
	}

	return -ETIMEDOUT;
}

/*
 * Release the BSS reference taken by mtk_wlan_mgmt_connect() when no result
 * is going to be reported to cfg80211.  Called with &mac->lock held.
 */
static void mtk_wlan_connect_release(struct mtk_wlan_mac *mac)
{
	struct cfg80211_bss *bss = mac->connect_bss;

	mac->connect_bss = NULL;
	if (bss)
		cfg80211_put_bss(mac->wiphy, bss);
}

/*
 * Report a failed connection attempt.  cfg80211 expects either an explicit
 * status code from the peer or a timeout notification, never silence.  The
 * BSS reference from connect() is handed over to cfg80211, which consumes it
 * on every path (sme.c:837-848).  Called with &mac->lock held.
 *
 * @status: peer status code, or -1 for a failure without a status code
 */
static void mtk_wlan_connect_report_fail(struct mtk_wlan_mac *mac, int status,
					 enum nl80211_timeout_reason reason)
{
	struct cfg80211_bss *bss = mac->connect_bss;

	mac->connect_bss = NULL;
	if (mac->registered)
		cfg80211_connect_bss(mac->ndev, mac->bssid, bss, mac->req_ie,
				     mac->req_ie_len, NULL, 0, status,
				     GFP_KERNEL, reason);
	else if (bss)
		cfg80211_put_bss(mac->wiphy, bss);
}

/* Release the firmware association state after a failed or cancelled attempt. */
static void mtk_wlan_connect_fw_teardown(struct mtk_wlan_mac *mac)
{
	mtk_wlan_cmd_teardown(mac);
	mac->sta_valid = false;
	mac->aid = 0;
}

/**
 * mtk_wlan_mgmt_connect_work - open-system auth/assoc state machine
 * @work: &struct mtk_wlan_mac.connect_work
 *
 * Mirrors aaaFsmRunEventRxAuth()/aisFsmRunEventJoinComplete(): activate the
 * AIS network and create the AP peer record, send the authentication request,
 * wait for the authentication response carrying transaction sequence 2 and
 * status 0, send the association request, wait for an association response,
 * then update the peer record to associated and sync the BSS state before
 * cfg80211 is told about the connection.  Only open-system authentication is
 * advertised, so a challenge-text exchange never happens.
 */
void mtk_wlan_mgmt_connect_work(struct work_struct *work)
{
	struct mtk_wlan_mac *mac = container_of(work, struct mtk_wlan_mac,
						connect_work);
	struct mtk_wlan_mgmt_rsp rsp;
	u8 frame[MTK_WLAN_MGMT_FRAME_MAX];
	u8 req_ies[MTK_WLAN_ASSOC_IES_MAX];
	size_t req_ies_len = 0;
	size_t len;
	int ret;

	mutex_lock(&mac->lock);

	if (READ_ONCE(mac->state) != MTK_WLAN_STATE_CONNECTING ||
	    READ_ONCE(mac->connect_aborted)) {
		/* Cancelled before the worker ran: nothing to report. */
		mtk_wlan_connect_release(mac);
		mutex_unlock(&mac->lock);
		return;
	}
	if (!READ_ONCE(mac->fw_up)) {
		WRITE_ONCE(mac->state, MTK_WLAN_STATE_DOWN);
		mtk_wlan_connect_report_fail(mac, -1,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}

	/*
	 * Firmware bring-up for the join: activate the AIS network
	 * (nicActivateNetwork(), ais_fsm.c:1873) and install the AP peer
	 * record in its initial state (ais_fsm.c:1372-1384).
	 */
	ret = mtk_wlan_cmd_bss_activate(mac, true);
	if (ret) {
		WRITE_ONCE(mac->state, MTK_WLAN_STATE_DOWN);
		mtk_wlan_connect_report_fail(mac, -1,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}
	ret = mtk_wlan_cmd_update_sta(mac, MTK_WLAN_STA_STATE_1, 0, false);
	if (ret) {
		mtk_wlan_connect_fw_teardown(mac);
		WRITE_ONCE(mac->state, MTK_WLAN_STATE_IDLE);
		mtk_wlan_connect_report_fail(mac, -1,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}
	mac->sta_valid = true;

	/* Step 1: open-system authentication, transaction sequence 1. */
	len = mtk_wlan_build_auth(mac, mac->bssid, 1, 0, frame, sizeof(frame));
	if (!len) {
		mtk_wlan_connect_fw_teardown(mac);
		WRITE_ONCE(mac->state, MTK_WLAN_STATE_IDLE);
		mtk_wlan_connect_report_fail(mac, -1, NL80211_TIMEOUT_AUTH);
		mutex_unlock(&mac->lock);
		return;
	}

	ret = mtk_wlan_mgmt_exchange(mac, IEEE80211_STYPE_AUTH, mac->bssid,
				     frame, len, &rsp);
	if (ret) {
		bool cancelled = READ_ONCE(mac->connect_aborted) ||
				 READ_ONCE(mac->state) !=
				 MTK_WLAN_STATE_CONNECTING;
		bool down = ret == -ENETDOWN || !READ_ONCE(mac->fw_up);

		mtk_wlan_connect_fw_teardown(mac);
		if (cancelled) {
			mtk_wlan_connect_release(mac);
			mutex_unlock(&mac->lock);
			return;
		}
		WRITE_ONCE(mac->state, down ? MTK_WLAN_STATE_DOWN :
			   MTK_WLAN_STATE_IDLE);
		mtk_wlan_connect_report_fail(mac, -1, NL80211_TIMEOUT_AUTH);
		mutex_unlock(&mac->lock);
		return;
	}
	if (rsp.status != WLAN_STATUS_SUCCESS) {
		mtk_wlan_connect_fw_teardown(mac);
		WRITE_ONCE(mac->state, MTK_WLAN_STATE_IDLE);
		mtk_wlan_connect_report_fail(mac, rsp.status,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}

	/* Step 2: association request. */
	ret = mtk_wlan_build_assoc_req(mac, mac->bssid, mac->assoc_ie,
				       mac->assoc_ie_len, frame, sizeof(frame),
				       req_ies, sizeof(req_ies), &req_ies_len);
	if (ret < 0) {
		mtk_wlan_connect_fw_teardown(mac);
		WRITE_ONCE(mac->state, MTK_WLAN_STATE_IDLE);
		mtk_wlan_connect_report_fail(mac, WLAN_STATUS_UNSPECIFIED_FAILURE,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}
	len = ret;

	ret = mtk_wlan_mgmt_exchange(mac, IEEE80211_STYPE_ASSOC_RESP,
				     mac->bssid, frame, len, &rsp);
	if (ret) {
		bool cancelled = READ_ONCE(mac->connect_aborted) ||
				 READ_ONCE(mac->state) !=
				 MTK_WLAN_STATE_CONNECTING;
		bool down = ret == -ENETDOWN || !READ_ONCE(mac->fw_up);

		mtk_wlan_connect_fw_teardown(mac);
		if (cancelled) {
			mtk_wlan_connect_release(mac);
			mutex_unlock(&mac->lock);
			return;
		}
		WRITE_ONCE(mac->state, down ? MTK_WLAN_STATE_DOWN :
			   MTK_WLAN_STATE_IDLE);
		mtk_wlan_connect_report_fail(mac, -1, NL80211_TIMEOUT_ASSOC);
		mutex_unlock(&mac->lock);
		return;
	}
	if (rsp.status != WLAN_STATUS_SUCCESS) {
		mtk_wlan_connect_fw_teardown(mac);
		WRITE_ONCE(mac->state, MTK_WLAN_STATE_IDLE);
		mtk_wlan_connect_report_fail(mac, rsp.status,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}

	/*
	 * Step 3: the AP accepted us.  Move the peer record to class 3 with
	 * the assigned AID (cnmStaRecChangeState() with STA_STATE_3 sends the
	 * update and waits for the result, cnm_mem.c:1032-1056), then sync the
	 * BSS state; downstream does exactly this before indicating the media
	 * state to the host (ais_fsm.c:3353-3358).
	 */
	mac->aid = rsp.aid;
	ret = mtk_wlan_cmd_update_sta(mac, MTK_WLAN_STA_STATE_3, mac->aid, true);
	if (ret) {
		dev_warn(mac->dev, "cannot update the STA record: %d\n", ret);
		mtk_wlan_connect_fw_teardown(mac);
		WRITE_ONCE(mac->state, READ_ONCE(mac->fw_up) ?
			   MTK_WLAN_STATE_IDLE : MTK_WLAN_STATE_DOWN);
		mtk_wlan_connect_report_fail(mac, -1,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}
	ret = mtk_wlan_cmd_set_bss_info(mac, true);
	if (ret) {
		dev_warn(mac->dev, "cannot set the BSS info: %d\n", ret);
		mtk_wlan_connect_fw_teardown(mac);
		WRITE_ONCE(mac->state, READ_ONCE(mac->fw_up) ?
			   MTK_WLAN_STATE_IDLE : MTK_WLAN_STATE_DOWN);
		mtk_wlan_connect_report_fail(mac, -1,
					     NL80211_TIMEOUT_UNSPECIFIED);
		mutex_unlock(&mac->lock);
		return;
	}

	ether_addr_copy(mac->req_ie, req_ies);
	mac->req_ie_len = req_ies_len;
	WRITE_ONCE(mac->connected, true);
	mac->assoc_ts = jiffies;
	WRITE_ONCE(mac->state, MTK_WLAN_STATE_CONNECTED);
	mtk_wlan_netdev_link_changed(mac, true);

	{
		struct cfg80211_bss *bss = mac->connect_bss;

		mac->connect_bss = NULL;
		if (mac->registered)
			cfg80211_connect_bss(mac->ndev, mac->bssid, bss,
					     mac->req_ie, mac->req_ie_len,
					     rsp.ies, rsp.ies_len,
					     WLAN_STATUS_SUCCESS, GFP_KERNEL,
					     NL80211_TIMEOUT_UNSPECIFIED);
		else if (bss)
			cfg80211_put_bss(mac->wiphy, bss);
	}
	mutex_unlock(&mac->lock);

	dev_info(mac->dev, "associated to %pM on channel %u\n", mac->bssid,
		 mac->channel);
}

/*
 * Tear down the association state, report it to cfg80211 and stop the TX
 * path.  Takes &mac->lock itself.
 */
static void mtk_wlan_link_down(struct mtk_wlan_mac *mac, u16 reason,
			       bool locally)
{
	bool was_connected;
	struct sk_buff *skb;

	mutex_lock(&mac->lock);
	was_connected = READ_ONCE(mac->connected);
	WRITE_ONCE(mac->connected, false);
	if (READ_ONCE(mac->state) != MTK_WLAN_STATE_DOWN)
		WRITE_ONCE(mac->state, READ_ONCE(mac->fw_up) ?
			   MTK_WLAN_STATE_IDLE : MTK_WLAN_STATE_DOWN);
	mtk_wlan_netdev_link_changed(mac, false);
	mutex_unlock(&mac->lock);

	/*
	 * Release the firmware peer/BSS state: SET_BSS_INFO disconnected, the
	 * STA record remove command and network deactivation.  Skipped when
	 * the firmware is already gone.
	 */
	if (was_connected || mac->sta_valid)
		mtk_wlan_cmd_teardown(mac);

	while ((skb = skb_dequeue(&mac->txq)))
		dev_kfree_skb(skb);

	if (was_connected && mac->registered)
		cfg80211_disconnected(mac->ndev, reason, NULL, 0, locally,
				      GFP_KERNEL);
}

/**
 * mtk_wlan_mgmt_deauth_rx - handle a deauthentication/disassociation frame
 * @mac: MAC instance
 * @reason: reason code from the frame
 * @locally: %true when the driver itself initiated the teardown
 */
void mtk_wlan_mgmt_deauth_rx(struct mtk_wlan_mac *mac, u16 reason,
			     bool locally)
{
	if (!READ_ONCE(mac->connected))
		return;

	dev_info(mac->dev, "%s by %pM, reason %u\n",
		 locally ? "disconnected" : "disassociated", mac->bssid,
		 reason);
	mtk_wlan_link_down(mac, reason, locally);
}

/**
 * mtk_wlan_mgmt_disconnect - locally requested disconnect
 * @mac: MAC instance
 * @reason: reason code to send in the deauthentication frame
 *
 * Handles both states cfg80211 can ask about: an established association is
 * deauthenticated and reported; an association attempt in progress is
 * aborted and the worker releases the frame/bss state without reporting a
 * result, because cfg80211 already abandoned the request.
 *
 * Return: 0 on success, -ENOTCONN when not associated or connecting.
 */
int mtk_wlan_mgmt_disconnect(struct mtk_wlan_mac *mac, u16 reason)
{
	u8 frame[MTK_WLAN_MAC_HDR_LEN + 2];
	bool connecting = READ_ONCE(mac->state) == MTK_WLAN_STATE_CONNECTING;
	bool fw_up;
	size_t len;

	/*
	 * Cancel an association attempt before taking &mac->lock: the connect
	 * worker holds that lock while it waits for a management response, so
	 * the exchange must be released first.
	 */
	if (connecting) {
		WRITE_ONCE(mac->connect_aborted, true);
		mtk_wlan_cmd_cancel(mac);
		spin_lock_bh(&mac->rx_lock);
		if (mac->mgmt_stype) {
			mac->mgmt_err = -ECANCELED;
			complete(&mac->mgmt_done);
		}
		spin_unlock_bh(&mac->rx_lock);
		flush_work(&mac->connect_work);
	}

	mutex_lock(&mac->lock);
	if (!READ_ONCE(mac->connected)) {
		if (READ_ONCE(mac->state) == MTK_WLAN_STATE_CONNECTING)
			WRITE_ONCE(mac->state, READ_ONCE(mac->fw_up) ?
				   MTK_WLAN_STATE_IDLE : MTK_WLAN_STATE_DOWN);
		mutex_unlock(&mac->lock);
		return connecting ? 0 : -ENOTCONN;
	}

	/* aisFsmDisconnect() deauthenticates the current AP, auth.c:932. */
	len = mtk_wlan_build_deauth(mac, mac->bssid, reason, frame,
				    sizeof(frame));
	fw_up = READ_ONCE(mac->fw_up);
	mutex_unlock(&mac->lock);

	if (len && fw_up)
		mtk_wlan_mgmt_tx(mac, frame, len);

	mtk_wlan_link_down(mac, reason, true);
	return 0;
}

/**
 * mtk_wlan_mgmt_connect - validate and start an open-system connection
 * @mac: MAC instance
 * @sme: cfg80211 connect parameters, only valid during this call
 *
 * Only open networks with open-system authentication are supported; the
 * wiphy advertises no cipher suites at all.  Everything else is rejected
 * with a real error instead of a fabricated success.  The BSS reference is
 * kept until the connect worker reports the result.
 *
 * Return: 0 when the connection was started, negative errno otherwise.
 */
int mtk_wlan_mgmt_connect(struct mtk_wlan_mac *mac,
			  struct cfg80211_connect_params *sme)
{
	struct cfg80211_bss *bss;
	struct ieee80211_channel *chan;
	const u8 *bssid = sme->bssid;
	int ret = 0;

	if (!READ_ONCE(mac->fw_up))
		return -ENETDOWN;
	if (READ_ONCE(mac->state) == MTK_WLAN_STATE_CONNECTING ||
	    READ_ONCE(mac->connected))
		return -EALREADY;
	if (sme->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
	    sme->auth_type != NL80211_AUTHTYPE_AUTOMATIC)
		return -EOPNOTSUPP;
	if (!sme->ssid || sme->ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;
	if (sme->ie_len > MTK_WLAN_ASSOC_IES_MAX)
		return -E2BIG;

	/*
	 * No key offload: reject every form of encryption material, whether
	 * it arrives in the crypto parameters, as a key, or as a WPA/RSN IE
	 * inside the association IEs.
	 */
	if (sme->privacy || sme->key || sme->key_len || sme->mfp ||
	    sme->crypto.wpa_versions || sme->crypto.n_ciphers_pairwise ||
	    sme->crypto.n_akm_suites ||
	    (sme->ie_len &&
	     (cfg80211_find_ie(WLAN_EID_RSN, sme->ie, sme->ie_len) ||
	      cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT,
				      WLAN_OUI_TYPE_MICROSOFT_WPA,
				      sme->ie, sme->ie_len)))) {
		dev_info(mac->dev,
			 "connect: encryption is not supported (no key offload)\n");
		return -EOPNOTSUPP;
	}

	bss = cfg80211_get_bss(mac->wiphy, sme->channel, bssid, sme->ssid,
			       sme->ssid_len, IEEE80211_BSS_TYPE_ESS,
			       IEEE80211_PRIVACY_ANY);
	if (!bss) {
		dev_dbg(mac->dev, "connect: requested BSS not in scan results\n");
		return -ENOENT;
	}

	chan = bss->channel;
	if (!bssid)
		bssid = bss->bssid;

	if (bss->capability & WLAN_CAPABILITY_PRIVACY) {
		/* Documented boundary: no key offload, so no WPA/RSN. */
		dev_info(mac->dev, "connect: %pM requires encryption, unsupported\n",
			 bssid);
		ret = -EOPNOTSUPP;
		goto out_put;
	}
	if (!chan || chan->band != NL80211_BAND_2GHZ) {
		ret = -EOPNOTSUPP;
		goto out_put;
	}

	/*
	 * Derive the firmware AP parameters (rate sets, PHY types, protection)
	 * from the BSS IEs before committing to the attempt.
	 */
	ret = mtk_wlan_cmd_prepare_ap(mac, bss);
	if (ret)
		goto out_put;

	/* Stop a software scan that is still running. */
	mtk_wlan_mgmt_abort_scan(mac);

	mutex_lock(&mac->lock);
	if (READ_ONCE(mac->state) == MTK_WLAN_STATE_CONNECTING ||
	    READ_ONCE(mac->connected)) {
		mutex_unlock(&mac->lock);
		ret = -EALREADY;
		goto out_put;
	}

	ether_addr_copy(mac->bssid, bssid);
	mac->channel = ieee80211_frequency_to_channel(chan->center_freq);
	memcpy(mac->ssid, sme->ssid, sme->ssid_len);
	mac->ssid_len = sme->ssid_len;
	if (sme->ie_len)
		memcpy(mac->assoc_ie, sme->ie, sme->ie_len);
	mac->assoc_ie_len = sme->ie_len;
	mac->req_ie_len = 0;
	mac->sta_valid = false;
	mac->aid = 0;
	mac->connect_bss = bss;	/* ownership moves to the worker/cfg80211 */
	WRITE_ONCE(mac->connect_aborted, false);
	WRITE_ONCE(mac->state, MTK_WLAN_STATE_CONNECTING);
	mutex_unlock(&mac->lock);

	schedule_work(&mac->connect_work);
	return 0;

out_put:
	cfg80211_put_bss(mac->wiphy, bss);
	return ret;
}

/**
 * mtk_wlan_mgmt_quiesce - abort scan/connect after the firmware went away
 * @mac: MAC instance
 *
 * Called from the firmware-down and teardown paths.  A running scan reports
 * itself aborted, a waiting management exchange is released with -ENETDOWN
 * so the connect worker reports a timeout, and an established connection is
 * reported as disconnected.  Every cfg80211 notification is emitted by the
 * worker or by mtk_wlan_link_down(), exactly once.
 */
void mtk_wlan_mgmt_quiesce(struct mtk_wlan_mac *mac)
{
	struct cfg80211_scan_request *req;

	/* Release a command wait first: the transport is gone. */
	mtk_wlan_cmd_abort(mac);

	WRITE_ONCE(mac->scan_aborted, true);
	flush_work(&mac->scan_work);

	/*
	 * The worker clears scan_req before calling cfg80211_scan_done(), so a
	 * request still present here was queued but never executed.  Report it
	 * rather than leaking it.
	 */
	req = READ_ONCE(mac->scan_req);
	if (req)
		mtk_wlan_scan_complete(mac, req, true);

	spin_lock_bh(&mac->rx_lock);
	if (mac->mgmt_stype) {
		mac->mgmt_err = -ENETDOWN;
		complete(&mac->mgmt_done);
	}
	spin_unlock_bh(&mac->rx_lock);
	flush_work(&mac->connect_work);

	if (READ_ONCE(mac->state) == MTK_WLAN_STATE_SCANNING)
		WRITE_ONCE(mac->state, READ_ONCE(mac->fw_up) ?
			   (READ_ONCE(mac->connected) ?
			    MTK_WLAN_STATE_CONNECTED : MTK_WLAN_STATE_IDLE) :
			   MTK_WLAN_STATE_DOWN);
	if (READ_ONCE(mac->state) == MTK_WLAN_STATE_CONNECTING)
		WRITE_ONCE(mac->state, READ_ONCE(mac->fw_up) ?
			   MTK_WLAN_STATE_IDLE : MTK_WLAN_STATE_DOWN);

	if (READ_ONCE(mac->connected))
		mtk_wlan_link_down(mac, 0, false);
}

/* Report a beacon/probe response to cfg80211 scan results. */
static void mtk_wlan_rx_bss(struct mtk_wlan_mac *mac,
			    struct ieee80211_mgmt *mgmt, size_t len,
			    u8 channel, u8 rcpi)
{
	struct cfg80211_inform_bss data = { };
	struct cfg80211_bss *bss;
	struct ieee80211_channel *chan;
	const u8 *ie;
	size_t fixed = offsetof(struct ieee80211_mgmt, u.beacon.variable);
	u32 freq;

	/*
	 * Trust the DS parameter set like scanUpdateBssDesc()
	 * (scan.c:1975-1987); fall back to the channel reported by the HIF RX
	 * header.  Only 2.4 GHz exists in this port.
	 */
	if (len >= fixed) {
		ie = cfg80211_find_ie(WLAN_EID_DS_PARAMS,
				      mgmt->u.beacon.variable, len - fixed);
		if (ie && ie[1] == 1 && ie[2] >= 1 && ie[2] <= 14)
			channel = ie[2];
	}

	freq = ieee80211_channel_to_frequency(channel, NL80211_BAND_2GHZ);
	chan = ieee80211_get_channel(mac->wiphy, freq);
	if (!chan) {
		atomic_inc(&mac->rx_dropped);
		return;
	}

	data.chan = chan;
	data.signal = mtk_wlan_rcpi_to_dbm(rcpi) * 100;
	data.boottime_ns = ktime_get_boottime_ns();

	bss = cfg80211_inform_bss_frame_data(mac->wiphy, &data, mgmt, len,
					     GFP_KERNEL);
	if (!bss)
		return;

	cfg80211_put_bss(mac->wiphy, bss);
}

/*
 * Association id from an association response, assoc.c:1183-1200: the AID is
 * the low 14 bits, with the downstream workaround for APs that raise bits 6
 * and 7 instead of bits 14 and 15.
 */
static u16 mtk_wlan_assoc_id(u16 aid)
{
	if ((aid & BIT(6)) && (aid & BIT(7)) && !(aid & GENMASK(15, 8)))
		return aid & ~GENMASK(7, 6);
	return aid & GENMASK(13, 0);
}

/*
 * Deliver a management response to a waiting exchange.  The waiter checks
 * the subtype and the transmitter address, so a stale frame can never
 * complete a newer request.
 */
static void mtk_wlan_mgmt_complete(struct mtk_wlan_mac *mac, u16 stype,
				   const u8 *sa, u16 status, u16 aid,
				   const u8 *ies, size_t ies_len, u8 rcpi)
{
	spin_lock_bh(&mac->rx_lock);

	if (mac->mgmt_stype != stype ||
	    !ether_addr_equal(mac->mgmt_peer, sa)) {
		spin_unlock_bh(&mac->rx_lock);
		atomic_inc(&mac->rx_dropped);
		return;
	}

	mac->mgmt_rsp.status = status;
	mac->mgmt_rsp.aid = aid;
	mac->mgmt_rsp.ies_len = min(ies_len, sizeof(mac->mgmt_rsp.ies));
	if (mac->mgmt_rsp.ies_len)
		memcpy(mac->mgmt_rsp.ies, ies, mac->mgmt_rsp.ies_len);
	mac->last_signal = rcpi;
	mac->have_signal = true;
	mac->mgmt_stype = 0;

	spin_unlock_bh(&mac->rx_lock);
	complete(&mac->mgmt_done);
}

/* Update the RCPI reported for the AP the station is associated with. */
static void mtk_wlan_update_signal(struct mtk_wlan_mac *mac, const u8 *sa,
				   u8 rcpi)
{
	spin_lock_bh(&mac->rx_lock);
	if (READ_ONCE(mac->connected) &&
	    ether_addr_equal(sa, mac->bssid)) {
		mac->last_signal = rcpi;
		mac->have_signal = true;
	}
	spin_unlock_bh(&mac->rx_lock);
}

/**
 * mtk_wlan_rx_mgmt - dispatch one received 802.11 management frame
 * @mac: MAC instance
 * @frame: frame including the MAC header
 * @len: frame length
 * @channel: channel reported by the HIF RX header
 * @rcpi: RCPI reported by the HIF RX header
 */
void mtk_wlan_rx_mgmt(struct mtk_wlan_mac *mac, const u8 *frame, size_t len,
		      u8 channel, u8 rcpi)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)frame;
	__le16 fc;

	if (len < MTK_WLAN_MAC_HDR_LEN) {
		atomic_inc(&mac->rx_dropped);
		return;
	}

	fc = mgmt->frame_control;

	/*
	 * Scan results are only collected while cfg80211 is waiting for them;
	 * a beacon received while idle is of no use to this driver (it has no
	 * roaming implementation).
	 */
	if (ieee80211_is_beacon(fc) || ieee80211_is_probe_resp(fc)) {
		if (len < MTK_WLAN_MAC_HDR_LEN + 12) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		mtk_wlan_update_signal(mac, mgmt->sa, rcpi);
		if (READ_ONCE(mac->scan_req))
			mtk_wlan_rx_bss(mac, mgmt, len, channel, rcpi);
		return;
	}

	if (ieee80211_is_auth(fc)) {
		if (len < MTK_WLAN_MAC_HDR_LEN + 6) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		/*
		 * Only the second transaction of an open-system exchange is a
		 * response: algorithm 0, sequence 2 (aaa_fsm.c:415-430).
		 */
		if (le16_to_cpu(mgmt->u.auth.auth_transaction) != 2 ||
		    le16_to_cpu(mgmt->u.auth.auth_alg) != WLAN_AUTH_OPEN) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		mtk_wlan_mgmt_complete(mac, IEEE80211_STYPE_AUTH, mgmt->sa,
				       le16_to_cpu(mgmt->u.auth.status_code), 0,
				       mgmt->u.auth.variable,
				       len - MTK_WLAN_MAC_HDR_LEN - 6, rcpi);
		return;
	}

	if (ieee80211_is_assoc_resp(fc)) {
		if (len < MTK_WLAN_MAC_HDR_LEN + 6) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		mtk_wlan_mgmt_complete(mac, IEEE80211_STYPE_ASSOC_RESP,
				       mgmt->sa,
				       le16_to_cpu(mgmt->u.assoc_resp.status_code),
				       mtk_wlan_assoc_id(
					       le16_to_cpu(mgmt->u.assoc_resp.aid)),
				       mgmt->u.assoc_resp.variable,
				       len - MTK_WLAN_MAC_HDR_LEN - 6, rcpi);
		return;
	}

	if (ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)) {
		u16 reason;

		if (len < MTK_WLAN_MAC_HDR_LEN + 2) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		reason = le16_to_cpu(mgmt->u.deauth.reason_code);
		if (!ether_addr_equal(mgmt->sa, mac->bssid)) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		mtk_wlan_mgmt_deauth_rx(mac, reason, false);
		return;
	}

	/*
	 * Association requests, probes, action frames and anything else are
	 * not part of the STA-only interface built here.
	 */
	atomic_inc(&mac->rx_dropped);
}
