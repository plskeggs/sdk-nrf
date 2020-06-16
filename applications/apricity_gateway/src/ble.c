/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>

#include <bluetooth/gatt.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <dk_buttons_and_leds.h>

#include <nrf_cloud.h>
#include "nrf_cloud.h"
#include "ble.h"

#include "ble_codec.h"
#include "ctype.h"
#include "nrf_cloud_transport.h"
#include "ble_conn_mgr.h"

#define SEND_NOTIFY_STACK_SIZE 2048
#define SEND_NOTIFY_PRIORITY 9

#define MAX_SUBSCRIPTIONS 4

bool discover_in_progress = false;
bool ok_to_send = true;
int num_devices_found = 0;
u8_t read_buf[512];

struct k_timer rec_timer;
struct k_timer scan_timer;
struct k_work scan_off_work;
struct k_work ble_device_encode_work;

char uuid[BT_MAX_UUID_LEN];
char path[BT_MAX_PATH_LEN];

ble_scanned_devices ble_scanned_device[MAX_SCAN_RESULTS];

struct rec_data_t
{
        void *fifo_reserved;
        struct bt_gatt_subscribe_params sub_params;
        struct bt_gatt_read_params read_params;
        char addr_trunc[BT_ADDR_STR_LEN];
        char data[256];
        bool read;
        u8_t length;
};

K_FIFO_DEFINE(rec_fifo);

/*Convert ble address string to uppcase*/
void bt_to_upper(char* addr, u8_t addr_len)
{

        for(int i = 0; i < addr_len; i++)
        {
                addr[i] = toupper(addr[i]);
        }

}

/*Get uuid string from bt_uuid object*/
void bt_uuid_get_str(const struct bt_uuid *uuid, char *str, size_t len)
{

        u32_t tmp1, tmp5;
        u16_t tmp0, tmp2, tmp3, tmp4;

        switch (uuid->type) {
        case BT_UUID_TYPE_16:
                snprintk(str, len, "%04x", BT_UUID_16(uuid)->val);
                break;
        case BT_UUID_TYPE_32:
                snprintk(str, len, "%04x", BT_UUID_32(uuid)->val);
                break;
        case BT_UUID_TYPE_128:
                memcpy(&tmp0, &BT_UUID_128(uuid)->val[0], sizeof(tmp0));
                memcpy(&tmp1, &BT_UUID_128(uuid)->val[2], sizeof(tmp1));
                memcpy(&tmp2, &BT_UUID_128(uuid)->val[6], sizeof(tmp2));
                memcpy(&tmp3, &BT_UUID_128(uuid)->val[8], sizeof(tmp3));
                memcpy(&tmp4, &BT_UUID_128(uuid)->val[10], sizeof(tmp4));
                memcpy(&tmp5, &BT_UUID_128(uuid)->val[12], sizeof(tmp5));

                snprintk(str, len, "%08x%04x%04x%04x%08x%04x",
                         tmp5, tmp4, tmp3, tmp2, tmp1, tmp0);
                break;
        default:
                (void)memset(str, 0, len);
                return;
        }

}

static void svc_attr_data_add(const struct bt_gatt_service_val *gatt_service, u8_t handle, connected_ble_devices* ble_conn_ptr)
{

        char str[UUID_STR_LEN];

        bt_uuid_get_str(gatt_service->uuid, str, sizeof(str));

        bt_to_upper(str, strlen(str));

        ble_conn_mgr_add_uuid_pair(gatt_service->uuid, handle, 0, 0, BT_ATTR_SERVICE, ble_conn_ptr, true);

}

static void chrc_attr_data_add(const struct bt_gatt_chrc *gatt_chrc,  connected_ble_devices* ble_conn_ptr)
{

        u8_t handle = gatt_chrc->value_handle;

        //printk("\tCharacteristic: 0x%s\tProperties: 0x%04X\tHandle: %d\n",str, gatt_chrc->properties, gatt_chrc->value_handle);

        ble_conn_mgr_add_uuid_pair(gatt_chrc->uuid, handle, 1, gatt_chrc->properties, BT_ATTR_CHRC, ble_conn_ptr, false);

}

static void ccc_attr_data_add(const struct bt_gatt_ccc *gatt_ccc, struct bt_uuid* uuid, u8_t handle, connected_ble_devices* ble_conn_ptr)
{

        //printk("\tHandle: %d\n",handle);

        ble_conn_mgr_add_uuid_pair(uuid, handle, 2, 0, BT_ATTR_CCC, ble_conn_ptr, false);

}

/*Add attributes to the connection manager objects*/
static void attr_add(const struct bt_gatt_dm *dm,
                     const struct bt_gatt_attr *attr,
                     connected_ble_devices* ble_conn_ptr)
{
        char str[UUID_STR_LEN];

        bt_uuid_get_str(attr->uuid, str, sizeof(str));

        bt_to_upper(str, strlen(str));

        if ((bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) == 0) ||
            (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_SECONDARY) == 0)) {
                svc_attr_data_add(attr->user_data, attr->handle, ble_conn_ptr);
        } else if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CHRC) == 0) {
                chrc_attr_data_add(attr->user_data, ble_conn_ptr);
        } else if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC) == 0) {
                ccc_attr_data_add(attr->user_data, attr->uuid, attr->handle, ble_conn_ptr);
        }
}

void ble_dm_data_add(const struct bt_gatt_dm *dm)
{

        const struct bt_gatt_attr *attr = NULL;
        char addr_trunc[BT_ADDR_STR_LEN];
        char addr[BT_ADDR_LE_STR_LEN];

        connected_ble_devices* ble_conn_ptr;

        struct bt_conn* conn_obj;
        conn_obj = bt_gatt_dm_conn_get(dm);

        bt_addr_le_to_str(bt_conn_get_dst(conn_obj), addr, sizeof(addr));

        memcpy(addr_trunc, addr, BT_ADDR_LE_DEVICE_LEN);
        addr_trunc[BT_ADDR_LE_DEVICE_LEN] = 0;

        bt_to_upper(addr_trunc, BT_ADDR_LE_STR_LEN);

        //printk("Trunc Addr %s\n", addr_trunc);

        ble_conn_mgr_get_conn_by_addr(addr_trunc, &ble_conn_ptr);

        discover_in_progress = true;

        attr = bt_gatt_dm_service_get(dm);

        attr_add(dm, attr, ble_conn_ptr);

        while (NULL != (attr = bt_gatt_dm_attr_next(dm, attr))) {
                attr_add(dm, attr, ble_conn_ptr);
        }

}

/*Thread resposible for transfering ble data over MQTT*/
void send_notify_data(int unused1, int unused2, int unused3)
{

        memset(uuid, 0, BT_MAX_UUID_LEN);
        memset(path, 0, BT_MAX_PATH_LEN);

        connected_ble_devices* connected_ptr;

        while (1)
        {
                struct rec_data_t *rx_data = k_fifo_get(&rec_fifo, K_NO_WAIT);

                if(rx_data!= NULL)
                {
                        //printk("Trunc Addr %s\n", rx_data->addr_trunc);

                        ble_conn_mgr_get_conn_by_addr(rx_data->addr_trunc, &connected_ptr);

                        if(rx_data->read)
                        {
                                //printk("Value Handle %d\n", rx_data->read_params.single.handle);

                                ble_conn_mgr_get_uuid_by_handle(rx_data->read_params.single.handle, uuid, connected_ptr);

                                ble_conn_mgr_generate_path(connected_ptr, rx_data->read_params.single.handle, path, false);

                                device_chrc_read_encode(rx_data->addr_trunc, uuid, path, ((char *)rx_data->data), rx_data->length);
                        }
                        else
                        {
                                //printk("Value Handle %d\n", rx_data->sub_params.value_handle);

                                ble_conn_mgr_get_uuid_by_handle(rx_data->sub_params.value_handle, uuid, connected_ptr);

                                ble_conn_mgr_generate_path(connected_ptr, rx_data->sub_params.value_handle, path, true);
                                device_value_changed_encode(rx_data->addr_trunc, uuid, path, ((char *)rx_data->data), rx_data->length);
                        }

                        k_free(rx_data);
                }
                k_sleep(50);
        }

}

K_THREAD_DEFINE(rec_thread, SEND_NOTIFY_STACK_SIZE,
                send_notify_data, NULL, NULL, NULL,
                SEND_NOTIFY_PRIORITY, 0, K_NO_WAIT);


static void discovery_completed(struct bt_gatt_dm *disc, void *ctx)
{

        printk("Attribute count: %d\n", bt_gatt_dm_attr_cnt(disc));

        ble_dm_data_add(disc);

        bt_gatt_dm_data_release(disc);

        bt_gatt_dm_continue(disc, NULL);

}

/*Despite the name. This is what is called at the end of a discovery service.*/
static void discovery_service_not_found(struct bt_conn *conn, void *ctx)
{

        printk("Service not found!\n");

        char addr[BT_ADDR_LE_STR_LEN];
        char addr_trunc[BT_ADDR_STR_LEN];
        connected_ble_devices* connected_ptr;

        bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
        memcpy(addr_trunc, addr, BT_ADDR_LE_DEVICE_LEN);
        addr_trunc[BT_ADDR_LE_DEVICE_LEN] = 0;

        bt_to_upper(addr_trunc, BT_ADDR_LE_STR_LEN);

        ble_conn_mgr_get_conn_by_addr(addr_trunc, &connected_ptr);

        connected_ptr->encode_discovered = true;
        connected_ptr->discovered = true;
        connected_ptr->discovering = false;
        discover_in_progress = false;

}

static void discovery_error_found(struct bt_conn *conn, int err, void *ctx)
{
        printk("The discovery procedure failed, err %d\n", err);

        char addr[BT_ADDR_LE_STR_LEN];
        char addr_trunc[BT_ADDR_STR_LEN];
        connected_ble_devices* connected_ptr;

        bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
        memcpy(addr_trunc, addr, BT_ADDR_LE_DEVICE_LEN);
        addr_trunc[BT_ADDR_LE_DEVICE_LEN] = 0;

        bt_to_upper(addr_trunc, BT_ADDR_LE_STR_LEN);

        ble_conn_mgr_get_conn_by_addr(addr_trunc, &connected_ptr);
        connected_ptr->discovering = false;
        connected_ptr->discovered = false;
        discover_in_progress = false;
}

static u8_t gatt_read_callback(struct bt_conn *conn, u8_t err,
                               struct bt_gatt_read_params *params,
                               const void *data, u16_t length)
{

        char addr[BT_ADDR_LE_STR_LEN];
        char addr_trunc[BT_ADDR_STR_LEN];

        printk("GATT Read\n");

        if(length > 0 && data != NULL)
        {
                bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

                printk("Data Addr: %s\n", addr);

                memcpy(addr_trunc, addr, BT_ADDR_LE_DEVICE_LEN);
                addr_trunc[BT_ADDR_LE_DEVICE_LEN] = 0;

                bt_to_upper(addr_trunc, BT_ADDR_LE_STR_LEN);

                printk("Addr %s\n", addr_trunc);

                struct rec_data_t read_data = { .length = length, .read = true};

                memcpy(&read_data.addr_trunc, addr_trunc, strlen(addr_trunc));
                memcpy(&read_data.data, data, length);
                memcpy(&read_data.read_params, params, sizeof(struct bt_gatt_read_params));

                size_t size = sizeof(struct rec_data_t);

                char *mem_ptr = k_malloc(size);


                if(mem_ptr != NULL)
                {

                        memcpy(mem_ptr, &read_data, size);

                        k_fifo_put(&rec_fifo, mem_ptr);

                }

        }

        return BT_GATT_ITER_CONTINUE;

}

void gatt_read(char* ble_addr, char* chrc_uuid)
{

        int err;

        static struct bt_gatt_read_params params;
        struct bt_conn *conn;
        bt_addr_le_t addr;
        connected_ble_devices* connected_ptr;
        u8_t handle;

        ble_conn_mgr_get_conn_by_addr(ble_addr, &connected_ptr);

        err = ble_conn_mgr_get_handle_by_uuid(&handle, chrc_uuid, connected_ptr);

        if (err) {
                printk("Could not find handle\n");
                goto end;
        }

        params.handle_count = 1;
        params.single.handle = handle;
        params.func = gatt_read_callback;

        err = bt_addr_le_from_str(ble_addr, "random", &addr);
        if (err) {
                printk("Address from string failed (err %d)\n", err);
        }

        conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &addr);
        if (conn == NULL) {
                //printk("Null Conn object (err)\n");
                goto end;
        }

        bt_gatt_read(conn, &params);

end:
        printk("End\n");
        //bt_conn_unref(conn);

}


static void on_sent(struct bt_conn *conn, u8_t err,
                    struct bt_gatt_write_params *params)
{
        const void *data;
        u16_t length;

        /* Make a copy of volatile data that is required by the callback. */
        data = params->data;
        length = params->length;

        printk("Sent Data of Length: %d\n", length);
}

void gatt_write(char* ble_addr, char* chrc_uuid, u8_t* data, u16_t data_len)
{

        int err;
        struct bt_conn *conn;
        bt_addr_le_t addr;
        connected_ble_devices* connected_ptr;
        u8_t handle;
        static struct bt_gatt_write_params params;

        ble_conn_mgr_get_conn_by_addr(ble_addr, &connected_ptr);

        ble_conn_mgr_get_handle_by_uuid(&handle, chrc_uuid, connected_ptr);

        err = bt_addr_le_from_str(ble_addr, "random", &addr);
        if (err) {
                printk("Address from string failed (err %d)\n", err);
        }

        conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &addr);
        if (conn == NULL) {
                printk("Null Conn object (err)\n");
                goto end;
        }

        for(int i =0; i< data_len; i++)
        {
                printk("Writing: %x\n", data[i]);
        }

        printk("Writing to addr: %s to chrc %s with handle %d\n:", ble_addr, chrc_uuid, handle);

        params.func = on_sent;
        params.handle = handle;
        params.offset = 0;
        params.data = data;
        params.length = data_len;

        bt_gatt_write(conn, &params);


//TODO: Add function for write without response.
//  bt_gatt_write_without_response(conn, handle, data, data_len, false);

end:

        printk("GATT Write end\n");

}


void rec_timer_handler(struct k_timer *timer)
{

        ok_to_send = true;

}
K_TIMER_DEFINE(rec_timer, rec_timer_handler, NULL);


static u8_t on_received(struct bt_conn *conn,
                        struct bt_gatt_subscribe_params *params,
                        const void *data, u16_t length)
{

        char addr[BT_ADDR_LE_STR_LEN];
        char addr_trunc[BT_ADDR_STR_LEN];

        if (!data)
        {
                return BT_GATT_ITER_STOP;
        }

        u32_t lock = irq_lock();

        if(length>0 && data !=NULL && ok_to_send)
        {

                bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

                //printk("Data Addr: %s\n", addr);

                memcpy(addr_trunc, addr, BT_ADDR_LE_DEVICE_LEN);
                addr_trunc[BT_ADDR_LE_DEVICE_LEN] = 0;

                bt_to_upper(addr_trunc, BT_ADDR_LE_STR_LEN);

                //printk("Trunc Addr %s\n", addr_trunc);

                struct rec_data_t tx_data = { .length = length};

                memcpy(&tx_data.addr_trunc, addr_trunc, strlen(addr_trunc));
                memcpy(&tx_data.data, data, length);
                memcpy(&tx_data.sub_params, params, sizeof(struct bt_gatt_subscribe_params));

                size_t size = sizeof(struct rec_data_t);

                char *mem_ptr = k_malloc(size);

                if(mem_ptr != NULL)
                {

                        memcpy(mem_ptr, &tx_data, size);
                        k_fifo_put(&rec_fifo, mem_ptr);

                }

                k_timer_start(&rec_timer, K_MSEC(500), 0); //Timer to limit the amount of data we can send. Some characteristics notify faster than can be processed.
                ok_to_send = false;
        }

        irq_unlock(lock);



        return BT_GATT_ITER_CONTINUE;

}


void ble_subscribe(char* ble_addr, char* chrc_uuid, u8_t value_type)
{

        int err;
        static int index = 0;
        static u8_t curr_subs = 0;
        /* Must be statically allocated */
        static struct bt_gatt_subscribe_params param[BT_MAX_SUBSCRIBES];
        struct bt_conn *conn;
        bt_addr_le_t addr;
        connected_ble_devices* connected_ptr;
        u8_t handle;
        bool subscribed;
        u8_t param_index;

        ble_conn_mgr_get_conn_by_addr(ble_addr, &connected_ptr);

        ble_conn_mgr_get_handle_by_uuid(&handle, chrc_uuid, connected_ptr);

        ble_conn_mgr_get_subscribed(handle, connected_ptr, &subscribed, &param_index);


        param[index].notify = on_received;

        param[index].value = BT_GATT_CCC_NOTIFY;

        if(value_type == BT_GATT_CCC_INDICATE)
        {
                param[index].value = BT_GATT_CCC_INDICATE;
        }

        param[index].value_handle = handle;
        param[index].ccc_handle = handle+1;

        printk("Subscribing Address: %s\n", ble_addr);
        printk("Value Handle: %d\n", param[index].value_handle);
        printk("CCC Handle: %d\n", param[index].ccc_handle);

        err = bt_addr_le_from_str(ble_addr, "random", &addr);
        if (err) {
                printk("Address from string failed (err %d)\n", err);
        }

        conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &addr);
        if (conn == NULL) {
                printk("Null Conn object (err %d)\n", err);
                goto end;
        }

        ble_conn_mgr_generate_path(connected_ptr, handle, path, true);

        if(!subscribed && curr_subs < MAX_SUBSCRIPTIONS)
        {
                err = bt_gatt_subscribe(conn, &param[index]);
                if (err) {
                        printk("Subscribe failed (err %d)\n", err);
                        goto end;
                }

                ble_conn_mgr_set_subscribed(handle, index, connected_ptr);

                u8_t value[2] = {1,0};
                device_descriptor_value_changed_encode(ble_addr, "2902", path, value, 2);
                device_value_write_result_encode(ble_addr, "2902", path, value, 2);


end:

                printk("Subscribed to %d\n", handle+1);
                //bt_conn_unref(conn);
                curr_subs++;
                index++;
        }
        else if(subscribed) //If subscribed then unsubscribe.
        {

                bt_gatt_unsubscribe(conn, &param[param_index]);
                printk("Unsubscribed to %d\n", handle+1);
                ble_conn_mgr_remove_subscribed(handle, connected_ptr);

                u8_t value[2] = {0,0};
                device_descriptor_value_changed_encode(ble_addr, "2902", path, value, 2);
                device_value_write_result_encode(ble_addr, "2902", path, value, 2);
                curr_subs--;
        }

        else if(curr_subs >= MAX_SUBSCRIPTIONS) //Send error when limit is reached.
        {

                device_error_encode(ble_addr, "Reached subscription limit of 4");

        }
}

static struct bt_gatt_dm_cb discovery_cb = {
        .completed = discovery_completed,
        .service_not_found = discovery_service_not_found,
        .error_found = discovery_error_found,
};


u8_t ble_discover(char* ble_addr)
{

        int err;
        struct bt_conn *conn;
        bt_addr_le_t addr;
        connected_ble_devices* connection_ptr;

        if(!discover_in_progress)
        {

                err = bt_addr_le_from_str(ble_addr, "random", &addr);
                if (err) {
                        printk("Address from string failed (err %d)\n", err);
                        return err;
                }


                conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &addr);
                if (conn == NULL) {
                        //printk("Null Conn object (err %d)\n", err);
                        return 1;
                }

                ble_conn_mgr_get_conn_by_addr(ble_addr, &connection_ptr);

                if(!connection_ptr->discovered)
                {
                        err = bt_gatt_dm_start(conn, NULL, &discovery_cb, NULL);
                        if (err) {
                                printk("Could not start service discovery, err %d\n", err);
                                connection_ptr->discovering = false;
                                return err;
                        }
                        connection_ptr->discovering = true;
                }
                else
                {
                        connection_ptr->encode_discovered = true;
                }
        }
        else
        {
                return 1;
        }
        return err;

}

static void connected(struct bt_conn *conn, u8_t conn_err)
{

        int err;
        char addr[BT_ADDR_LE_STR_LEN];
        char addr_trunc[BT_ADDR_STR_LEN];

        bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

        memcpy(addr_trunc, addr, BT_ADDR_LE_DEVICE_LEN);
        addr_trunc[BT_ADDR_LE_DEVICE_LEN] = 0;

        bt_to_upper(addr_trunc, BT_ADDR_LE_STR_LEN);

        if (conn_err) {

                printk("Failed to connect to %s (%u)\n", addr, conn_err);
                ble_conn_set_connected(addr_trunc, false);
                bt_conn_unref(conn);
                return;
        }

        printk("Connected: %s\n", addr);

        device_connect_result_encode(addr_trunc, true);
        device_shadow_data_encode(addr_trunc, false, true);

        ble_conn_set_connected(addr_trunc, true);

        //Restart to scanning for whitelisted devices
        err = bt_conn_create_auto_le(BT_LE_CONN_PARAM_DEFAULT);

        if (err) {
                printk("Connection exists\n");
        } else {

                printk("Connection creation pending\n");
        }

}

static void disconnected(struct bt_conn *conn, u8_t reason)
{

        char addr[BT_ADDR_LE_STR_LEN];
        int err;
        char addr_trunc[BT_ADDR_STR_LEN];


        bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

        memcpy(addr_trunc, addr, BT_ADDR_LE_DEVICE_LEN);
        addr_trunc[BT_ADDR_LE_DEVICE_LEN] = 0;

        bt_to_upper(addr_trunc, BT_ADDR_LE_STR_LEN);

        device_connect_result_encode(addr_trunc, false);
        device_shadow_data_encode(addr_trunc, false, false);

        ble_conn_set_disconnected(addr_trunc);

        printk("Disconnected: %s (reason 0x%02x)\n", addr, reason);

        bt_conn_unref(conn);

        //Restart to scanning for whitelisted devices
        err = bt_conn_create_auto_le(BT_LE_CONN_PARAM_DEFAULT);

        if (err) {
                printk("Connection exists\n");
        } else {

                printk("Connection creation pending\n");
        }

}

static struct bt_conn_cb conn_callbacks = {
        .connected = connected,
        .disconnected = disconnected,
};

void scan_filter_match(struct bt_scan_device_info *device_info,
                       struct bt_scan_filter_match *filter_match,
                       bool connectable)
{

        char addr[BT_ADDR_LE_STR_LEN];

        bt_addr_le_to_str(device_info->addr, addr, sizeof(addr));

        printk("Device found: %s\n", addr);

}

void scan_connecting_error(struct bt_scan_device_info *device_info)
{
        printk("Connection to peer failed!\n");
}


static bool data_cb(struct bt_data *data, void *user_data)
{

        char *name = user_data;

        switch (data->type) {
        case BT_DATA_NAME_SHORTENED:
        case BT_DATA_NAME_COMPLETE:
                memcpy(name, data->data, MIN(data->data_len, NAME_LEN - 1));
                return false;
        default:
                return true;
        }

}



void ble_device_found_enc_handler(struct k_work *work)
{

        device_found_encode(num_devices_found);

}

K_WORK_DEFINE(ble_device_encode_work, ble_device_found_enc_handler);


static void device_found(const bt_addr_le_t *addr, s8_t rssi, u8_t type,
                         struct net_buf_simple *ad)
{
        char addr_str[BT_ADDR_LE_STR_LEN];
        char name[NAME_LEN] = {0};
        bool dup_addr = false;

        (void)memset(name, 0, sizeof(name));

        bt_data_parse(ad, data_cb, name);

        /* We're only interested in connectable events */
        if (type != BT_LE_ADV_IND && type != BT_LE_ADV_DIRECT_IND) {
                return;
        }

        bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

        memcpy(ble_scanned_device[num_devices_found].type, addr_str+BT_ADDR_LE_DEVICE_LEN_SHIFT, BT_ADDR_LE_TYPE_LEN);
        ble_scanned_device[num_devices_found].type[BT_ADDR_LE_TYPE_LEN] = 0;

        bt_to_upper(addr_str, BT_ADDR_LE_STR_LEN);

        memcpy(ble_scanned_device[num_devices_found].addr, addr_str, BT_ADDR_LE_DEVICE_LEN);
        ble_scanned_device[num_devices_found].addr[BT_ADDR_LE_DEVICE_LEN] = 0;
        ble_scanned_device[num_devices_found].rssi = (int)rssi;
        memcpy(ble_scanned_device[num_devices_found].name, name, strlen(name));

        /* Check for dupclicate addresses */
        for(int j=0; j<num_devices_found; j++)
        {
                if(!(strcmp(ble_scanned_device[num_devices_found].addr, ble_scanned_device[j].addr)))
                {
                        dup_addr = true;
                }
        }

        if((num_devices_found < MAX_SCAN_RESULTS) && (dup_addr == false))
        {
                printk("Device found: %s (RSSI %d)\n", ble_scanned_device[num_devices_found].addr, rssi);
                printk("Device Name: %s\n", ble_scanned_device[num_devices_found].name);
                printk("Type: %s\n", ble_scanned_device[num_devices_found].type);

                num_devices_found++;
        }
}

void scan_off_handler(struct k_work *work)
{
        int err;

        err = bt_le_scan_stop();
        if (err) {
                printk("Stopping scanning failed (err %d)\n", err);
        } else {
                printk("Scan successfully stopped\n");
        }

        //Restart the scanning
        err = bt_conn_create_auto_le(BT_LE_CONN_PARAM_DEFAULT);

        if (err) {
                printk("Connection exists\n");
        } else {
                printk("Connection creation pending\n");
        }

        k_work_submit(&ble_device_encode_work);

}

K_WORK_DEFINE(scan_off_work, scan_off_handler);

void scan_timer_handler(struct k_timer *timer)
{

        k_work_submit(&scan_off_work);

}

K_TIMER_DEFINE(scan_timer, scan_timer_handler, NULL);

void ble_add_to_whitelist(char* addr_str, char* conn_type)
{

        int err;
        bt_addr_le_t addr;

        printk("Whitelisting Address: %s\n", addr_str);
        printk("Whitelisting Address Type: %s\n", conn_type);

        err = bt_addr_le_from_str(addr_str, conn_type, &addr);
        if (err) {
                printk("Invalid peer address (err %d)\n", err);
        }

        bt_conn_create_auto_stop();

        bt_le_whitelist_add(&addr);

        err = bt_conn_create_auto_le(BT_LE_CONN_PARAM_DEFAULT);

        if (err) {
                printk("Connection exists\n");
        } else {
                printk("Connection creation pending\n");
        }

}



void scan_start(void)
{

        int err;

        num_devices_found = 0;

        struct bt_le_scan_param param = {
                .type       = BT_HCI_LE_SCAN_ACTIVE,
                .filter_dup = BT_HCI_LE_SCAN_FILTER_DUP_ENABLE,
                .interval   = BT_GAP_SCAN_FAST_INTERVAL,
                .window     = BT_GAP_SCAN_FAST_WINDOW
        };

        //Stop the auto connect
        bt_conn_create_auto_stop();

        err = bt_le_scan_start(&param, device_found);
        if (err) {
                printk("Bluetooth set active scan failed "
                       "(err %d)\n", err);
        } else {
                printk("Bluetooth active scan enabled\n");

                k_timer_start(&scan_timer, K_SECONDS(5), 0);      //TODO: Get scan timeout from scan message
        }

}

static void ble_ready(int err)
{

        printk("Bluetooth ready\n");

        bt_conn_cb_register(&conn_callbacks);

}

void ble_init(void)
{
        int err;

        printk("Initializing Bluetooth..\n");
        err = bt_enable(ble_ready);
        if (err) {
                printk("Bluetooth init failed (err %d)\n", err);
                return;
        }

        for(int i = 0; i< MAX_SCAN_RESULTS; i++)
        {
                memset(ble_scanned_device[i].name, 0, sizeof(ble_scanned_device[1].name));
        }
}
