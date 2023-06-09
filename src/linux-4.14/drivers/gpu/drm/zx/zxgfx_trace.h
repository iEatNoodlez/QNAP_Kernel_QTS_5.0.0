/*
* Copyright 1998-2014 VIA Technologies, Inc. All Rights Reserved.
* Copyright 2001-2014 S3 Graphics, Inc. All Rights Reserved.
* Copyright 2013-2014 Shanghai Zhaoxin Semiconductor Co., Ltd. All Rights Reserved.
*
* This file is part of zx.ko
* 
* zx.ko is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* zx.ko is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#undef TRACE_SYSTEM
#define TRACE_SYSTEM zxgfx

#if !defined(_ZXGFX_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _ZXGFX_TRACE_H

#if ZX_TRACE_EVENT
#include <linux/tracepoint.h>
#include "zx_fence.h"
#include "zx_gem.h"


TRACE_EVENT(zxgfx_fence_back,

    TP_PROTO(unsigned long jif_time, unsigned long engine_index, unsigned long long fence),

    TP_ARGS(jif_time, engine_index, fence),

    TP_STRUCT__entry(
        __field(unsigned long, jif_time)
        __field(unsigned long, engine_index)
        __field(unsigned long long, fence)
    ),

    TP_fast_assign(
        __entry->jif_time = jif_time;
        __entry->engine_index = engine_index;
        __entry->fence = fence;
    ),

    TP_printk("jif_time = %lu, engine_index = %lu, fence = %llu", __entry->jif_time, __entry->engine_index, __entry->fence)
);

TRACE_EVENT(zxgfx_cmd_render,

    TP_PROTO(unsigned long jif_time, unsigned long engine_index, void *context, unsigned long long render_counter),

    TP_ARGS(jif_time, engine_index, context, render_counter),

    TP_STRUCT__entry(
        __field(unsigned long, jif_time)
        __field(unsigned long, engine_index)
        __field(void *,        context)
        __field(unsigned long long, render_counter)
    ),

    TP_fast_assign(
        __entry->jif_time = jif_time;
        __entry->engine_index = engine_index;
        __entry->context      = context;
        __entry->render_counter = render_counter;
   ),

    TP_printk("jif_time = %lu, engine_index = %lu, context = %p, render_counter = %llu", __entry->jif_time, __entry->engine_index, __entry->context, __entry->render_counter)
);

TRACE_EVENT(zxgfx_dma_submit,

   TP_PROTO(unsigned long jif_time, unsigned long engine_index, void *context, unsigned long long render_counter, unsigned long long fence),

   TP_ARGS(jif_time, engine_index, context, render_counter, fence),

   TP_STRUCT__entry(
      __field(unsigned long, jif_time)
      __field(unsigned long, engine_index)
      __field(void *,        context)
      __field(unsigned long long, render_counter)
      __field(unsigned long long, fence)
   ), 

   TP_fast_assign(
      __entry->jif_time = jif_time;
      __entry->engine_index = engine_index;
      __entry->context      = context;
      __entry->render_counter = render_counter;
      __entry->fence = fence;
   ),

   TP_printk("jif_time = %lu, engine_index = %lu, context = %p, render_counter = %llu, fence = %llu", __entry->jif_time, __entry->engine_index, __entry->context, __entry->render_counter, __entry->fence)
);

TRACE_EVENT(zxgfx_vblank_intrr,
TP_PROTO(unsigned long jiff, unsigned int index, unsigned int cnt),
TP_ARGS(jiff, index, cnt),
TP_STRUCT__entry(
    __field(unsigned long, jiff)
    __field(unsigned int, index)
    __field(unsigned int, cnt)
),
TP_fast_assign(
    __entry->jiff = jiff;
    __entry->index = index;
    __entry->cnt = cnt;
),
TP_printk("Jiffies = %lu, crtc_index = %d, vbl_count = %d", __entry->jiff, __entry->index, __entry->cnt)
);

TRACE_EVENT(zxgfx_vblank_onoff,
TP_PROTO(unsigned long jiff, int index, int on),
TP_ARGS(jiff, index, on),
TP_STRUCT__entry(
    __field(unsigned long, jiff)
    __field(int, index)
    __field(int, on)
),
TP_fast_assign(
    __entry->jiff = jiff;
    __entry->index = index;
    __entry->on = on;
),
TP_printk("Jiffies = %lu, crtc_index = %d, vblank on = %d", __entry->jiff, __entry->index, __entry->on)
);

TRACE_EVENT(zxgfx_drm_gem_create_object,
        TP_PROTO(struct drm_zx_gem_object *obj),
        TP_ARGS(obj),

        TP_STRUCT__entry(
            __field(struct drm_zx_gem_object *, obj)
            __field(u64, size)
            ),

        TP_fast_assign(
            __entry->obj = obj;
            __entry->size = obj->base.size;
            ),

        TP_printk("obj=%p/0x%llx", __entry->obj, __entry->size)
);

TRACE_EVENT(zxgfx_drm_gem_release_object,
        TP_PROTO(struct drm_zx_gem_object *obj),
        TP_ARGS(obj),

        TP_STRUCT__entry(
            __field(struct drm_zx_gem_object *, obj)
            ),

        TP_fast_assign(
            __entry->obj = obj;
            ),

        TP_printk("obj=%p", __entry->obj)
);

TRACE_EVENT(zxgfx_drm_gem_create_resource,
        TP_PROTO(int count, struct drm_zx_gem_object **objs),
        TP_ARGS(count, objs),

        TP_STRUCT__entry(
            __field(int, count)
            __array(u64, sizes, 4)
            __array(struct drm_zx_gem_object *, objs, 4)
            ),

        TP_fast_assign(
            __entry->objs[0] = count > 0 ? objs[0] : NULL;
            __entry->objs[1] = count > 1 ? objs[1] : NULL;
            __entry->objs[2] = count > 2 ? objs[2] : NULL;
            __entry->objs[3] = count > 3 ? objs[3] : NULL;
            __entry->sizes[0] = count > 0 ? objs[0]->base.size : 0;
            __entry->sizes[1] = count > 1 ? objs[1]->base.size : 0;
            __entry->sizes[2] = count > 2 ? objs[2]->base.size : 0;
            __entry->sizes[3] = count > 3 ? objs[3]->base.size : 0;
            __entry->count = count;
            ),

        TP_printk("objs=[%p/0x%llx,%p/0x%llx,%p/0x%llx,%p/0x%llx]", 
            __entry->objs[0], __entry->sizes[0],
            __entry->objs[1], __entry->sizes[1],
            __entry->objs[2], __entry->sizes[2],
            __entry->objs[3], __entry->sizes[3])
);

TRACE_EVENT(zxgfx_gem_prime_import,
        TP_PROTO(struct dma_buf *dma_buf, struct drm_zx_gem_object* obj),
        TP_ARGS(dma_buf, obj),

        TP_STRUCT__entry(
            __field(struct dma_buf *, dma_buf)
            __field(struct drm_zx_gem_object*, obj)
            ),

        TP_fast_assign(
            __entry->dma_buf = dma_buf;
            __entry->obj = obj;
            ),

        TP_printk("dma_buf=%p, obj=%p", __entry->dma_buf, __entry->obj)
);

TRACE_EVENT(zxgfx_gem_prime_export,
        TP_PROTO(struct dma_buf *dma_buf, struct drm_zx_gem_object* obj),
        TP_ARGS(dma_buf, obj),

        TP_STRUCT__entry(
            __field(struct dma_buf *, dma_buf)
            __field(struct drm_zx_gem_object*, obj)
            ),

        TP_fast_assign(
            __entry->dma_buf = dma_buf;
            __entry->obj = obj;
            ),

        TP_printk("dma_buf=%p, obj=%p", __entry->dma_buf, __entry->obj)
);

TRACE_EVENT(zxgfx_dma_fence_wait,
        TP_PROTO(dma_fence_t *fence, bool intr, signed long timeout),
        TP_ARGS(fence, intr, timeout),

        TP_STRUCT__entry(
            __field(dma_fence_t *, fence)
            __field(bool , intr)
            __field(signed long , timeout)
            ),

        TP_fast_assign(
            __entry->fence = fence;
            __entry->intr = intr;
            __entry->timeout = timeout;
            ),

        TP_printk("fence=%p, intr=%d, timeout=%ld", __entry->fence, __entry->intr, __entry->timeout)
);

TRACE_EVENT(zxgfx_dma_fence_enable_signaling,
        TP_PROTO(dma_fence_t *fence),
        TP_ARGS(fence),

        TP_STRUCT__entry(
            __field(dma_fence_t *, fence)
            ),

        TP_fast_assign(
            __entry->fence = fence;
            ),

        TP_printk("fence=%p", __entry->fence)
);

TRACE_EVENT(zxgfx_gem_object_begin_cpu_access,
        TP_PROTO(struct drm_zx_gem_object *allocation, long timeout, int write),
        TP_ARGS(allocation, timeout, write),

        TP_STRUCT__entry(
            __field(struct drm_zx_gem_object*, allocation)
            __field(long, timeout)
            __field(int, write)
            ),

        TP_fast_assign(
            __entry->allocation = allocation;
            __entry->timeout = timeout;
            __entry->write = write;
            ),

        TP_printk("allocation=%p, timeout=%ld, write=%d", __entry->allocation, __entry->timeout, __entry->write)
);

TRACE_EVENT(zxgfx_atomic_flush,
    TP_PROTO(long pipe, void *e),
    TP_ARGS(pipe, e),

    TP_STRUCT__entry(
        __field(long, pipe)
        __field(void *, e)
        ),

    TP_fast_assign(
        __entry->pipe = pipe;
        __entry->e = e
        ),

    TP_printk("flush pipe=%ld,e=%p ", __entry->pipe, __entry->e)
);

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE zxgfx_trace
#include <trace/define_trace.h>

#else

#define trace_zxgfx_fence_back(args...) do {} while(0)
#define trace_zxgfx_cmd_render(args...) do {} while(0)
#define trace_zxgfx_dma_submit(args...) do {} while(0)
#define trace_zxgfx_vblank_intrr(args...) do {} while(0)
#define trace_zxgfx_vblank_onoff(args...) do {} while(0)

#define trace_zxgfx_drm_gem_create_object(args...) do {} while(0)
#define trace_zxgfx_drm_gem_release_object(args...) do {} while(0)
#define trace_zxgfx_drm_gem_create_resource(args...) do {} while(0)
#define trace_zxgfx_gem_object_begin_cpu_access(args...) do {} while(0)
#define trace_zxgfx_gem_prime_import(args...) do {} while(0)
#define trace_zxgfx_gem_prime_export(args...) do {} while(0)
#define trace_zxgfx_dma_fence_wait(args...) do {} while(0)
#define trace_zxgfx_dma_fence_enable_signaling(args...) do {} while(0)
#define trace_zxgfx_atomic_flush(args...) do {} while(0)

#endif

void zx_fence_back_trace_event(int engine_index, unsigned long long fence);
void zx_cmd_render_trace_event(int engine_index, void *context, unsigned long long render_counter);
void zx_dma_submit_trace_event(int engine_index, void *context, unsigned long long render_counter, unsigned long long fence);
void zx_vblank_onoff_event(int index, int on);
void zx_vblank_intrr_trace_event(unsigned int index, unsigned int cnt);
void zx_register_trace_events(void);
void zx_unregister_trace_events(void);

#endif

/*_ZXGFX_TRACE_H */


