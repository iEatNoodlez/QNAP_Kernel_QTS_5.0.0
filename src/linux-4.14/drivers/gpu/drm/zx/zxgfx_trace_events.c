/** Copyright (c) 2010-2012, S3 Graphics, Inc.
* Copyright (c) 2010-2012, S3 Graphics Co., Ltd.
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
#include "os_interface.h"
#include "zx.h"

#define CREATE_TRACE_POINTS
#include "zxgfx_trace.h"


/* os interface for trace points */
void zx_fence_back_trace_event(int engine_index, unsigned long long fence)
{
    trace_zxgfx_fence_back(jiffies, engine_index, fence);
}

void zx_cmd_render_trace_event(int engine_index, void *context, unsigned long long render_counter)
{
    trace_zxgfx_cmd_render(jiffies, engine_index, context, render_counter);
}

void zx_dma_submit_trace_event(int engine_index, void *context, unsigned long long render_counter, unsigned long long fence)
{
    trace_zxgfx_dma_submit(jiffies, engine_index, context, render_counter, fence);
}

void zx_vblank_onoff_event(int index, int on)
{
    trace_zxgfx_vblank_onoff(jiffies, index, on);
}

void zx_vblank_intrr_trace_event(unsigned int index, unsigned int cnt)
{    
    trace_zxgfx_vblank_intrr(jiffies, index, cnt);
}

/* register/unregister probe functions */
void zx_register_trace_events(void)
{

}

void zx_unregister_trace_events(void)
{

}

