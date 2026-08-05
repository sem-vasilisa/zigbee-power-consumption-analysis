#include <zboss_api.h> // the Zigbee stack itself (ZBOSS) - joining network, sending packets, etc.
#include <zephyr/kernel.h>  // Zephyr RTOS - gives you k_sleep, threads, etc.
#include "zb_light_bulb.h"
#include <zboss_api_addons.h> // extra helper macros/functions on top of zboss_api.h
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zigbee/zigbee_app_utils.h> // Nordic's helper functions (zigbee_enable, sleepy behavior, etc.)
#include <zigbee/zigbee_error_handler.h> // ZB_ERROR_CHECK macro

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF); // create a log channel called app 

#define LIGHT_BULB_ENDPOINT 20
#define LED_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* container to hold data for each cluster */
struct zb_device_ctx {
    zb_zcl_basic_attrs_ext_t    basic_attr;
    zb_zcl_identify_attrs_t     identify_attr;
    zb_zcl_on_off_attrs_t       on_off_attr;
};

static struct zb_device_ctx dev_ctx; /* our router */

/* set the starting values for the routers attributes */
static void app_clusters_attr_init(void)
{ 
    dev_ctx.basic_attr.zcl_version  = ZB_ZCL_VERSION;
    // dev_ctx.basic_attr.power_source = ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    dev_ctx.identify_attr.identify_time = ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;
    dev_ctx.on_off_attr.on_off = ZB_FALSE;
    dev_ctx.basic_attr.power_source = ZB_ZCL_BASIC_POWER_SOURCE_BATTERY; /* set the power source to battery */
}

/* basic_attr_ list is the name of the array variable holding the listg of attributes for the basic cluster*/
ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST(basic_attr_list, &dev_ctx.basic_attr.zcl_version, &dev_ctx.basic_attr.power_source);
ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(identify_attr_list, &dev_ctx.identify_attr.identify_time);
ZB_ZCL_DECLARE_ON_OFF_ATTRIB_LIST(on_off_attr_list, &dev_ctx.on_off_attr.on_off);            /* ← atrybut OnOff (stan żarówki) */

ZB_DECLARE_LIGHT_BULB_CLUSTER_LIST(light_bulb_clusters, basic_attr_list, identify_attr_list, on_off_attr_list);
ZB_DECLARE_LIGHT_BULB_EP(light_bulb_ep, LIGHT_BULB_ENDPOINT, light_bulb_clusters); /* endpoint - object light_bulb_ep is a one single "endpoint descriptor" */
ZBOSS_DECLARE_DEVICE_CTX_1_EP(light_bulb_ctx, light_bulb_ep); /* the whole device - how many endpoints the device has and a list of pointers to their descriptors */

void on_off_set_value(zb_bool_t value)
{
    dev_ctx.on_off_attr.on_off = value;
    if(value){
        gpio_pin_set_dt(&led, 1);
        LOG_INF("Setting On/Off value: %s", value ? "ON" : "OFF");
    }
    else{
        gpio_pin_set_dt(&led, 0);
        LOG_INF("Setting On/Off value: %s", value ? "ON" : "OFF");
    }
}

/* the coordinator callback function, called automatically whenever any attribute on our device changes due to an incoming Zigbee command */
static zb_uint8_t zcl_device_cb(zb_bufid_t bufid){ /* is a buffer where the callback data is stored */
    zb_zcl_device_callback_param_t *p = ZB_BUF_GET_PARAM(bufid, zb_zcl_device_callback_param_t); /* p is a pointer to the event details from the buffer */
    p->status = RET_OK; /* the buffer was processed successfully */

    /* is this callback happening because an attribute value was changed? */
    if(p->device_cb_id != ZB_ZCL_SET_ATTR_VALUE_CB_ID){
        return ZB_FALSE;
    }

    /* extract from the buffer which cluster and aatribute have been set */
    zb_uint16_t cluster_id = p->cb_param.set_attr_value_param.cluster_id;
    zb_uint16_t attr_id = p->cb_param.set_attr_value_param.attr_id;
    
    /* if it's on/off cluster and on/off attribute */
    if(cluster_id == ZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID){
        zb_uint8_t value = p->cb_param.set_attr_value_param.values.data8; /* the new value that was set */
        on_off_set_value(value ? ZB_TRUE :ZB_FALSE);
        LOG_INF("On/Off attribute changed to: %d", value);
    }
    return ZB_FALSE;
}

void zboss_signal_handler(zb_bufid_t bufid){ /* read info from the buffer */
    // get details about the event 
    zb_zdo_app_signal_hdr_t  *sg_p = NULL; 
    zb_zdo_app_signal_type_t  sig  = zb_get_app_signal(bufid, &sg_p); // reads from the buffer which event has occurred
    zb_ret_t                  status = ZB_GET_APP_SIGNAL_STATUS(bufid); // if that event succeeded or failed

    switch (sig){
        /* first boot ever (fresh device, empty NVRAM/flash), never joined a network before */
        case ZB_BDB_SIGNAL_DEVICE_FIRST_START:   
            LOG_INF("Joining network for the first time...");
            bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            break;

        /* reboot, but already knows a network */
        case ZB_BDB_SIGNAL_DEVICE_REBOOT:        
            bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            break;

        /* network steering completed */
        case ZB_BDB_SIGNAL_STEERING:
            if (status == RET_OK) {
                LOG_INF("Joined network OK");
                LOG_INF("PAN ID: 0x%04x, channel: %d", zb_get_pan_id(), zb_get_current_channel());
                LOG_INF("Our short addr: 0x%04x", zb_get_short_address());
            } 
            /* if failed try again in one second */
            else 
            {  
                ZB_SCHEDULE_APP_ALARM((zb_callback_t)bdb_start_top_level_commissioning, ZB_BDB_NETWORK_STEERING, ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
            }
            break;
        
        default:
            ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid)); /* let the stack handle it the normal way*/
            break;
    }
    if (bufid) zb_buf_free(bufid); // if buffer not empty - clean
}


int main(void)
{
    LOG_INF("Starting Zigbee Light Bulb (Router)");
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    ZB_ZCL_REGISTER_DEVICE_CB(zcl_device_cb);    /* callback ZCL - call this function when the attribute changes */
    ZB_AF_REGISTER_DEVICE_CTX(&light_bulb_ctx);  /* gives zigbee stack a context for the device */
    app_clusters_attr_init();                    /* sets starting attribute values */
    
    zb_set_ed_timeout(ED_AGING_TIMEOUT_64MIN); /* if you don't hear from me for 64 minutes - i'm gone */
    zb_set_keepalive_timeout(ZB_MILLISECONDS_TO_BEACON_INTERVAL(30000)); /* how often an end device will send messages that it's alive */
    zigbee_configure_sleepy_behavior(true);        /* go to sleep and turn off radio */

    // if (IS_ENABLED(CONFIG_RAM_POWER_DOWN_LIBRARY)) {
    //     power_down_unused_ram(); /* turns off parts of the memory that are not being used */
    // }

    zb_zdo_pim_set_long_poll_interval(3000); /* how often device wakes up and asks a parent about a new message */
    
    zigbee_enable();                             /* start zigbee stack ZBOSS running */
    k_sleep(K_FOREVER); /* sleeps forever and zboss waits for events */
    return 0;
}