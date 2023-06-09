#include "zx_atomic.h"

#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)

struct drm_atomic_state* zx_atomic_state_alloc(struct drm_device *dev)
{
	zx_ato_state_t *state = zx_calloc(sizeof(zx_ato_state_t));

	if (!state || drm_atomic_state_init(dev, &state->base_ato_state) < 0) 
        {
		zx_free(state);
		return NULL;
	}

	return &state->base_ato_state;
}

void zx_atomic_state_clear(struct drm_atomic_state *s)
{
	zx_ato_state_t *state = to_zx_atomic_state(s);
	drm_atomic_state_default_clear(s);
}

void zx_atomic_state_free(struct drm_atomic_state *s)
{
    	zx_ato_state_t *state = to_zx_atomic_state(s);
	drm_atomic_state_default_release(s);
        zx_free(state);
}

struct drm_crtc_state* zx_crtc_duplicate_state(struct drm_crtc *crtc)
{
    zx_crtc_state_t* crtc_state;

    crtc_state = zx_calloc(sizeof(zx_crtc_state_t));
    if (!crtc_state)
    {
        return NULL;
    }

    __drm_atomic_helper_crtc_duplicate_state(crtc, &crtc_state->base_cstate);


    if(crtc->state)
    {
        crtc_state->timing_strategy = to_zx_crtc_state(crtc->state)->timing_strategy;
    }

    return &crtc_state->base_cstate;
   
}

void zx_crtc_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *s)
{
    zx_crtc_state_t*  state = to_zx_crtc_state(s);
    __drm_atomic_helper_crtc_destroy_state(s);
    zx_free(state);
}


int zx_crtc_atomic_get_property(struct drm_crtc *crtc,
               const struct drm_crtc_state *state,
               struct drm_property *property,
               uint64_t *val)
{
    int ret = -EINVAL;
    zx_crtc_t* zx_crtc = to_zx_crtc(crtc);
    zx_crtc_state_t* zx_crtc_state = to_zx_crtc_state(state);
    

    if(property == zx_crtc->timing_strategy)
    {
        *val = zx_crtc_state->timing_strategy;
        ret = 0;
    }

    if(ret)
    {
#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
        DRM_WARN("Invalid driver-private property '%s'\n", property->name);
#endif
    }
    return ret;
}

int  zx_crtc_atomic_set_property(struct drm_crtc *crtc,
               struct drm_crtc_state *state,
               struct drm_property *property,
               uint64_t val)  
{
    int ret = -EINVAL;
    zx_crtc_t* zx_crtc = to_zx_crtc(crtc);
    zx_crtc_state_t* zx_crtc_state = to_zx_crtc_state(state);


    if(property == zx_crtc->timing_strategy)
    {
        zx_crtc_state->timing_strategy = val;
        ret = 0;
    }

    if(ret)
    {
#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
        DRM_WARN("Invalid driver-private property '%s'\n", property->name);
#endif
    }
    return ret;
}



int zx_crtc_helper_check(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
    return 0;
}

struct drm_connector_state* zx_connector_duplicate_state(struct drm_connector *connector)
{
    zx_connector_state_t* zx_conn_state;

    zx_conn_state = zx_calloc(sizeof(zx_connector_state_t));
    if (!zx_conn_state)
    {
        return NULL;
    }

    __drm_atomic_helper_connector_duplicate_state(connector, &zx_conn_state->base_conn_state);

    return &zx_conn_state->base_conn_state;
}

void zx_connector_destroy_state(struct drm_connector *connector, struct drm_connector_state *state)
{
    zx_connector_state_t *zx_conn_state = to_zx_conn_state(state);
    __drm_atomic_helper_connector_destroy_state(state);
    zx_free(zx_conn_state);
}


#endif

