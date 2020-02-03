/*
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: Target interface file for pkt_capture component to
 * Implement api's which shall be used by pkt_capture component
 * in target_if internally.
 */

#include <target_if_pkt_capture.h>
#include <wmi_unified_api.h>
#include <target_if.h>
#include <init_deinit_lmac.h>

QDF_STATUS target_if_set_packet_capture_mode(struct wlan_objmgr_psoc *psoc,
					     uint8_t vdev_id,
					     enum pkt_capture_mode mode)
{
	wmi_unified_t wmi_handle = lmac_get_wmi_unified_hdl(psoc);
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	if (!wmi_handle) {
		target_if_err("Invalid wmi handle");
		return QDF_STATUS_E_INVAL;
	}

	target_if_debug("psoc:%pK, vdev_id:%d mode:%d",
			psoc, vdev_id, mode);

	if (mode != PACKET_CAPTURE_MODE_DISABLE) {
		struct vdev_set_params param;

		param.if_id = vdev_id;
		param.param_id = WMI_VDEV_PARAM_PACKET_CAPTURE_MODE;
		param.param_value = (uint32_t)mode;

		status = wmi_unified_vdev_set_param_send(wmi_handle, &param);
		if (QDF_IS_STATUS_SUCCESS(status))
			ucfg_pkt_capture_set_pktcap_mode(psoc, mode);
		else
			pkt_capture_err("failed to set packet capture mode");
	}
	return status;
}

/**
 * target_if_mgmt_offload_data_event_handler() - offload event handler
 * @handle: scn handle
 * @data: mgmt data
 * @data_len: data length
 *
 * Process management offload frame.
 *
 * Return: 0 for success or error code
 */
static int
target_if_mgmt_offload_data_event_handler(void *handle, uint8_t *data,
					  uint32_t data_len)
{
	static uint8_t limit_prints_invalid_len = RATE_LIMIT - 1;
	struct mgmt_offload_event_params params;
	struct wmi_unified *wmi_handle;
	struct wlan_objmgr_psoc *psoc;
	struct wlan_objmgr_pdev *pdev;
	QDF_STATUS status;
	qdf_nbuf_t wbuf;

	psoc = target_if_get_psoc_from_scn_hdl(handle);
	if (!psoc) {
		pkt_capture_err("psoc is NULL");
		return -EINVAL;
	}

	wmi_handle = get_wmi_unified_hdl_from_psoc(psoc);
	if (!wmi_handle) {
		target_if_err("Invalid WMI handle");
		return -EINVAL;
	}

	pdev = target_if_get_pdev_from_scn_hdl(handle);
	if (!pdev) {
		pkt_capture_err("pdev is NULL");
		return -EINVAL;
	}

	if (!(ucfg_pkt_capture_get_pktcap_mode(psoc) &
	      PKT_CAPTURE_MODE_MGMT_ONLY))
		return -EINVAL;

	status = wmi_unified_extract_vdev_mgmt_offload_event(wmi_handle, data,
							     &params);
	if (QDF_IS_STATUS_ERROR(status)) {
		pkt_capture_err("Extract mgmt offload event failed");
		return -EINVAL;
	}

	if (!params.buf) {
		pkt_capture_err("Mgmt offload buf is NULL");
		return -EINVAL;
	}

	if (params.buf_len < sizeof(struct ieee80211_hdr_3addr) ||
	    params.buf_len > data_len) {
		limit_prints_invalid_len++;
		if (limit_prints_invalid_len == RATE_LIMIT) {
			pkt_capture_debug(
			"Invalid mgmt packet, data_len %u, params.buf_len %u",
			data_len, params.buf_len);
			limit_prints_invalid_len = 0;
		}
		return -EINVAL;
	}

	wbuf = qdf_nbuf_alloc(NULL,
			      roundup(params.buf_len + RESERVE_BYTES, 4),
			      RESERVE_BYTES, 4, false);
	if (!wbuf) {
		pkt_capture_err("Failed to allocate wbuf for mgmt pkt len(%u)",
				params.buf_len);
		return -ENOMEM;
	}

	qdf_nbuf_put_tail(wbuf, params.buf_len);
	qdf_nbuf_set_protocol(wbuf, ETH_P_CONTROL);
	qdf_mem_copy(qdf_nbuf_data(wbuf), params.buf, params.buf_len);

	status = params.tx_status;
	if (QDF_STATUS_SUCCESS !=
		ucfg_pkt_capture_process_mgmt_tx_data(pdev, &params,
						      wbuf, status))
		qdf_nbuf_free(wbuf);

	return 0;
}

QDF_STATUS
target_if_register_mgmt_data_offload_event(struct wlan_objmgr_psoc *psoc)
{
	wmi_unified_t wmi_handle;

	PKT_CAPTURE_ENTER();

	if (!psoc) {
		pkt_capture_err("psoc got NULL");
		return QDF_STATUS_E_FAILURE;
	}
	wmi_handle = get_wmi_unified_hdl_from_psoc(psoc);

	if (ucfg_pkt_capture_get_mode(psoc) &&
	    wmi_service_enabled(wmi_handle,
				wmi_service_packet_capture_support)) {
		uint8_t status;

		status = wmi_unified_register_event_handler(
				wmi_handle,
				wmi_mgmt_offload_data_event_id,
				target_if_mgmt_offload_data_event_handler,
				WMI_RX_WORK_CTX);
		if (status) {
			pkt_capture_err("Failed to register MGMT offload handler");
			return QDF_STATUS_E_FAILURE;
		}
	}
	PKT_CAPTURE_ENTER();
	return QDF_STATUS_SUCCESS;
}
