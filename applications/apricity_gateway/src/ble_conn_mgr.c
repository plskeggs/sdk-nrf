#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include "ble_conn_mgr.h"
#include "ble_codec.h"
#include "cJSON.h"
#include "ble.h"

static connected_ble_devices connected_ble_device[CONFIG_BT_MAX_CONN];

#define CONN_MGR_STACK_SIZE 2048
#define CONN_MGR_PRIORITY 1


void connection_manager(int unused1, int unused2, int unused3)
{

        int err;
        ble_conn_mgr_init();
        while (1)
        {

                for(int i = 0; i<CONFIG_BT_MAX_CONN; i++)
                {
                        //Manager is busy. Do nothing.
                        if(connected_ble_device[i].discovering)
                        {
                                printk("Connection work busy.\n");
                                goto end;
                        }
                }

                for(int i = 0; i<CONFIG_BT_MAX_CONN; i++)
                {

                        if(connected_ble_device[i].free == false)
                        {
                                //Add devices to whitelist
                                if(!connected_ble_device[i].added_to_whitelist)
                                {
                                        ble_add_to_whitelist(connected_ble_device[i].addr, connected_ble_device[i].addr_type);
                                        connected_ble_device[i].added_to_whitelist = true;
                                        device_shadow_data_encode(connected_ble_device[i].addr, true, false); //Maybe not the right spot to send this. IDK.
                                        printk("Device added to whitelist.\n");

                                }

                                //Not connected. Update the shadow
                                if(connected_ble_device[i].connected == true && !connected_ble_device[i].shadow_updated)
                                {
                                        device_shadow_data_encode(connected_ble_device[i].addr, false, true); //Maybe not the right spot to send this. IDK.
                                        connected_ble_device[i].shadow_updated = true;
                                }

                                //Connected. Do discoverying if not discoveryed or currently discovering
                                if( connected_ble_device[i].connected == true && connected_ble_device[i].discovered == false)
                                {

                                        err = ble_discover(connected_ble_device[i].addr);

                                        if(!err)
                                        {
                                                connected_ble_device[i].discovered = true;
                                        }

                                }

                                //Discoverying done. Encode and send.
                                if(connected_ble_device[i].connected == true && connected_ble_device[i].encode_discovered == true)
                                {

                                        u32_t lock = irq_lock(); //I don't know why this is needed in a work thread. TODO: FIX

                                        connected_ble_device[i].encode_discovered = false;
                                        device_discovery_encode(&connected_ble_device[i]);

                                        irq_unlock(lock);

                                        device_shadow_data_encode(connected_ble_device[i].addr, false, true); //Maybe not the right spot to send this. IDK.

                                }
                        }

                }

end:
                k_sleep(1000);
        }

}

K_THREAD_DEFINE(conn_mgr_thread, CONN_MGR_STACK_SIZE,
                connection_manager, NULL, NULL, NULL,
                CONN_MGR_PRIORITY, 0, K_NO_WAIT);


void ble_conn_mgr_generate_path(connected_ble_devices* conn_ptr, u8_t handle, char* path, bool ccc)
{

        char path_str[BT_MAX_PATH_LEN];
        char temp_str[BT_MAX_PATH_LEN];
        char service_uuid[BT_UUID_STR_LEN];
        char ccc_uuid[BT_UUID_STR_LEN];
        char chrc_uuid[BT_UUID_STR_LEN];

        u8_t path_depth = 0;
        int pos = 0;
        u8_t num_bytes;

        //bt_uuid_get_str(&conn_ptr->uuid_handle_pair[chrc_location+i].uuid_128.uuid, uuid, BT_MAX_UUID_LEN);

        printk("Num Pairs: %d\n", conn_ptr->num_pairs);

        for(int i=0; i< conn_ptr->num_pairs; i++)
        {

                if(handle == conn_ptr->uuid_handle_pair[i].handle)
                {

                        path_depth = conn_ptr->uuid_handle_pair[i].path_depth;
                        printk("Path Depth %d\n", path_depth);

                        bt_uuid_get_str(&conn_ptr->uuid_handle_pair[i].uuid_128.uuid, chrc_uuid, BT_MAX_UUID_LEN);
                        bt_uuid_get_str(&conn_ptr->uuid_handle_pair[i+1].uuid_128.uuid, ccc_uuid, BT_MAX_UUID_LEN);

                        for(int j = i; j>=0; j--)
                        {
                                if(conn_ptr->uuid_handle_pair[j].is_service)
                                {
                                        bt_uuid_get_str(&conn_ptr->uuid_handle_pair[j].uuid_128.uuid, service_uuid, BT_MAX_UUID_LEN);
                                        //printk("service uuid in path %s\n", service_uuid);
                                        break;
                                }
                        }

                        snprintk(path_str, BT_MAX_PATH_LEN, "%s/%s", service_uuid, chrc_uuid);

                        if(ccc)
                        {
                                snprintk(path_str, BT_MAX_PATH_LEN, "%s/%s/%s", service_uuid, chrc_uuid, ccc_uuid);
                        }
                }
        }

        bt_to_upper(path_str, strlen(path_str));
        memset(path, 0, BT_MAX_PATH_LEN);
        memcpy(path, path_str, strlen(path_str));

        printk("Generated Path: %s\n", path_str);

}

u8_t ble_conn_mgr_add_conn(char* addr, char* addr_type)
{

        int err = 0;

        connected_ble_devices* connected_ble_ptr;

        //Check if already added
        for(int i = 0; i<CONFIG_BT_MAX_CONN; i++)
        {
                if(connected_ble_device[i].free == false)
                {
                        if(!strcmp(addr, connected_ble_device[i].addr))
                        {
                                printk("Connection already exsists\n");
                                return 1;
                        }
                }
        }

        err = ble_conn_mgr_get_free_conn(&connected_ble_ptr);

        if(err)
        {
                printk("No free connections\n");
                return err;
        }

        memcpy(connected_ble_ptr->addr, addr, DEVICE_ADDR_LEN);
        memcpy(connected_ble_ptr->addr_type, addr_type, DEVICE_ADDR_TYPE_LEN);
        connected_ble_ptr->free = false;

        printk("Ble conn added to manager\n");
        return err;

}

u8_t ble_conn_set_connected(char* addr, bool connected)
{

        int err = 0;
        connected_ble_devices* connected_ble_ptr;

        err = ble_conn_mgr_get_conn_by_addr(addr, &connected_ble_ptr);

        if(err)
        {
                printk("Conn not found\n");
                return err;
        }

        if(connected)
        {
                connected_ble_ptr->connected = true;
        }
        else
        {
                connected_ble_ptr->connected = false;
        }
        printk("Conn updated\n");
        return err;

}

u8_t ble_conn_set_disconnected(char* addr)
{
        int err = 0;
        connected_ble_devices* connected_ble_ptr;

        err = ble_conn_mgr_get_conn_by_addr(addr, &connected_ble_ptr);

        if(err)
        {
                printk("Can't find conn to disconnect\n");
                return err;
        }

        connected_ble_ptr->num_pairs = 0;
        connected_ble_ptr->connected = false;
        connected_ble_ptr->discovered = false; //Should we need to discover again on reconnect?
        connected_ble_ptr->shadow_updated = false;
        printk("Conn Disconnected\n");
        return err;
}

u8_t ble_conn_mgr_remove_conn(char* addr)
{
        int err = 0;
        connected_ble_devices* connected_ble_ptr;

        err = ble_conn_mgr_get_conn_by_addr(addr, &connected_ble_ptr);

        if(err)
        {
                printk("Can't find conn to remove\n");
                return err;
        }

        memset(connected_ble_ptr, 0, sizeof(connected_ble_devices));

        connected_ble_ptr->free = true;
        connected_ble_ptr->connected = false;
        connected_ble_ptr->discovered = false;
        printk("Conn Removed\n");
        return err;
}


u8_t ble_conn_mgr_get_free_conn(connected_ble_devices** conn_ptr)
{

        for(int i = 0; i<CONFIG_BT_MAX_CONN; i++)
        {
                if(connected_ble_device[i].free == true)
                {
                        *conn_ptr = &connected_ble_device[i];
                        printk("Found Free connection: %d\n", i);
                        return 0;
                }
        }

        return 1;
}


u8_t ble_conn_mgr_get_conn_by_addr(char* addr, connected_ble_devices** conn_ptr)
{

        int err = 0;


        for(int i = 0; i<CONFIG_BT_MAX_CONN; i++)
        {
                if(!strcmp(addr, connected_ble_device[i].addr))
                {
                        *conn_ptr = &connected_ble_device[i];
                        printk("Conn Found\n");
                        return 0;
                }
        }

        printk("No Conn Found\n");
        return 1;

}

u8_t ble_conn_mgr_set_subscribed(u8_t handle, u8_t sub_index, connected_ble_devices* conn_ptr)
{

        for(int i=0; i< conn_ptr->num_pairs; i++)
        {
                if(handle == conn_ptr->uuid_handle_pair[i].handle)
                {
                        conn_ptr->uuid_handle_pair[i].sub_enabled = true;
                        conn_ptr->uuid_handle_pair[i].sub_index = sub_index;

                        return 0;
                }
        }

        return 1;
}


u8_t ble_conn_mgr_remove_subscribed(u8_t handle, connected_ble_devices* conn_ptr)
{

        for(int i=0; i< conn_ptr->num_pairs; i++)
        {
                if(handle == conn_ptr->uuid_handle_pair[i].handle)
                {
                        conn_ptr->uuid_handle_pair[i].sub_enabled = false;
                        return 0;
                }
        }

        return 1;
}


u8_t ble_conn_mgr_get_subscribed(u8_t handle, connected_ble_devices* conn_ptr, bool* status, u8_t* sub_index)
{

        for(int i=0; i< conn_ptr->num_pairs; i++)
        {
                if(handle == conn_ptr->uuid_handle_pair[i].handle)
                {
                        *status = conn_ptr->uuid_handle_pair[i].sub_enabled;
                        *sub_index = conn_ptr->uuid_handle_pair[i].sub_index;
                        return 0;
                }
        }

        return 1;
}

u8_t ble_conn_mgr_get_uuid_by_handle(u8_t handle, char* uuid, connected_ble_devices* conn_ptr)
{

        char uuid_str[BT_UUID_STR_LEN];

        memset(uuid, 0, BT_UUID_STR_LEN);

        for(int i=0; i< conn_ptr->num_pairs; i++)
        {
                if(handle == conn_ptr->uuid_handle_pair[i].handle)
                {
                        bt_uuid_get_str(&conn_ptr->uuid_handle_pair[i].uuid_128.uuid, uuid_str, BT_MAX_UUID_LEN);
                        bt_to_upper(uuid_str, strlen(uuid_str));
                        memcpy(uuid, uuid_str, strlen(uuid_str));
                        printk("Found UUID: %s For Handle: %d\n", uuid_str, handle);
                        return 0;
                }
        }

        printk("Handle Not Found\n");

        return 1;

}


u8_t ble_conn_mgr_get_handle_by_uuid(u8_t* handle, char* uuid, connected_ble_devices* conn_ptr)
{

        char str[BT_UUID_STR_LEN];

        //printk("Num Pairs: %d\n", conn_ptr->num_pairs);

        for(int i=0; i< conn_ptr->num_pairs; i++)
        {

                bt_uuid_get_str(&conn_ptr->uuid_handle_pair[i].uuid_16.uuid, str, sizeof(str));
                bt_to_upper(str, strlen(str));
                //printk("UUID IN: %s UUID FOUND: %s\n", uuid, str);

                if(!strcmp(uuid, str))
                {
                        *handle = conn_ptr->uuid_handle_pair[i].handle;
                        //printk("16 Bit UUID Found\n");
                        return 0;
                }

                bt_uuid_get_str(&conn_ptr->uuid_handle_pair[i].uuid_128.uuid, str, sizeof(str));
                bt_to_upper(str, strlen(str));
                //printk("UUID IN: %s UUID FOUND: %s\n", uuid, str);
                if(!strcmp(uuid, str))
                {
                        *handle = conn_ptr->uuid_handle_pair[i].handle;
                        //printk("128 Bit UUID Found\n");
                        return 0;
                }

        }

        printk("Handle Not Found\n");

        return 1;
}

u8_t ble_conn_mgr_add_uuid_pair(struct bt_uuid *uuid, u8_t handle, u8_t path_depth, u8_t properties, u8_t attr_type, connected_ble_devices* conn_ptr, bool is_service)
{

        int err = 0;

        char str[BT_UUID_STR_LEN];

        if(conn_ptr->num_pairs >= MAX_UUID_PAIRS)
        {
                printk("Max uuid pair limit reached\n");
                return 1;
        }

        printk("Handle Added: %d\n", handle);

        if (!uuid) {

                return NULL;
        }

        switch (uuid->type) {
        case BT_UUID_TYPE_16:
                //printk("16 Bit Type\n");
                memcpy(&conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].uuid_16, BT_UUID_16(uuid), sizeof(struct bt_uuid_16));
                conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].uuid_type = BT_UUID_TYPE_16;
                bt_uuid_get_str(&conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].uuid_16.uuid, str, sizeof(str));

                printk("\tCONN MGR Characteristic: 0x%s\n",str);
                break;
        case BT_UUID_TYPE_128:
                //printk("128 Bit Type\n");
                memcpy(&conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].uuid_128, BT_UUID_128(uuid), sizeof(struct bt_uuid_128));
                conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].uuid_type = BT_UUID_TYPE_128;
                bt_uuid_get_str(&conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].uuid_128.uuid, str, sizeof(str));

                printk("\tCONN MGR Characteristic: 0x%s\n",str);
                break;
        default:

                return NULL;
        }

        conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].properties = properties;
        conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].attr_type = attr_type;
        conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].path_depth = path_depth;
        conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].is_service = is_service;
        conn_ptr->uuid_handle_pair[conn_ptr->num_pairs].handle = handle;

        conn_ptr->num_pairs++;

        //printk("UUID pair added\n");

        return err;
}

void ble_conn_mgr_init()
{

        for(int i = 0; i<CONFIG_BT_MAX_CONN; i++)
        {
                connected_ble_device[i].free = true;
                connected_ble_device[i].added_to_whitelist = false;
                connected_ble_device[i].connected = false;
                connected_ble_device[i].discovered = false;
                connected_ble_device[i].encode_discovered = false;

        }

}