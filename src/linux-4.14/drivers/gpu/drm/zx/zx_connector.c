#include "zx_disp.h"
#include "zx_cbios.h"
#include "zx_atomic.h"

enum drm_connector_status
zx_connector_detect_internal(struct drm_connector *connector, bool force, int FullDetect)
{
    struct drm_device *dev = connector->dev;
    zx_card_t *zx_card = dev->dev_private;
    disp_info_t *disp_info = (disp_info_t *)zx_card->disp_info;
    zx_connector_t *zx_connector = to_zx_connector(connector);
    disp_output_type output = zx_connector->output_type;
    int detected_output = 0;
    unsigned char* edid = NULL;
    enum drm_connector_status conn_status;

    detected_output = disp_cbios_detect_connected_output(disp_info, output, FullDetect);

    conn_status = (detected_output & output)? connector_status_connected : connector_status_disconnected;

    zx_connector->edid_changed = 0;

    if(conn_status == connector_status_connected)
    {
        if(connector->status != connector_status_connected)
        {
            edid = disp_cbios_read_edid(disp_info, output);
            zx_connector->edid_changed = 1;
        }
        else if(zx_connector->compare_edid)
        {
            edid = disp_cbios_read_edid(disp_info, output);
            if((!zx_connector->edid_data && edid) ||
                (zx_connector->edid_data && !edid) ||
                (zx_connector->edid_data && edid && zx_memcmp(zx_connector->edid_data, edid, EDID_BUF_SIZE)))
            {
                zx_connector->edid_changed = 1;
            }
        }
        
        if(zx_connector->edid_changed)
        {
            if(zx_connector->edid_data)
            {
                zx_free(zx_connector->edid_data);
            }
            zx_connector->edid_data = edid;
            disp_cbios_get_connector_attrib(disp_info, zx_connector);
        }
        else if(edid)
        {
            zx_free(edid);
            edid = NULL;
        }
    }
    else
    {
        if(zx_connector->edid_data)
        {
            zx_free(zx_connector->edid_data);
            zx_connector->edid_data = NULL;
        }
        zx_connector->monitor_type = UT_OUTPUT_TYPE_NONE;
        zx_connector->support_audio = 0;
    }

    return conn_status;
}

static enum drm_connector_status
zx_connector_detect(struct drm_connector *connector, bool force)
{
    return zx_connector_detect_internal(connector, force, 0);
}

static int zx_connector_get_modes(struct drm_connector *connector)
{
    struct drm_device *dev = connector->dev;
    zx_card_t *zx_card = dev->dev_private;
    disp_info_t *disp_info = (disp_info_t *)zx_card->disp_info;
    zx_connector_t *zx_connector = to_zx_connector(connector);
    int dev_mode_size = 0, adapter_mode_size = 0, dev_real_num = 0, adapter_real_num = 0, real_num = 0, added_num = 0;
    int i = 0, skip_create = 0;
    void* mode_buf = NULL;
    void* adapter_mode_buf = NULL; 
    void* merge_mode_buf = NULL;
    void* cb_mode_list = NULL;
    int  output = zx_connector->output_type;
    struct drm_display_mode *drm_mode = NULL;

    dev_mode_size = disp_cbios_get_modes_size(disp_info, output);

    if(!dev_mode_size)
    {
        goto END;
    }

    mode_buf = zx_calloc(dev_mode_size);

    if(!mode_buf)
    {
        goto END;
    }

#if DRM_VERSION_CODE >= KERNEL_VERSION(4,19,0)
    drm_connector_update_edid_property(connector, (struct edid*)zx_connector->edid_data);
#else
    drm_mode_connector_update_edid_property(connector, (struct edid*)zx_connector->edid_data);
#endif
    
    dev_real_num = disp_cbios_get_modes(disp_info, output, mode_buf, dev_mode_size);

    cb_mode_list = mode_buf;
    real_num = dev_real_num;
    
    if(disp_info->scale_support)
    {
        adapter_mode_size = disp_cbios_get_adapter_modes_size(disp_info);
    
        if(adapter_mode_size != 0)
        {
            adapter_mode_buf = zx_calloc(adapter_mode_size);
        
            if(!adapter_mode_buf)
            {
                goto END;
            }
            adapter_real_num = disp_cbios_get_adapter_modes(disp_info, adapter_mode_buf, adapter_mode_size);
        
            merge_mode_buf = zx_calloc((dev_real_num + adapter_real_num) * sizeof(CBiosModeInfoExt));
    
            if(!merge_mode_buf)
            {
                goto END;
            }
        
            real_num = disp_cbios_merge_modes(merge_mode_buf, adapter_mode_buf, adapter_real_num, mode_buf, dev_real_num);
            cb_mode_list = merge_mode_buf;
        }
    }

    for (i = 0; i < real_num; i++)
    {
        if(!skip_create)
        {
            drm_mode = drm_mode_create(dev);
        }
        if(!drm_mode)
        {
            skip_create = 0;
            break;
        }
        if(S_OK != disp_cbios_cbmode_to_drmmode(disp_info, output, cb_mode_list, i, drm_mode))
        {
            skip_create = 1;
            continue;
        }
        
        skip_create = 0;
        drm_mode_set_name(drm_mode);
        drm_mode_probed_add(connector, drm_mode);
        added_num++;
    }

    zx_free(mode_buf);
    mode_buf = NULL;

    dev_mode_size = disp_cbios_get_3dmode_size(disp_info, output);
    mode_buf = zx_calloc(dev_mode_size);
    if(!mode_buf)
    {
        goto END;
    }

    real_num = disp_cbios_get_3dmodes(disp_info, output, mode_buf, dev_mode_size);

    for(i = 0; i < real_num; i++)
    {
        if(!skip_create)
        {
            drm_mode = drm_mode_create(dev);
        }
        if(!drm_mode)
        {
            skip_create = 0;
            break;
        }
        if(S_OK != disp_cbios_3dmode_to_drmmode(disp_info, output, mode_buf, i, drm_mode))
        {
            skip_create = 1;
            continue;
        }

        skip_create = 0;
        drm_mode_set_name(drm_mode);
        drm_mode_probed_add(connector, drm_mode);
        added_num++;
    }

END:

    if(skip_create && drm_mode)
    {
        drm_mode_destroy(dev, drm_mode);
    }

    if(mode_buf)
    {
        zx_free(mode_buf);
        mode_buf = NULL;
    }

    if(adapter_mode_buf)
    {
        zx_free(adapter_mode_buf);
        adapter_mode_buf = NULL;
    }

    if(merge_mode_buf)
    {
        zx_free(merge_mode_buf);
        merge_mode_buf = NULL;
    }

    return added_num;
}

enum drm_mode_status 
zx_connector_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode)
{
    struct drm_device *dev = connector->dev;
    zx_card_t *zx_card = dev->dev_private;
    disp_info_t *disp_info = (disp_info_t *)zx_card->disp_info;
    adapter_info_t *adp_info = disp_info->adp_info;
    zx_connector_t *zx_connector = to_zx_connector(connector);
    int max_clock;

    if ((adp_info->chip_id == CHIP_CHX001) || (adp_info->chip_id == CHIP_CHX002))
    {
        if (zx_connector->output_type == DISP_OUTPUT_CRT)
        {
            max_clock = 400000;    // 400 MHz
        }
        else
        {
            max_clock = 600000;    // 600 MHz
        }
    }
    else
    {
        // default value
        max_clock = 300000;    // 300 MHz
    }

    if (mode->clock > max_clock)
    {
        return MODE_CLOCK_HIGH;
    }

    return MODE_OK;
}

static void zx_connector_destroy(struct drm_connector *connector)
{
    zx_connector_t *zx_connector = to_zx_connector(connector);

    if(zx_connector->edid_data)
    {
        zx_free(zx_connector->edid_data);
        zx_connector->edid_data = NULL;
    }
    drm_connector_unregister(connector);
    drm_connector_cleanup(connector);
    zx_free(zx_connector);
}

#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
static int zx_connector_dpms(struct drm_connector *connector, int mode)
#else
static void zx_connector_dpms(struct drm_connector *connector, int mode)
#endif
{
    if(connector->encoder)
    {
        if(mode == DRM_MODE_DPMS_ON)
        {
            zx_encoder_enable(connector->encoder);
        }
        else
        {
            zx_encoder_disable(connector->encoder);
        }

        connector->dpms = mode;
    }  

#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
    return 0;
#endif
}

#if DRM_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
//in zx chip, connector is fixed to encoder, so we just pick up the 1st encoder from table
static struct drm_encoder *zx_best_encoder(struct drm_connector *connector)
{
    struct drm_mode_object* obj = NULL;
    int encoder_id = connector->encoder_ids[0];

    obj = drm_mode_object_find(connector->dev, encoder_id, DRM_MODE_OBJECT_ENCODER);
    zx_assert(obj != NULL);

    return  obj_to_encoder(obj);
}
#endif

static const struct drm_connector_funcs zx_connector_funcs = 
{
    .detect = zx_connector_detect,
    .dpms = zx_connector_dpms,
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = zx_connector_destroy,
#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
    .atomic_destroy_state = zx_connector_destroy_state,
    .atomic_duplicate_state = zx_connector_duplicate_state,
#endif
};

static const struct drm_connector_helper_funcs zx_connector_helper_funcs = 
{
    .get_modes = zx_connector_get_modes,
    .mode_valid = zx_connector_mode_valid,
#if DRM_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
    .best_encoder = zx_best_encoder,
#endif
};

struct drm_connector* disp_connector_init(disp_info_t* disp_info, disp_output_type output)
{
    zx_card_t*  zx_card = disp_info->zx_card;
    struct drm_device*  drm = zx_card->drm_dev;
    struct drm_connector*  connector = NULL;
    zx_connector_t* zx_connector = NULL;
    int  conn_type = 0;

    zx_connector = zx_calloc(sizeof(zx_connector_t));
    if (!zx_connector)
    {
        return NULL;
    }

#if  DRM_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
    {
        zx_connector_state_t* zx_conn_state = NULL;
        zx_conn_state = zx_calloc(sizeof(zx_connector_state_t));
        if (!zx_conn_state)
        {
            if (zx_connector)
            {
                zx_free(zx_connector);
            }
            return NULL;
        }

        zx_connector->base_connector.state = &zx_conn_state->base_conn_state;
        zx_conn_state->base_conn_state.connector = &zx_connector->base_connector; 
    }
#endif
    connector = &zx_connector->base_connector;

    if (output == DISP_OUTPUT_CRT)
    {
        conn_type = DRM_MODE_CONNECTOR_VGA;
        connector->stereo_allowed = FALSE;
        connector->interlace_allowed = FALSE;
    }
    else if ((output == DISP_OUTPUT_DP5) | (output == DISP_OUTPUT_DP6))
    {
        conn_type = DRM_MODE_CONNECTOR_DisplayPort;
        connector->stereo_allowed = TRUE;
        connector->interlace_allowed = TRUE;
    }
    else
    {
        conn_type = DRM_MODE_CONNECTOR_Unknown;
    }
    
    drm_connector_init(drm, connector, &zx_connector_funcs, conn_type);
    drm_connector_helper_add(connector, &zx_connector_helper_funcs);
    zx_connector->output_type = output;
    connector->doublescan_allowed = FALSE;

    if(output & disp_info->supp_polling_outputs)
    {
        connector->polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;
    }
    else if(output & disp_info->supp_hpd_outputs)
    {
        connector->polled = DRM_CONNECTOR_POLL_HPD;
        if(output == DISP_OUTPUT_DP5)
        {
            zx_connector->hpd_int_bit = INT_DP_1;
            zx_connector->hda_int_bit = (1 << 25);
        }
        else if(output == DISP_OUTPUT_DP6)
        {
            zx_connector->hpd_int_bit = INT_DP_2;
            zx_connector->hda_int_bit = (1 << 26);
        }
    }

    return connector;
}



