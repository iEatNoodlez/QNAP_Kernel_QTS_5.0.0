#include "zx_disp.h"
#include "zx_cbios.h"
#include "zx_fbdev.h"
#include "zx_gem.h"
#include "zx_debugfs.h"
#include <drm/drm_fb_helper.h>
#if DRM_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
#include <linux/async.h>
#endif

static int zxfb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
    struct drm_fb_helper *helper = info->par;
    struct drm_device *dev = helper->dev;
    struct zx_fbdev *fbdev = to_zx_fbdev(helper);
    struct drm_zx_framebuffer *fb = fbdev->fb;
    struct drm_zx_gem_object *obj = fb->obj;
    int ret = 0;

    if(obj->core_handle)
    {
        ret = drm_gem_mmap_obj(&obj->base, obj->base.size, vma);
        if(ret)
            return ret;
    }

    obj->delay_map = 1;

    vma->vm_flags &= ~(VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
    vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

    zx_drm_gem_object_vm_prepare(obj);

    return zx_drm_gem_object_mmap(dev->dev_private, obj, vma);
}

static int zx_drm_fb_set_par(struct fb_info *info)
{
    struct drm_fb_helper *helper = info->par;
    struct drm_device *dev = helper->dev;
    struct zx_fbdev *fbdev = to_zx_fbdev(helper);
    zx_card_t* zx = (zx_card_t*)dev->dev_private;
    
    zx_info("To set par on drm fb.\n");
    
    return drm_fb_helper_set_par(info);
}

static struct fb_ops zxfb_ops = {
    .owner = THIS_MODULE,
#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
#if DRM_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
    DRM_FB_HELPER_DEFAULT_OPS,
#endif

    .fb_set_par = zx_drm_fb_set_par,
    .fb_fillrect = drm_fb_helper_cfb_fillrect,
    .fb_copyarea = drm_fb_helper_cfb_copyarea,
    .fb_imageblit = drm_fb_helper_cfb_imageblit,
#else
    .fb_check_var = drm_fb_helper_check_var,
    .fb_set_par = drm_fb_helper_set_par,
    .fb_pan_display = drm_fb_helper_pan_display,
    .fb_blank = drm_fb_helper_blank,
    .fb_debug_enter = drm_fb_helper_debug_enter,
    .fb_debug_leave = drm_fb_helper_debug_leave,
    .fb_setcmap = drm_fb_helper_setcmap,

    .fb_fillrect = cfb_fillrect,
    .fb_copyarea = cfb_copyarea,
    .fb_imageblit = cfb_imageblit,
#endif
    .fb_mmap    = zxfb_mmap,
};

#if DRM_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
static bool zx_fb_initial_config(struct drm_fb_helper *fb_helper,
        struct drm_fb_helper_crtc **crtcs,
        struct drm_display_mode **modes,
        struct drm_fb_offset *offsets,
        bool *enabled, int width, int height)
{
    struct drm_device *dev = fb_helper->dev;
    zx_card_t *zx = dev->dev_private;


    return false;
}
#endif

#if DRM_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
static void zx_crtc_fb_gamma_set(struct drm_crtc *crtc, u16 red, u16 green, u16 blue, int reqno)
{
    DRM_DEBUG_KMS("crtc=%d,red=%d,green=%d,blue=%d\n", to_zx_crtc(crtc)->pipe, red, green, blue);
}

static void zx_crtc_fb_gamma_get(struct drm_crtc *crtc, u16 *red, u16 *green, u16 *blue, int reqno)
{
    DRM_DEBUG_KMS("crtc=%d\n", to_zx_crtc(crtc)->pipe);
}
#endif

static int zxfb_create(struct drm_fb_helper *helper, struct drm_fb_helper_surface_size *sizes)
{
    int ret = 0;
    struct fb_info *info;
    struct zx_fbdev *fbdev = to_zx_fbdev(helper);
    struct drm_zx_framebuffer *fb = fbdev->fb;
    struct drm_device *dev = helper->dev;
    zx_card_t *zx = dev->dev_private;
    disp_info_t *disp_info = (disp_info_t *)zx->disp_info;
    struct drm_zx_gem_object *obj = NULL;
    int linear_status;

    DRM_DEBUG_KMS("fb_width=%d,fb_height=%d,surface_width=%d,surface_height=%d,surface_bpp=%d,surface_depth=%d\n",
            sizes->fb_width, sizes->fb_height, sizes->surface_width, sizes->surface_height, sizes->surface_bpp, sizes->surface_depth);

    mutex_lock(&dev->struct_mutex);

    if (fb && (sizes->fb_width > fb->base.width || sizes->fb_height > fb->base.height))
    {
#if DRM_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
        drm_framebuffer_unreference(&fb->base);
#else
        drm_framebuffer_put(&fb->base);
#endif
        fbdev->fb = NULL;
        fb = NULL;
    }

    if (!fb)
    {
        zx_create_allocation_t *create = zx_calloc(sizeof(zx_create_allocation_t));
        struct drm_mode_fb_cmd2 *mode_cmd = zx_calloc(sizeof(struct drm_mode_fb_cmd2));

        linear_status = disp_enable_disable_linear_vga(disp_info, 1, 0);
        create->device   = fbdev->gpu_device;
        create->width    = sizes->surface_width;
        create->height   = sizes->surface_height;
        create->format   = (sizes->surface_bpp == 32) ? ZX_FORMAT_B8G8R8A8_UNORM : ZX_FORMAT_B5G6R5_UNORM;
        create->tiled    = 0;
        create->unpagable    = TRUE;
        create->usage_mask   = ZX_USAGE_DISPLAY_SURFACE | ZX_USAGE_FRAMEBUFFER;
        create->access_hint  = ZX_ACCESS_CPU_ALMOST;

        obj = zx_drm_gem_create_object(zx, create, &fbdev->debug);
        zx_assert(obj != NULL && obj->core_handle != 0);

        mode_cmd->width = sizes->surface_width;
        mode_cmd->height = sizes->surface_height;
        mode_cmd->pitches[0] = obj->info.pitch;
        mode_cmd->pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);
        fb = __zx_framebuffer_create(dev, mode_cmd, obj);
        fbdev->fb = fb;
        disp_enable_disable_linear_vga(disp_info, linear_status, 1);

        zx_free(mode_cmd);
        zx_free(create);
    }
    else
    {
        sizes->fb_width = fb->base.width;
        sizes->fb_height = fb->base.height;
    }

#if DRM_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
    info = framebuffer_alloc(0, &dev->pdev->dev);
    zx_assert(info != NULL);

    info->par = helper;
    info->flags = FBINFO_DEFAULT | FBINFO_CAN_FORCE_OUTPUT;
    info->fbops = &zxfb_ops;

    ret = fb_alloc_cmap(&info->cmap, 256, 0);
    zx_assert(ret == 0);

    info->apertures = alloc_apertures(1);
    zx_assert(info->apertures != NULL);

    info->apertures->ranges[0].base = dev->mode_config.fb_base;
    info->apertures->ranges[0].size = obj->info.gpu_virt_addr + obj->info.pitch * obj->info.height;
    helper->fbdev = info;
#else
    info = drm_fb_helper_alloc_fbi(helper);
    info->par = helper;
    info->flags = FBINFO_DEFAULT;
    info->fbops = &zxfb_ops;
#endif

#if DRM_VERSION_CODE < KERNEL_VERSION(4, 20, 0)
    info->flags |= FBINFO_CAN_FORCE_OUTPUT;
#endif

    info->skip_vt_switch = true;
    fbdev->helper.fb = &fb->base;
    zx_vsprintf(info->fix.id, "%s", "zxfb");

    info->fix.smem_start = obj->info.cpu_phy_addr;
    info->fix.smem_len = obj->info.pitch * obj->info.aligned_height;

//map fbdev fb  gem object
    info->screen_base = zx_gem_object_vmap(obj);
    info->screen_size = obj->info.pitch * obj->info.aligned_height;

    info->skip_vt_switch = true;
#if DRM_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
    drm_fb_helper_fill_fix(info, fb->base.pitches[0], fb->base.depth);
    drm_fb_helper_fill_var(info, &fbdev->helper, sizes->fb_width, sizes->fb_height);
#elif DRM_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
    drm_fb_helper_fill_fix(info, fb->base.pitches[0], fb->base.format->depth);
    drm_fb_helper_fill_var(info, &fbdev->helper, sizes->fb_width, sizes->fb_height);
#else
    drm_fb_helper_fill_info(info, &fbdev->helper, sizes);
#endif

    DRM_DEBUG_KMS("screen_base=0x%p,screen_size=0x%lx,smem_start=0x%lx,smem_len=0x%x,xres=%d,yres=%d\n",
            info->screen_base, info->screen_size, info->fix.smem_start, info->fix.smem_len, info->var.xres, info->var.yres);

    mutex_unlock(&dev->struct_mutex);
    return 0;
}

static const struct drm_fb_helper_funcs zx_fb_helper_funcs = {
#if DRM_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
    .initial_config     = zx_fb_initial_config,
#endif
#if DRM_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
    .gamma_set          = zx_crtc_fb_gamma_set,
    .gamma_get          = zx_crtc_fb_gamma_get,
#endif    
    .fb_probe           = zxfb_create,
};

void zx_fbdev_disable_vesa(zx_card_t *zx)
{
    struct fb_info *vesafb = NULL;
    bus_config_t *bus_config = zx_calloc(sizeof(bus_config_t));
    int i, vesafb_found = 0, vga16fb_found = 0;

    zx_get_bus_config(zx->pdev, bus_config);
    for(i = 0; i < num_registered_fb; i++)
    {
        vesafb = registered_fb[i];
        if((vesafb != NULL) && 
            (vesafb->fix.smem_start >= bus_config->mem_start_addr[0]) && 
            (vesafb->fix.smem_start <  bus_config->mem_end_addr[0]))
        {
            vesafb_found = 1;
            break;
        }

        if((vesafb != NULL) &&
            (vesafb->fix.smem_start == 0xA0000))
        {
            vga16fb_found = 1;
            break;
        }
    }

    if (vesafb_found || vga16fb_found)
    {
        zx_info("%s detected, conflicted with zxfb, remove %s.\n", vesafb->fix.id, vesafb->fix.id);

        unregister_framebuffer(vesafb);
    }

    zx_free(bus_config);
}

int zx_fbdev_init(zx_card_t *zx)
{
    int ret;
    struct zx_fbdev *fbdev;

    DRM_DEBUG_KMS("zx=%p\n", zx);

    fbdev = zx_calloc(sizeof(*fbdev));
    zx_assert(fbdev != NULL);

    ret = zx_core_interface->create_device(zx->adapter, NULL, &fbdev->gpu_device);
    zx_assert(ret == 0);

    fbdev->debug = zx_debugfs_add_device_node(zx->debugfs_dev, zx_get_current_pid(), fbdev->gpu_device);

    drm_fb_helper_prepare(zx->drm_dev, &fbdev->helper, &zx_fb_helper_funcs);

#if DRM_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
    ret = drm_fb_helper_init(zx->drm_dev, &fbdev->helper, ((disp_info_t*)(zx->disp_info))->num_crtc, 4);
#elif DRM_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
    ret = drm_fb_helper_init(zx->drm_dev, &fbdev->helper, 4);
#else
    ret = drm_fb_helper_init(zx->drm_dev, &fbdev->helper);
#endif    
    zx_assert(ret == 0);

#if DRM_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
    drm_fb_helper_single_add_all_connectors(&fbdev->helper);
#endif

    drm_fb_helper_initial_config(&fbdev->helper, 32);

    zx->fbdev = fbdev;

    return 0;
}


int zx_fbdev_deinit(zx_card_t *zx)
{
    struct zx_fbdev *fbdev = zx->fbdev;
    struct drm_zx_framebuffer *fb;
    struct fb_info *info;

    if (!fbdev)
    {
        return 0;
    }

    fb = fbdev->fb;

    info = fbdev->helper.fbdev;
#if DRM_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
    unregister_framebuffer(info);
#else
    drm_fb_helper_unregister_fbi(&fbdev->helper);
#endif

#if DRM_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
    iounmap(info->screen_base);
    if (info->cmap.len)
        fb_dealloc_cmap(&info->cmap);
    framebuffer_release(info);
#elif DRM_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
    drm_fb_helper_release_fbi(&fbdev->helper);
#endif    

    if (fb && fb->obj)
    {
        zx_gem_object_vunmap(fb->obj);
        if(fb->obj->core_handle)
        {
            zx_gem_object_put(fb->obj);
        }
        else
        {
            zx_free(fb->obj);
        }
        fb->obj = NULL;
    }

    drm_fb_helper_fini(&fbdev->helper);
    if(fbdev->fb)
    {
        drm_framebuffer_remove(&fbdev->fb->base);
    }

    zx_debugfs_remove_device_node(zx->debugfs_dev, fbdev->debug);
    zx_core_interface->destroy_device(zx->adapter, fbdev->gpu_device);

    zx_free(fbdev);
    zx->fbdev = NULL;

    return 0;
}

void zx_fbdev_set_suspend(zx_card_t *zx, int state)
{
    struct zx_fbdev *fbdev = zx->fbdev;
    struct fb_info *info;

    if (fbdev)
    {
        info = fbdev->helper.fbdev;

        console_lock();
        
#if DRM_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
        if (info)
        {
            fb_set_suspend(info, state);
        }
#else
        drm_fb_helper_set_suspend(&fbdev->helper, state);
#endif

        console_unlock();
    }
}

void zx_fbdev_poll_changed(struct drm_device *dev)
{
    zx_card_t*  zx = dev->dev_private;
    struct zx_fbdev *fbdev = zx->fbdev;

#if DRM_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
    if (fbdev && fbdev->fb)
#else
    if (fbdev)
#endif        
    {
        drm_fb_helper_hotplug_event(&fbdev->helper);
    }
}
