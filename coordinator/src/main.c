#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zboss_api.h>
#include <zb_mem_config_max.h>          /* konfiguracja pamięci ZBOSS — WYMAGANE */
#include <zigbee/zigbee_error_handler.h>
#include <zigbee/zigbee_app_utils.h>    /* zigbee_enable(), default handler */
#include <zb_nrf_platform.h>
#include "zb_range_extender.h"
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h> /* aktywujemy shell */

// LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);   /* obok pozostałych #include */

LOG_MODULE_REGISTER(zigbee_coordinator, LOG_LEVEL_INF);

#define LED_NODE DT_ALIAS(led0)
#define COORD_EP  10   /* endpoint On/Off clienta, taki sam jak ustawiliśmy w pierwszym kroku  */
#define SW_NODE DT_ALIAS(sw0)             /* SW1 na donglu */
#define MAX_NAME_LEN 32

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/*  here we will store end-device information  */
static struct {
    bool           used, bound; /* used = have we seen a device yet, bound = did binding succeed */
    zb_ieee_addr_t ieee;
    zb_uint16_t    short_addr;
    zb_uint8_t     remote_ep;   /* endpoint number on the remote device */
    char name[MAX_NAME_LEN];
} dev;

/* --- naming devices --- */
static void name_wait_work_handler(struct k_work *work);
static K_WORK_DEFINE(name_wait_work, name_wait_work_handler);

/* -------- coordinator's device profile -------- */

/*  clusters declaration  */
struct zb_device_ctx {
    zb_zcl_basic_attrs_t    basic_attr;
    zb_zcl_identify_attrs_t identify_attr;
};
static struct zb_device_ctx dev_ctx;

/* which variables belong to which cluster */
ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(identify_attr_list, &dev_ctx.identify_attr.identify_time);
ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST(basic_attr_list, &dev_ctx.basic_attr.zcl_version, &dev_ctx.basic_attr.power_source);

/* the endpoint that connects to those clusters*/
ZB_DECLARE_SIMPLE_DESC(2, 1);  /* 2 input clusters, 1 output cluster */
ZB_DECLARE_RANGE_EXTENDER_CLUSTER_LIST(coord_ep1_clusters, basic_attr_list, identify_attr_list); /* put 3 clusters in one cluster list */
ZB_DECLARE_RANGE_EXTENDER_EP(coord_ep1, 10, coord_ep1_clusters); /* attach cluster list to endpoint 10 */


ZBOSS_DECLARE_DEVICE_CTX_1_EP(coordinator_ctx, coord_ep1); /* put everything in one device context */

static void app_clusters_attr_init(void)
{
    dev_ctx.basic_attr.zcl_version  = ZB_ZCL_VERSION;
    dev_ctx.basic_attr.power_source = ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    dev_ctx.identify_attr.identify_time = ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;
}

/* --- create a network sygnalized by led --- */
static volatile bool network_up = false; // is network ready

/* -------- controlling the bulb with a button -------- */

/*  list of endpoints of the discovered device  */
static struct { zb_uint8_t eps[16], count, idx; } disc; /* eps is a list of enpoint numbers the bulb reported, count how many were returned and idx which one we're currently checking*/

static void send_simple_desc_req(zb_bufid_t bufid); /* sends a ZDO request about endpoints */

/* if binding succeeds, handle the response */
static void bind_cb(zb_bufid_t bufid)
{
    zb_zdo_bind_resp_t *r = (zb_zdo_bind_resp_t *)zb_buf_begin(bufid);
    if (r->status == ZB_ZDP_STATUS_SUCCESS) {
        dev.bound = true;
        dev.remote_ep = disc.eps[disc.idx - 1];
        LOG_INF("Bind OK: On/Off → EP %d", dev.remote_ep);
    } else {
        LOG_ERR("Bind failed: %d", r->status);
    }
    zb_buf_free(bufid);
}
/* binding - connect ep with cluster*/
static void do_bind(zb_bufid_t bufid)
{
    zb_zdo_bind_req_param_t *req = ZB_BUF_GET_PARAM(bufid, zb_zdo_bind_req_param_t); /* get a pointer to the bind request parameters */
    zb_ieee_addr_t my_ieee;
    zb_get_long_address(my_ieee);
    ZB_MEMCPY(req->src_address, my_ieee, sizeof(zb_ieee_addr_t));
    req->src_endp      = COORD_EP; /* source endpoint */
    req->cluster_id    = ZB_ZCL_CLUSTER_ID_ON_OFF; /* which cluster this binding is for*/
    req->dst_addr_mode = ZB_BIND_DST_ADDR_MODE_64_BIT_EXTENDED; /* destination address mode */
    ZB_MEMCPY(&req->dst_address.addr_long, dev.ieee, sizeof(zb_ieee_addr_t));
    req->dst_endp     = disc.eps[disc.idx - 1];
    req->req_dst_addr = zb_get_short_address();   /* where the binding request is being sent */
    zb_zdo_bind_req(bufid, bind_cb);
}

/* response to the cluster request*/
static void simple_desc_cb(zb_bufid_t bufid)
{
    zb_zdo_simple_desc_resp_t *r = (zb_zdo_simple_desc_resp_t *)zb_buf_begin(bufid);
    bool found = false;
    if (r->hdr.status == ZB_ZDP_STATUS_SUCCESS) {
        /* loop over the list of clusters looking for a on-off cluster */
        for (zb_uint8_t i = 0; i < r->simple_desc.app_input_cluster_count; i++)
            if (r->simple_desc.app_cluster_list[i] == ZB_ZCL_CLUSTER_ID_ON_OFF)
                found = true;
    }
    zb_buf_free(bufid); /* free the buffer */

    if (found)                       zb_buf_get_out_delayed(do_bind); /* create the binding*/
    else if (disc.idx < disc.count)  zb_buf_get_out_delayed(send_simple_desc_req); /* continue to the next endpoint */
    else                             LOG_WRN("No On/Off server found");
}
/* sends a ZDO request asking about endpoints clusters */
static void send_simple_desc_req(zb_bufid_t bufid)
{
    zb_zdo_simple_desc_req_t *req = (zb_zdo_simple_desc_req_t *)zb_buf_initial_alloc(bufid, sizeof(zb_zdo_simple_desc_req_t)); /* reseres place in the buffer for the request */
    req->nwk_addr = dev.short_addr; /* fill in which device are we asking, by short address */
    req->endpoint = disc.eps[disc.idx++]; /* fill in the endpoint we are asking about */
    zb_zdo_simple_desc_req(bufid, simple_desc_cb); /* send the request, when the response comes back, call simple_desc_cb */
}

/* gets the list of endpoints from end device and saves it */
static void active_ep_cb(zb_bufid_t bufid)
{
    zb_zdo_ep_resp_t *r = (zb_zdo_ep_resp_t *)zb_buf_begin(bufid); // gets from the buffer reads header
    zb_uint8_t *list = (zb_uint8_t *)(r + 1); // after the header, gets the list of endpoints that were returned
    disc.count = MIN(r->ep_count, ARRAY_SIZE(disc.eps)); // how many endpoints were returned, but not more than the size of our array
    disc.idx = 0;
    for (zb_uint8_t i = 0; i < disc.count; i++) disc.eps[i] = list[i]; // copy each enpoint to our disc.eps array, to our memory
    zb_buf_free(bufid); // return buffer memory to the zboss
    if (disc.count) zb_buf_get_out_delayed(send_simple_desc_req); // if we have an endpoint, request a new buffer and call simple description to find a cluster
}

/* what endpoints do we have */
static void send_active_ep_req(zb_bufid_t bufid)
{
    zb_zdo_active_ep_req_t *req = (zb_zdo_active_ep_req_t *)zb_buf_initial_alloc(bufid, sizeof(zb_zdo_active_ep_req_t));
    req->nwk_addr = dev.short_addr;
    zb_zdo_active_ep_req(bufid, active_ep_cb);
}

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW_NODE, gpios); /* gpio pin info */
static struct gpio_callback button_cb; /* structure to register interrupt handler for that pin */

/* runs on a zboss thread */
static void send_toggle_cmd(zb_bufid_t bufid)   /* już w wątku ZBOSS */
{
    if (!dev.bound) { zb_buf_free(bufid); return; } /* if device is not bound, free the buffer and return */
    LOG_INF("Sending Toggle to 0x%04x EP %d", dev.short_addr, dev.remote_ep);
    ZB_ZCL_ON_OFF_SEND_TOGGLE_REQ(bufid, dev.short_addr,
        ZB_APS_ADDR_MODE_16_ENDP_PRESENT,   /* adresowanie jawne: short + EP */
        dev.remote_ep, COORD_EP,
        ZB_AF_HA_PROFILE_ID, ZB_ZCL_DISABLE_DEFAULT_RESPONSE, NULL); /* sends a ZCL toggle command */
}

/* ISR for button press */
static void button_pressed(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    zb_buf_get_out_delayed(send_toggle_cmd); /* grabs a buffer and schedules the toggle command */
}

/* -------- looking for an end device --------*/
/*gets called from the ZBOSS thread when a new device joins the network, records the new device and starts discovery */
static void handle_device_joined(zb_uint16_t short_addr, const zb_ieee_addr_t ieee)
{
    // LOG_INF("=====================================================");
    // LOG_INF("New device joined: 0x%04x", short_addr);
    // LOG_INF("  IEEE: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
    //     ieee[7], ieee[6], ieee[5], ieee[4],
    //     ieee[3], ieee[2], ieee[1], ieee[0]);
    // LOG_INF("=====================================================");

    if (dev.used) return;                 /* handels one device at a time */
    dev.used = true; /* occupied */
    dev.bound = false; /* reset bounding */
    dev.short_addr = short_addr;
    ZB_MEMCPY(dev.ieee, ieee, sizeof(zb_ieee_addr_t)); /* copy device IEEE address to our memory */
    // LOG_INF("New device 0x%04x — starting discovery", short_addr);
    // zb_buf_get_out_delayed(send_active_ep_req); /* request the buffer and schedule the first step of discovery */
    k_work_submit(&name_wait_work);          /* nie blokuj wątku ZBOSS! */
}

/* handles events from the ZBOSS thread */
void zboss_signal_handler(zb_bufid_t bufid)
{
    // get details about the event 
    zb_zdo_app_signal_hdr_t  *sg_p = NULL;
    zb_zdo_app_signal_type_t  sig  = zb_get_app_signal(bufid, &sg_p); // reads from the buffer which event has occurred
    zb_ret_t                  status = ZB_GET_APP_SIGNAL_STATUS(bufid); // did it succeed or fail
    switch (sig) {
        case ZB_BDB_SIGNAL_DEVICE_FIRST_START:      /* świeży NVRAM → utwórz sieć */
            if (status == RET_OK){
                LOG_INF("First start — forming new network");
                bdb_start_top_level_commissioning(ZB_BDB_NETWORK_FORMATION);
            }
            break;

        case ZB_BDB_SIGNAL_FORMATION:               /* sieć utworzona → otwórz na join */
            if (status == RET_OK)
            {
                LOG_INF("Network formed — starting steering");
                bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            }
            break;

        case ZB_BDB_SIGNAL_DEVICE_REBOOT:           /* sieć odtworzona z NVRAM */
            if (status == RET_OK){
                LOG_INF("Device reboot — starting steering");
                bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            }
            break;

        case ZB_BDB_SIGNAL_STEERING:                /* permit join aktywny (180 s) */
            if (status == RET_OK){
                network_up = true;

                zb_bdb_set_legacy_device_support(1);   /* dopuść starsze urządzenia */

                LOG_INF("Network steering started");
                LOG_INF("PAN ID: 0x%04x, channel: %d",
                zb_get_pan_id(), zb_get_current_channel());
            }                  /* ← dioda to pokaże */
            break;

        case ZB_ZDO_SIGNAL_DEVICE_ANNCE: {        /* end device get's an id */
            zb_zdo_signal_device_annce_params_t *a =
                ZB_ZDO_SIGNAL_GET_PARAMS(sg_p, zb_zdo_signal_device_annce_params_t); // treat raw data sg_p as a deice annce
            handle_device_joined(a->device_short_addr, a->ieee_addr);// gets the short and long address and calls the function to handle it
        } break;

        case ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {    /* device authorized and now is a prt of network */
            zb_zdo_signal_device_authorized_params_t *auth =
                ZB_ZDO_SIGNAL_GET_PARAMS(sg_p, zb_zdo_signal_device_authorized_params_t); // extracts the authorization parameters from the signal
            // normal login from new zigbee success || older device login success, puts it into a normal state and calls the function to handle it
            if (auth->authorization_status == ZB_ZDO_TCLK_AUTHORIZATION_SUCCESS ||
                auth->authorization_status == ZB_ZDO_LEGACY_DEVICE_AUTHORIZATION_SUCCESS) {
                handle_device_joined(auth->short_addr, auth->long_addr);
            }
        } break;

        /* for unimplemented signals we add cases in event handler so they no longer fall to through to the default case */
        /* network open/closed for joining changed; permit_duration = seconds left open, 0 = closed */
        /* unimplemented signal 53 */
        case ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
            zb_nlme_permit_joining_signal_info_t *p =
                ZB_ZDO_SIGNAL_GET_PARAMS(sg_p, zb_nlme_permit_joining_signal_info_t);
            LOG_INF("Permit join status: %s", p->permit_duration ? "OPEN" : "CLOSED");
        } break;

        /* unimplemented signal 59 */
        case ZB_TCSWAP_DB_BACKUP_REQUIRED_SIGNAL:
            LOG_INF("Trust Center DB backup required");
            break;
        
        /* unimplemented signal 52 */
        case ZB_NLME_STATUS_INDICATION:
            LOG_INF("NLME status indication received");
            break;

        default:
            ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid)); /* let the stack handle it the normal way*/
            break;
    }

    if (bufid) zb_buf_free(bufid); // if buffer not empty - clean
}

/* -------- shell -------- */

/* this method will be called when the toggle command is received */
static int cmd_toggle(const struct shell *sh, size_t argc, char **argv) /* let the function print message to the shell, number of words and words */
{
    // if (!dev.bound) {
    //     shell_error(sh, "Brak zbindowanego urządzenia");
    //     return -EAGAIN; /* stop the function and return an error code */
    // }
    // zb_buf_get_out_delayed(send_toggle_cmd);   /* asks zigbee for a buffer to call send toggle and actually send toggle command */
    // shell_print(sh, "Toggle wysłany");
    // return 0;

    if (argc != 2) { shell_error(sh, "Użycie: toggle <nazwa>"); return -EINVAL; }
    if (!dev.used || strcmp(dev.name, argv[1]) != 0) {
        shell_error(sh, "Nie znam '%s'", argv[1]); return -ENOENT;
    }
    if (!dev.bound) { shell_error(sh, "Jeszcze nie zbindowane"); return -EAGAIN; }
    zb_buf_get_out_delayed(send_toggle_cmd);
    shell_print(sh, "Toggle → %s", dev.name);
    return 0;
}

/* opens the Zigbee network so new devices can join */
static void do_open_network(zb_uint8_t param)
{
    ARG_UNUSED(param);
    zb_bdb_set_legacy_device_support(1); /* support for older devices */
    bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING); /* start network steering */
}

/* runs when we print open*/
static int cmd_open(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    ZB_SCHEDULE_APP_CALLBACK(do_open_network, 0); /* zboss thread will run it when have time */
    shell_print(sh, "Sieć otwarta na dołączanie (180 s)");
    return 0;
}

/* tells the shell which commands exist and what function to run when someone types them */
SHELL_CMD_REGISTER(toggle, NULL, "Wyślij Toggle do urządzenia", cmd_toggle);
SHELL_CMD_REGISTER(open,   NULL, "Otwórz sieć na dołączanie (180 s)", cmd_open);

/* -------- device naming -------- */
static K_SEM_DEFINE(name_sem, 0, 1);
static char pending_name[MAX_NAME_LEN];

static void name_wait_work_handler(struct k_work *work)
{
    LOG_INF("New device 0x%04x — wpisz: name <nazwa>", dev.short_addr);

    k_sem_take(&name_sem, K_FOREVER);        /* czekaj na shell (osobny wątek) */

    strncpy(dev.name, pending_name, MAX_NAME_LEN - 1);
    dev.name[MAX_NAME_LEN - 1] = '\0';
    LOG_INF("Nazwa: %s → start discovery", dev.name);

    zb_buf_get_out_delayed(send_active_ep_req);   /* discovery jak w Etapie 5 */
}

static int cmd_name(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) { shell_error(sh, "Użycie: name <nazwa>"); return -EINVAL; }
    strncpy(pending_name, argv[1], MAX_NAME_LEN - 1);
    pending_name[MAX_NAME_LEN - 1] = '\0';
    k_sem_give(&name_sem);                    /* odblokuj worker */
    shell_print(sh, "Nazwa '%s' przypisana", pending_name);
    return 0;
}

SHELL_CMD_REGISTER(name, NULL, "Nadaj nazwę dołączonemu urządzeniu", cmd_name);

static int cmd_devices(const struct shell *sh, size_t argc, char **argv)
{
    if (!dev.used) {
        shell_print(sh, "No device joined");
        return 0;
    }

    shell_print(sh, "Short: 0x%04x, Name: %s, Bound: %s, Remote EP: %d",
        dev.short_addr,
        dev.name,
        dev.bound ? "yes" : "no",
        dev.remote_ep);

    return 0;
}

SHELL_CMD_REGISTER(devices, NULL, "List joined device(s)", cmd_devices);

static zb_uint8_t coord_ep_handler(zb_bufid_t bufid)
{
    zb_zcl_parsed_hdr_t *zcl_hdr = ZB_BUF_GET_PARAM(bufid, zb_zcl_parsed_hdr_t); /* read frame header from the buffer */

    if (zcl_hdr->cluster_id == ZB_ZCL_CLUSTER_ID_ON_OFF && zcl_hdr->cmd_id == ZB_ZCL_CMD_REPORT_ATTRIB) {

        zb_zcl_report_attr_req_t *rep;
        ZB_ZCL_GENERAL_GET_NEXT_REPORT_ATTR_REQ(bufid, rep); /* get data out of the frame, where rep will have info about which attribute is this, what type and the raw bytes */

        if (rep != NULL && rep->attr_id == ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            zb_uint8_t value = *(zb_uint8_t *)rep->attr_value; /* read actual data send by the end device */
            LOG_INF("Test value received: %d", value);
        } else {
            LOG_WRN("Report attribute parse failed or unexpected attr_id");
        }

        zb_buf_free(bufid);
        return ZB_TRUE;
    }

    return ZB_FALSE;
}

int main(void)
{
    LOG_INF("Starting Zigbee Coordinator");
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    /* konfiguracja przycisku*/
    gpio_pin_configure_dt(&button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb);

    ZB_AF_REGISTER_DEVICE_CTX(&coordinator_ctx);
    ZB_AF_SET_ENDPOINT_HANDLER(COORD_EP, coord_ep_handler);
    app_clusters_attr_init(); // starting values from the device context
    zigbee_enable();

    while (1) {
        if (network_up) {
            gpio_pin_set_dt(&led, 1);           /* sieć gotowa → światło ciągłe */
            k_sleep(K_MSEC(1000));
        } else {
            gpio_pin_toggle_dt(&led);           /* czekamy → szybkie miganie */
            k_sleep(K_MSEC(150));
        }
    }
    return 0;
}