/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mesh/mesh.h"
#include "prov.h"
#include "net.h"
#include "proxy.h"
#include "adv.h"
#include "prov_bearer.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_PROV)
#define LOG_MODULE_NAME bt_mesh_pb_gatt
#include "log/log.h"

struct prov_link {
	uint16_t conn_handle;
	const struct prov_bearer_cb *cb;
	void *cb_data;
	struct {
		u8_t  id;        /* Transaction ID */
		u8_t  prev_id;   /* Previous Transaction ID */
		u8_t  seg;       /* Bit-field of unreceived segments */
		u8_t  last_seg;  /* Last segment (to check length) */
		u8_t  fcs;       /* Expected FCS value */
		struct os_mbuf *buf;
	} rx;
	struct k_delayed_work prot_timer;
};

static struct prov_link link;

static void reset_state(void)
{
	link.conn_handle = BLE_HS_CONN_HANDLE_NONE;

	k_delayed_work_cancel(&link.prot_timer);
	memset(&link, 0, offsetof(struct prov_link, prot_timer));

	link.rx.buf = bt_mesh_proxy_get_buf();
}

static void protocol_timeout(struct ble_npl_event *work)
{
	const struct prov_bearer_cb *cb = link.cb;

	BT_DBG("Protocol timeout");

	if (link.conn_handle) {
		bt_mesh_pb_gatt_close(link.conn_handle);
	}

	reset_state();

	cb->link_closed(&pb_gatt, link.cb_data,
			PROV_BEARER_LINK_STATUS_TIMEOUT);
}

int bt_mesh_pb_gatt_recv(uint16_t conn_handle, struct os_mbuf *buf)
{
	BT_DBG("%u bytes: %s", buf->om_len, bt_hex(buf->om_data, buf->om_len));

	if (link.conn_handle != conn_handle || !link.cb) {
		BT_WARN("Data for unexpected connection");
		return -ENOTCONN;
	}

	if (buf->om_len < 1) {
		BT_WARN("Too short provisioning packet (len %u)", buf->om_len);
		return -EINVAL;
	}

	k_delayed_work_submit(&link.prot_timer, PROTOCOL_TIMEOUT);

	link.cb->recv(&pb_gatt, link.cb_data, buf);

	return 0;
}

int bt_mesh_pb_gatt_open(uint16_t conn_handle)
{
	BT_DBG("conn %p", conn_handle);

	if (link.conn_handle) {
		return -EBUSY;
	}

	link.conn_handle = conn_handle;
	k_delayed_work_submit(&link.prot_timer, PROTOCOL_TIMEOUT);

	link.cb->link_opened(&pb_gatt, link.cb_data);

	return 0;
}

int bt_mesh_pb_gatt_close(uint16_t conn_handle)
{
	BT_DBG("conn %p", conn_handle);

	if (link.conn_handle != conn_handle) {
		BT_ERR("Not connected");
		return -ENOTCONN;
	}

	link.cb->link_closed(&pb_gatt, link.cb_data,
			     PROV_BEARER_LINK_STATUS_SUCCESS);

	reset_state();

	return 0;
}

static int link_accept(const struct prov_bearer_cb *cb, void *cb_data)
{
	bt_mesh_proxy_prov_enable();
	bt_mesh_adv_update();

	link.cb = cb;
	link.cb_data = cb_data;

	return 0;
}

static int buf_send(struct os_mbuf *buf, prov_bearer_send_complete_t cb,
		    void *cb_data)
{
	if (!link.conn_handle) {
		return -ENOTCONN;
	}

	k_delayed_work_submit(&link.prot_timer, PROTOCOL_TIMEOUT);

	return bt_mesh_proxy_send(link.conn_handle, BT_MESH_PROXY_PROV, buf);
}

static void clear_tx(void)
{
	/* No action */
}

void pb_gatt_init(void)
{
	k_delayed_work_init(&link.prot_timer, protocol_timeout);
}

const struct prov_bearer pb_gatt = {
	.type = BT_MESH_PROV_GATT,
	.link_accept = link_accept,
	.send = buf_send,
	.clear_tx = clear_tx,
};