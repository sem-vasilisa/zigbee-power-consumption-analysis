#include <zboss_api.h> // the Zigbee stack itself (ZBOSS) - joining network, sending packets, etc.
#include <zephyr/kernel.h>  // Zephyr RTOS - gives you k_sleep, threads, etc.
#include "zb_sensor_node.h"
#include <zboss_api_addons.h> // extra helper macros/functions on top of zboss_api.h
#include <zephyr/logging/log.h>
// #include <zephyr/drivers/gpio.h>
#include <zigbee/zigbee_app_utils.h> // Nordic's helper functions (zigbee_enable, sleepy behavior, etc.)
#include <zigbee/zigbee_error_handler.h> // ZB_ERROR_CHECK macro
#include <zb_nrf_platform.h> // zigbee_enable()

LOG_MODULE_REGISTER(btz, LOG_LEVEL_INF);

#define LIGHT_BULB_ENDPOINT 10
// #define LED_NODE DT_ALIAS(led0)

#define COORDINATOR_SHORT_ADDR 0x0000
#define COORDINATOR_EP 10
zb_uint16_t coord_short_addr = COORDINATOR_SHORT_ADDR; /* coordinator address we need for communication */

// static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

struct zb_device_ctx{
    zb_zcl_basic_attrs_ext_t basic_attr;
    zb_zcl_identify_attrs_t identify_attr;
    zb_zcl_on_off_attrs_t on_off_attr;
};

static struct zb_device_ctx dev_ctx;

static void app_clusters_attr_init(void){
    dev_ctx.basic_attr.zcl_version = ZB_ZCL_VERSION;
    dev_ctx.basic_attr.power_source =  ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
    dev_ctx.identify_attr.identify_time = ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;
    dev_ctx.on_off_attr.on_off = ZB_FALSE;
}

/* attribute lists*/
ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST(basic_attrib_list, &dev_ctx.basic_attr.zcl_version, &dev_ctx.basic_attr.power_source);
ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(identify_attr_list,&dev_ctx.identify_attr.identify_time);
ZB_ZCL_DECLARE_ON_OFF_ATTRIB_LIST(on_off_attr_list, &dev_ctx.on_off_attr.on_off);

/* cluster lists */
ZB_DECLARE_LIGHT_BULB_CLUSTER_LIST(light_bulb_clusters, basic_attrib_list, identify_attr_list, on_off_attr_list);

/* endpoint */
ZB_DECLARE_LIGHT_BULB_EP(light_bulb_ep, LIGHT_BULB_ENDPOINT, light_bulb_clusters);

/* device context */
ZBOSS_DECLARE_DEVICE_CTX_1_EP(light_bulb_ctx, light_bulb_ep);

// static void on_off_set_value(zb_bool_t value){
//     dev_ctx.on_off_attr.on_off = value;
//     if(value){
//         LOG_INF("LED IS OFF!");
//         gpio_pin_set_dt(&led, 1);
//     }
//     else{
//         LOG_INF("LED IS ON!");
//         gpio_pin_set_dt(&led, 0);
//     }
// }

static zb_uint8_t zcl_device_cb(zb_bufid_t bufid){
    zb_zcl_device_callback_param_t *p = ZB_BUF_GET_PARAM(bufid, zb_zcl_device_callback_param_t); /* returns a pointer to the data in the buffer */
    p->status = RET_OK;

    if(p->device_cb_id != ZB_ZCL_SET_ATTR_VALUE_CB_ID){
        return ZB_FALSE;
    }

    // zb_uint16_t cluster_id = p->cb_param.set_attr_value_param.cluster_id;
    // zb_uint16_t attr_id = p->cb_param.set_attr_value_param.attr_id;

    // if(cluster_id == ZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID){
    //     zb_uint8_t value = p->cb_param.set_attr_value_param.values.data8;
    //     if(value){
    //         on_off_set_value(ZB_TRUE);
    //     }
    //     else{
    //         on_off_set_value(ZB_FALSE);
    //     }
    // }
    return ZB_FALSE;
}

void zboss_signal_handler(zb_bufid_t bufid){
    zb_zdo_app_signal_hdr_t *sg_p  = NULL;
    zb_zdo_app_signal_type_t  sig  = zb_get_app_signal(bufid, &sg_p);
    zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(bufid);

    switch(sig){
        case ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            LOG_INF("Joining network for the first time...");
            bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            break;

        case ZB_BDB_SIGNAL_DEVICE_REBOOT:
            zb_zdo_pim_set_long_poll_interval(CONFIG_ZB_POLL_INTERVAL_S * 1000);
            bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            break;

        case ZB_BDB_SIGNAL_STEERING:
            if(status == RET_OK){
                zb_zdo_pim_set_long_poll_interval(CONFIG_ZB_POLL_INTERVAL_S * 1000);
                uint16_t panId = zb_get_pan_id();
                uint8_t channel = zb_get_current_channel();
                uint16_t shortAddr = zb_get_short_address();

                LOG_INF("Joined a network: PAN ID=0x%04X, Channel=%u, Short Addr=0x%04X", panId, channel, shortAddr);
            }
            else{
                /* ZB network unit of time is BI(beacon interval), ZB devices use it to schedule operations. Beacons because zb in built on ieee802.15.4 which measures time in beacon intervals */
                ZB_SCHEDULE_APP_ALARM((zb_callback_t)bdb_start_top_level_commissioning, ZB_BDB_NETWORK_STEERING, ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000)); /* after delay call this function */
            }
            break;

        /* if an end device leaves the zigbee network */
        case ZB_ZDO_SIGNAL_LEAVE:
            LOG_INF("Reconnecting the network...");
            ZB_SCHEDULE_APP_ALARM((zb_callback_t)bdb_start_top_level_commissioning, ZB_BDB_NETWORK_STEERING, ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000)); /* after delay call this function */
            break;

        default:
            ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
            break;
    }
    if (bufid) {
        zb_buf_free(bufid);
    }
}

/* builds and sends the frame directly and schedule it in the loop via alarm */
static void send_test_report(zb_uint8_t param)
{
    zb_bufid_t bufid = zb_buf_get_out(); /* free memory buffer */
    if (!bufid) {
        LOG_ERR("No buffer available");
        ZB_SCHEDULE_APP_ALARM(send_test_report, 0, ZB_MILLISECONDS_TO_BEACON_INTERVAL(60000));
        return;
    }

    zb_uint8_t *ptr = ZB_ZCL_START_PACKET(bufid); /* write into that buffer */
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_REQ_FRAME_CONTROL_A(ptr, ZB_ZCL_FRAME_DIRECTION_TO_CLI, ZB_ZCL_NOT_MANUFACTURER_SPECIFIC, ZB_ZCL_DISABLE_DEFAULT_RESPONSE); /* writes control byte */
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER(ptr, ZB_ZCL_GET_SEQ_NUM(), ZB_ZCL_CMD_REPORT_ATTRIB); /* the following header bytes, frame is a report attribute cmd */

    zb_uint8_t test_value = ZB_TRUE; /* the const test value */

    ZB_ZCL_PACKET_PUT_DATA16_VAL(ptr, ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID); /* which attribute */
    ZB_ZCL_PACKET_PUT_DATA8(ptr, ZB_ZCL_ATTR_TYPE_BOOL); /* what data type */
    ZB_ZCL_PACKET_PUT_DATA8(ptr, test_value); /* the actual value */

    ZB_ZCL_SEND_COMMAND_SHORT_WITHOUT_ACK(bufid, ptr, coord_short_addr, ZB_APS_ADDR_MODE_16_ENDP_PRESENT, COORDINATOR_EP, LIGHT_BULB_ENDPOINT, ZB_AF_HA_PROFILE_ID, ZB_ZCL_CLUSTER_ID_ON_OFF, NULL, 0);
    LOG_INF("Sent test report"); /* locally we see that frame was sent */
    ZB_SCHEDULE_APP_ALARM(send_test_report, 0, ZB_MILLISECONDS_TO_BEACON_INTERVAL(CONFIG_ZB_SEND_INTERVAL_S * 1000)); /* repeats every CONFIG_ZB_SEND_INTERVAL_S seconds (default 20) */
}

int main(void){
    LOG_INF("Starting Zigbee Light Bulb (Sleepy End Device)");
    // gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE); /* configure led gpio as output*/
    
    ZB_ZCL_REGISTER_DEVICE_CB((zb_callback_t)zcl_device_cb); /* tell zboss which function ahould be called as an event handler callback */
    ZB_AF_REGISTER_DEVICE_CTX(&light_bulb_ctx); /* register device context */
    app_clusters_attr_init(); /* attribute init function */
    
    zb_set_ed_timeout(ED_AGING_TIMEOUT_256MIN); /* set end device waiting timeout - if no reaction for 256 minutes -> end device is dead */
    // keepalive timeout should be ≥ poll interval.
    zb_set_keepalive_timeout(ZB_MILLISECONDS_TO_BEACON_INTERVAL(3600000)); /* 60 min - how often the end device tells its parent "I'm still here" */
    zigbee_configure_sleepy_behavior(true); /* enable sleepy behavoir */

    zb_zdo_pim_set_long_poll_interval(CONFIG_ZB_POLL_INTERVAL_S * 1000); /* how often an end device wakes up to poll a parent about a new message (default 30 s) */
    zigbee_enable(); /* enable zigbee */
    
    ZB_SCHEDULE_APP_ALARM(send_test_report, 0, ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
    k_sleep(K_FOREVER); /* sleep forever*/
    return 0;
}
