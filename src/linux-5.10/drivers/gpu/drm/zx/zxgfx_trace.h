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


TRACE_EVENT(zxgfx_task_create,

    TP_PROTO(int engine_index, unsigned int context, unsigned long long task_id, unsigned int task_type),

    TP_ARGS(engine_index, context, task_id, task_type),

    TP_STRUCT__entry(
        __field(int, engine_index)
        __field(unsigned int, context)
        __field(unsigned long long, task_id)
        __field(unsigned int, task_type)
    ),

    TP_fast_assign(
        __entry->engine_index = engine_index;
        __entry->context = context;
        __entry->task_id = task_id;
        __entry->task_type = task_type;
   ),

    TP_printk("engine_index=%d, context=0x%x, task_id=%llu, task_type=%u",
        __entry->engine_index, __entry->context, __entry->task_id, __entry->task_type)
);

TRACE_EVENT(zxgfx_task_submit,

    TP_PROTO(int engine_index, unsigned int context, unsigned long long task_id, unsigned int task_type, unsigned long long fence_id, unsigned int args),

    TP_ARGS(engine_index, context, task_id, task_type, fence_id, args),

    TP_STRUCT__entry(
        __field(int, engine_index)
        __field(unsigned int, context)
        __field(unsigned long long, task_id)
        __field(unsigned int, task_type)
        __field(unsigned long long, fence_id)
        __field(unsigned int, args)
    ),

    TP_fast_assign(
        __entry->engine_index = engine_index;
        __entry->context = context;
        __entry->task_id = task_id;
        __entry->task_type = task_type;
        __entry->fence_id = fence_id;
        __entry->args = args;
   ),

    TP_printk("engine_index=%d, context=0x%x, task_id=%llu, task_type=%u, fence_id=%llu, args=%u",
        __entry->engine_index, __entry->context, __entry->task_id, __entry->task_type, __entry->fence_id, __entry->args)
);

TRACE_EVENT(zxgfx_fence_back,

    TP_PROTO(unsigned long engine_index, unsigned long long fence_id),

    TP_ARGS(engine_index, fence_id),

    TP_STRUCT__entry(
        __field(unsigned long, engine_index)
        __field(unsigned long long, fence_id)
    ),

    TP_fast_assign(
        __entry->engine_index = engine_index;
        __entry->fence_id = fence_id;
    ),

    TP_printk("engine_index=%lu, fence_id=%llu", __entry->engine_index, __entry->fence_id)
);

TRACE_EVENT(zxgfx_vblank_intrr,
    TP_PROTO(unsigned int index, unsigned int cnt),

    TP_ARGS(index, cnt),

    TP_STRUCT__entry(
        __field(unsigned int, index)
        __field(unsigned int, cnt)
    ),

    TP_fast_assign(
        __entry->index = index;
        __entry->cnt = cnt;
    ),

    TP_printk("crtc_index=%d, vbl_count=%d", __entry->index, __entry->cnt)
);

TRACE_EVENT(zxgfx_vblank_onoff,
    TP_PROTO(int index, int on),

    TP_ARGS(index, on),

    TP_STRUCT__entry(
        __field(int, index)
        __field(int, on)
    ),

    TP_fast_assign(
        __entry->index = index;
        __entry->on = on;
    ),

    TP_printk("crtc_index=%d, vblank on=%d", __entry->index, __entry->on)
);

TRACE_EVENT(zxgfx_drm_gem_create_object,
        TP_PROTO(struct drm_zx_gem_object *obj),
        TP_ARGS(obj),

        TP_STRUCT__entry(
            __field(unsigned int, allocation)
            __field(u64, size)
            ),

        TP_fast_assign(
            __entry->allocation = obj->core_handle;
            __entry->size = obj->base.size;
            ),

        TP_printk("allocation=%x/0x%llx", __entry->allocation, __entry->size)
);

TRACE_EVENT(zxgfx_drm_gem_release_object,
        TP_PROTO(struct drm_zx_gem_object *obj),
        TP_ARGS(obj),

        TP_STRUCT__entry(
            __field(unsigned int, allocation)
            ),

        TP_fast_assign(
            __entry->allocation = obj->core_handle;
            ),

        TP_printk("allocation=%x", __entry->allocation)
);

TRACE_EVENT(zxgfx_drm_gem_create_resource,
        TP_PROTO(int count, struct drm_zx_gem_object **objs),
        TP_ARGS(count, objs),

        TP_STRUCT__entry(
            __field(int, count)
            __array(u64, sizes, 4)
            __array(unsigned int, allocations, 4)
            ),

        TP_fast_assign(
            __entry->allocations[0] = count > 0 ? objs[0]->core_handle : 0;
            __entry->allocations[1] = count > 1 ? objs[1]->core_handle : 0;
            __entry->allocations[2] = count > 2 ? objs[2]->core_handle : 0;
            __entry->allocations[3] = count > 3 ? objs[3]->core_handle : 0;
            __entry->sizes[0] = count > 0 ? objs[0]->base.size : 0;
            __entry->sizes[1] = count > 1 ? objs[1]->base.size : 0;
            __entry->sizes[2] = count > 2 ? objs[2]->base.size : 0;
            __entry->sizes[3] = count > 3 ? objs[3]->base.size : 0;
            __entry->count = count;
            ),

        TP_printk("allocations=[%x/0x%llx,%x/0x%llx,%x/0x%llx,%x/0x%llx]",
            __entry->allocations[0], __entry->sizes[0],
            __entry->allocations[1], __entry->sizes[1],
            __entry->allocations[2], __entry->sizes[2],
            __entry->allocations[3], __entry->sizes[3])
);

TRACE_EVENT(zxgfx_gem_prime_import,
        TP_PROTO(struct dma_buf *dma_buf, struct drm_zx_gem_object* obj),
        TP_ARGS(dma_buf, obj),

        TP_STRUCT__entry(
            __field(struct dma_buf *, dma_buf)
            __field(unsigned int, allocation)
            __field(u64, size)
            ),

        TP_fast_assign(
            __entry->dma_buf = dma_buf;
            __entry->allocation = obj->core_handle;
            __entry->size = obj->base.size;
            ),

        TP_printk("dma_buf=%p, allocation=%x/0x%llx", __entry->dma_buf, __entry->allocation, __entry->size)
);

TRACE_EVENT(zxgfx_gem_prime_export,
        TP_PROTO(struct dma_buf *dma_buf, struct drm_zx_gem_object* obj),
        TP_ARGS(dma_buf, obj),

        TP_STRUCT__entry(
            __field(struct dma_buf *, dma_buf)
            __field(unsigned int, allocation)
            ),

        TP_fast_assign(
            __entry->dma_buf = dma_buf;
            __entry->allocation = obj->core_handle;
            ),

        TP_printk("dma_buf=%p, allocation=%x", __entry->dma_buf, __entry->allocation)
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
        TP_PROTO(struct drm_zx_gem_object *obj, long timeout, int write),
        TP_ARGS(obj, timeout, write),

        TP_STRUCT__entry(
            __field(unsigned int, allocation)
            __field(long, timeout)
            __field(int, write)
            ),

        TP_fast_assign(
            __entry->allocation = obj->core_handle;
            __entry->timeout = timeout;
            __entry->write = write;
            ),

        TP_printk("allocation=%x, timeout=%ld, write=%d", __entry->allocation, __entry->timeout, __entry->write)
);

TRACE_EVENT(zxgfx_gem_object_end_cpu_access,
        TP_PROTO(struct drm_zx_gem_object *obj),
        TP_ARGS(obj),

        TP_STRUCT__entry(
            __field(unsigned int, allocation)
            ),

        TP_fast_assign(
            __entry->allocation = obj->core_handle;
            ),

        TP_printk("allocation=%x", __entry->allocation)
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

TRACE_EVENT(zxgfx_crtc_flip,
    TP_PROTO(int crtc, int stream_type, struct drm_zx_gem_object* obj),
    TP_ARGS(crtc, stream_type, obj),

    TP_STRUCT__entry(
        __field(int, crtc)
        __field(int, stream_type)
        __field(unsigned int, allocation)
        ),

    TP_fast_assign(
        __entry->crtc = crtc;
        __entry->stream_type = stream_type;
        __entry->allocation = obj->core_handle;
        ),

    TP_printk("crtc=%d, stream_type=%d, allocation=%x", __entry->crtc, __entry->stream_type, __entry->allocation)
);

TRACE_EVENT(zxgfx_begin_section,
    TP_PROTO(const char *desc),
    TP_ARGS(desc),

    TP_STRUCT__entry(
        __field(const char *, desc)
    ),

    TP_fast_assign(
        __entry->desc = desc;
    ),

    TP_printk("begin=%s", __entry->desc)
);

TRACE_EVENT(zxgfx_end_section,
    TP_PROTO(int result),
    TP_ARGS(result),

    TP_STRUCT__entry(
        __field(int, result)
    ),

    TP_fast_assign(
        __entry->result = result;
    ),

    TP_printk("end=%d", __entry->result)
);

TRACE_EVENT(zxgfx_counter,
    TP_PROTO(const char *desc, unsigned int value),
    TP_ARGS(desc, value),

    TP_STRUCT__entry(
        __field(const char *, desc)
        __field(unsigned int, value)
    ),

    TP_fast_assign(
        __entry->desc = desc;
        __entry->value = value;
    ),

    TP_printk("desc=%s, val=%u", __entry->desc, __entry->value)
);

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE zxgfx_trace
#include <trace/define_trace.h>

#else

#define trace_zxgfx_task_create(args...) do {} while(0)
#define trace_zxgfx_task_submit(args...) do {} while(0)
#define trace_zxgfx_fence_back(args...) do {} while(0)
#define trace_zxgfx_vblank_intrr(args...) do {} while(0)
#define trace_zxgfx_vblank_onoff(args...) do {} while(0)

#define trace_zxgfx_drm_gem_create_object(args...) do {} while(0)
#define trace_zxgfx_drm_gem_release_object(args...) do {} while(0)
#define trace_zxgfx_drm_gem_create_resource(args...) do {} while(0)
#define trace_zxgfx_gem_object_begin_cpu_access(args...) do {} while(0)
#define trace_zxgfx_gem_object_end_cpu_access(args...) do {} while(0)
#define trace_zxgfx_gem_prime_import(args...) do {} while(0)
#define trace_zxgfx_gem_prime_export(args...) do {} while(0)
#define trace_zxgfx_dma_fence_wait(args...) do {} while(0)
#define trace_zxgfx_dma_fence_enable_signaling(args...) do {} while(0)
#define trace_zxgfx_atomic_flush(args...) do {} while(0)
#define trace_zxgfx_crtc_flip(args...) do {} while(0)

#define trace_zxgfx_begin_section(args...) do {} while(0)
#define trace_zxgfx_end_section(args...) do {} while(0)
#define trace_zxgfx_counter(args...) do {} while(0)

#endif

#endif

/*_ZXGFX_TRACE_H */


