#define ZB_LIGHT_BULB_DEVICE_ID 0x0100    /* hue app: it's an On/Off Light */

#define ZB_DEVICE_VER_LIGHT_BULB 0

#define ZB_LIGHT_BULB_IN_CLUSTER_NUM 3    /* server: Basic + Identify + On/Off clusters */
#define ZB_LIGHT_BULB_OUT_CLUSTER_NUM 0   /* no out clusters - cluster where you're the client */

/* macro that builds the cluster list */
#define ZB_DECLARE_LIGHT_BULB_CLUSTER_LIST(                      \
    cluster_list_name,                                           \
    basic_attr_list,                                             \
    identify_attr_list,                                          \
    on_off_attr_list                                             \
)zb_zcl_cluster_desc_t cluster_list_name[] =                     \
{                                                                \
    ZB_ZCL_CLUSTER_DESC(                                         \
        ZB_ZCL_CLUSTER_ID_IDENTIFY,                              \
        ZB_ZCL_ARRAY_SIZE(identify_attr_list, zb_zcl_attr_t),    \
        (identify_attr_list),                                    \
        ZB_ZCL_CLUSTER_SERVER_ROLE,                              \
        ZB_ZCL_MANUF_CODE_INVALID                                \
    ),                                                           \
    ZB_ZCL_CLUSTER_DESC(                                         \
        ZB_ZCL_CLUSTER_ID_BASIC,                                 \
        ZB_ZCL_ARRAY_SIZE(basic_attr_list, zb_zcl_attr_t),       \
        (basic_attr_list),                                       \
        ZB_ZCL_CLUSTER_SERVER_ROLE,                              \
        ZB_ZCL_MANUF_CODE_INVALID                                \
    ),                                                           \
    ZB_ZCL_CLUSTER_DESC(                                         \
        ZB_ZCL_CLUSTER_ID_ON_OFF,                                \
        ZB_ZCL_ARRAY_SIZE(on_off_attr_list, zb_zcl_attr_t),      \
        (on_off_attr_list),                                      \
        ZB_ZCL_CLUSTER_SERVER_ROLE,                              \
        ZB_ZCL_MANUF_CODE_INVALID                                \
    )                                                            \
}
/* the result of calling ZB_DECLARE_LIGHT_BULB_CLUSTER_LIST(name, cl1, cl2, cl3) is an array zb_zcl_cluster_desc_t name = {cl1, cl2, cl3} */

ZB_DECLARE_SIMPLE_DESC(3, 0); /* 3 cluster description in one array (basic, identify, on/off)*/

/* ## is a token, macro that generates a type, ## glues two pieces of text together into one identifier */
/* creates a variable to preserve endpoint number, device type, list of clusters */
#define ZB_ZCL_DECLARE_LIGHT_BULB_SIMPLE_DESC(ep_name, ep_id, in_clust_num, out_clust_num) \
					       \
	ZB_AF_SIMPLE_DESC_TYPE(in_clust_num, out_clust_num) simple_desc_##ep_name =	       \
	{										       \
		ep_id,									       \
		ZB_AF_HA_PROFILE_ID,							       \
		ZB_LIGHT_BULB_DEVICE_ID,						       \
		ZB_DEVICE_VER_LIGHT_BULB,						       \
		0,									       \
		in_clust_num,								       \
		out_clust_num,								       \
		         {                                               \
            ZB_ZCL_CLUSTER_ID_BASIC,                    \
            ZB_ZCL_CLUSTER_ID_IDENTIFY,                 \
            ZB_ZCL_CLUSTER_ID_ON_OFF                    \
        }                                               \
    }

/* where ZB_ZCL_DECLARE_LIGHT_BULB_SIMPLE_DESC creates a varibale that has (endpoint number, device type, cluster ID list) */
/* ZB_AF_DECLARE_ENDPOINT_DESC(...) creates a big description of the endpoint */
#define ZB_DECLARE_LIGHT_BULB_EP(ep_name, ep_id, cluster_list)		      \
	ZB_ZCL_DECLARE_LIGHT_BULB_SIMPLE_DESC(ep_name, ep_id,		      \
		ZB_LIGHT_BULB_IN_CLUSTER_NUM, ZB_LIGHT_BULB_OUT_CLUSTER_NUM); \
		ZB_AF_DECLARE_ENDPOINT_DESC(ep_name, ep_id, ZB_AF_HA_PROFILE_ID, 0, NULL,     \
		ZB_ZCL_ARRAY_SIZE(cluster_list, zb_zcl_cluster_desc_t), cluster_list, \
			(ZB_AF_SIMPLE_DESC_TYPE(ZB_LIGHT_BULB_IN_CLUSTER_NUM, ZB_LIGHT_BULB_OUT_CLUSTER_NUM) *)&simple_desc_##ep_name, \
0, NULL, /* No reporting ctx */ \
0, NULL) 
