/*
 * Copyright 2016 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include "dm_services.h"
#include "dc.h"
#include "mod_freesync.h"
#include "core_types.h"

#define MOD_FREESYNC_MAX_CONCURRENT_STREAMS  32

#define MIN_REFRESH_RANGE_IN_US 10000000
/* Refresh rate ramp at a fixed rate of 65 Hz/second */
#define STATIC_SCREEN_RAMP_DELTA_REFRESH_RATE_PER_FRAME ((1000 / 60) * 65)
/* Number of elements in the render times cache array */
#define RENDER_TIMES_MAX_COUNT 10
/* Threshold to exit BTR (to avoid frequent enter-exits at the lower limit) */
#define BTR_EXIT_MARGIN 2000
/* Number of consecutive frames to check before entering/exiting fixed refresh*/
#define FIXED_REFRESH_ENTER_FRAME_COUNT 5
#define FIXED_REFRESH_EXIT_FRAME_COUNT 5

struct core_freesync {
	struct mod_freesync public;
	struct dc *dc;
};

#define MOD_FREESYNC_TO_CORE(mod_freesync)\
		container_of(mod_freesync, struct core_freesync, public)

struct mod_freesync *mod_freesync_create(struct dc *dc)
{
	struct core_freesync *core_freesync =
			kzalloc(sizeof(struct core_freesync), GFP_KERNEL);

	if (core_freesync == NULL)
		goto fail_alloc_context;

	if (dc == NULL)
		goto fail_construct;

	core_freesync->dc = dc;
	return &core_freesync->public;

fail_construct:
	kfree(core_freesync);

fail_alloc_context:
	return NULL;
}

void mod_freesync_destroy(struct mod_freesync *mod_freesync)
{
	struct core_freesync *core_freesync = NULL;
	if (mod_freesync == NULL)
		return;
	core_freesync = MOD_FREESYNC_TO_CORE(mod_freesync);
	kfree(core_freesync);
}

#if 0 /* unused currently */
static unsigned int calc_refresh_in_uhz_from_duration(
		unsigned int duration_in_ns)
{
	unsigned int refresh_in_uhz =
			((unsigned int)(div64_u64((1000000000ULL * 1000000),
					duration_in_ns)));
	return refresh_in_uhz;
}
#endif

static unsigned int calc_duration_in_us_from_refresh_in_uhz(
		unsigned int refresh_in_uhz)
{
	unsigned int duration_in_us =
			((unsigned int)(div64_u64((1000000000ULL * 1000),
					refresh_in_uhz)));
	return duration_in_us;
}

static unsigned int calc_duration_in_us_from_v_total(
		const struct dc_stream_state *stream,
		const struct mod_vrr_params *in_vrr,
		unsigned int v_total)
{
	unsigned int duration_in_us =
			(unsigned int)(div64_u64(((unsigned long long)(v_total)
				* 1000) * stream->timing.h_total,
					stream->timing.pix_clk_khz));

	if (duration_in_us < in_vrr->min_duration_in_us)
		duration_in_us = in_vrr->min_duration_in_us;

	if (duration_in_us > in_vrr->max_duration_in_us)
		duration_in_us = in_vrr->max_duration_in_us;

	return duration_in_us;
}

static unsigned int calc_v_total_from_refresh(
		const struct dc_stream_state *stream,
		unsigned int refresh_in_uhz)
{
	unsigned int v_total = stream->timing.v_total;
	unsigned int frame_duration_in_ns;

	frame_duration_in_ns =
			((unsigned int)(div64_u64((1000000000ULL * 1000000),
					refresh_in_uhz)));

	v_total = div64_u64(div64_u64(((unsigned long long)(
			frame_duration_in_ns) * stream->timing.pix_clk_khz),
			stream->timing.h_total), 1000000);

	/* v_total cannot be less than nominal */
	if (v_total < stream->timing.v_total) {
		ASSERT(v_total < stream->timing.v_total);
		v_total = stream->timing.v_total;
	}

	return v_total;
}

static unsigned int calc_v_total_from_duration(
		const struct dc_stream_state *stream,
		const struct mod_vrr_params *vrr,
		unsigned int duration_in_us)
{
	unsigned int v_total = 0;

	if (duration_in_us < vrr->min_duration_in_us)
		duration_in_us = vrr->min_duration_in_us;

	if (duration_in_us > vrr->max_duration_in_us)
		duration_in_us = vrr->max_duration_in_us;

	v_total = div64_u64(div64_u64(((unsigned long long)(
				duration_in_us) * stream->timing.pix_clk_khz),
				stream->timing.h_total), 1000);

	/* v_total cannot be less than nominal */
	if (v_total < stream->timing.v_total) {
		ASSERT(v_total < stream->timing.v_total);
		v_total = stream->timing.v_total;
	}

	return v_total;
}

static void update_v_total_for_static_ramp(
		struct core_freesync *core_freesync,
		const struct dc_stream_state *stream,
		struct mod_vrr_params *in_out_vrr)
{
	unsigned int v_total = 0;
	unsigned int current_duration_in_us =
			calc_duration_in_us_from_v_total(
				stream, in_out_vrr,
				in_out_vrr->adjust.v_total_max);
	unsigned int target_duration_in_us =
			calc_duration_in_us_from_refresh_in_uhz(
				in_out_vrr->fixed.target_refresh_in_uhz);
	bool ramp_direction_is_up = (current_duration_in_us >
				target_duration_in_us) ? true : false;

	/* Calc ratio between new and current frame duration with 3 digit */
	unsigned int frame_duration_ratio = div64_u64(1000000,
		(1000 +  div64_u64(((unsigned long long)(
		STATIC_SCREEN_RAMP_DELTA_REFRESH_RATE_PER_FRAME) *
		current_duration_in_us),
		1000000)));

	/* Calculate delta between new and current frame duration in us */
	unsigned int frame_duration_delta = div64_u64(((unsigned long long)(
		current_duration_in_us) *
		(1000 - frame_duration_ratio)), 1000);

	/* Adjust frame duration delta based on ratio between current and
	 * standard frame duration (frame duration at 60 Hz refresh rate).
	 */
	unsigned int ramp_rate_interpolated = div64_u64(((unsigned long long)(
		frame_duration_delta) * current_duration_in_us), 16666);

	/* Going to a higher refresh rate (lower frame duration) */
	if (ramp_direction_is_up) {
		/* reduce frame duration */
		current_duration_in_us -= ramp_rate_interpolated;

		/* adjust for frame duration below min */
		if (current_duration_in_us <= target_duration_in_us) {
			in_out_vrr->fixed.ramping_active = false;
			in_out_vrr->fixed.ramping_done = true;
			current_duration_in_us =
				calc_duration_in_us_from_refresh_in_uhz(
				in_out_vrr->fixed.target_refresh_in_uhz);
		}
	/* Going to a lower refresh rate (larger frame duration) */
	} else {
		/* increase frame duration */
		current_duration_in_us += ramp_rate_interpolated;

		/* adjust for frame duration above max */
		if (current_duration_in_us >= target_duration_in_us) {
			in_out_vrr->fixed.ramping_active = false;
			in_out_vrr->fixed.ramping_done = true;
			current_duration_in_us =
				calc_duration_in_us_from_refresh_in_uhz(
				in_out_vrr->fixed.target_refresh_in_uhz);
		}
	}

	v_total = calc_v_total_from_duration(stream,
			in_out_vrr,
			current_duration_in_us);


	in_out_vrr->adjust.v_total_min = v_total;
	in_out_vrr->adjust.v_total_max = v_total;
}

static void apply_below_the_range(struct core_freesync *core_freesync,
		const struct dc_stream_state *stream,
		unsigned int last_render_time_in_us,
		struct mod_vrr_params *in_out_vrr)
{
	unsigned int inserted_frame_duration_in_us = 0;
	unsigned int mid_point_frames_ceil = 0;
	unsigned int mid_point_frames_floor = 0;
	unsigned int frame_time_in_us = 0;
	unsigned int delta_from_mid_point_in_us_1 = 0xFFFFFFFF;
	unsigned int delta_from_mid_point_in_us_2 = 0xFFFFFFFF;
	unsigned int frames_to_insert = 0;
	unsigned int min_frame_duration_in_ns = 0;
	unsigned int max_render_time_in_us = in_out_vrr->max_duration_in_us;

	min_frame_duration_in_ns = ((unsigned int) (div64_u64(
		(1000000000ULL * 1000000),
		in_out_vrr->max_refresh_in_uhz)));

	/* Program BTR */
	if (last_render_time_in_us + BTR_EXIT_MARGIN < max_render_time_in_us) {
		/* Exit Below the Range */
		if (in_out_vrr->btr.btr_active) {
			in_out_vrr->btr.frame_counter = 0;
			in_out_vrr->btr.btr_active = false;

		/* Exit Fixed Refresh mode */
		} else if (in_out_vrr->fixed.fixed_active) {

			in_out_vrr->fixed.frame_counter++;

			if (in_out_vrr->fixed.frame_counter >
					FIXED_REFRESH_EXIT_FRAME_COUNT) {
				in_out_vrr->fixed.frame_counter = 0;
				in_out_vrr->fixed.fixed_active = false;
			}
		}
	} else if (last_render_time_in_us > max_render_time_in_us) {
		/* Enter Below the Range */
		if (!in_out_vrr->btr.btr_active &&
				in_out_vrr->btr.btr_enabled) {
			in_out_vrr->btr.btr_active = true;

		/* Enter Fixed Refresh mode */
		} else if (!in_out_vrr->fixed.fixed_active &&
				!in_out_vrr->btr.btr_enabled) {
			in_out_vrr->fixed.frame_counter++;

			if (in_out_vrr->fixed.frame_counter >
					FIXED_REFRESH_ENTER_FRAME_COUNT) {
				in_out_vrr->fixed.frame_counter = 0;
				in_out_vrr->fixed.fixed_active = true;
			}
		}
	}

	/* BTR set to "not active" so disengage */
	if (!in_out_vrr->btr.btr_active) {
		in_out_vrr->btr.btr_active = false;
		in_out_vrr->btr.inserted_duration_in_us = 0;
		in_out_vrr->btr.frames_to_insert = 0;
		in_out_vrr->btr.frame_counter = 0;

		/* Restore FreeSync */
		in_out_vrr->adjust.v_total_min =
			calc_v_total_from_refresh(stream,
				in_out_vrr->max_refresh_in_uhz);
		in_out_vrr->adjust.v_total_max =
			calc_v_total_from_refresh(stream,
				in_out_vrr->min_refresh_in_uhz);
	/* BTR set to "active" so engage */
	} else {

		/* Calculate number of midPoint frames that could fit within
		 * the render time interval- take ceil of this value
		 */
		mid_point_frames_ceil = (last_render_time_in_us +
				in_out_vrr->btr.mid_point_in_us - 1) /
					in_out_vrr->btr.mid_point_in_us;

		if (mid_point_frames_ceil > 0) {
			frame_time_in_us = last_render_time_in_us /
				mid_point_frames_ceil;
			delta_from_mid_point_in_us_1 =
				(in_out_vrr->btr.mid_point_in_us >
				frame_time_in_us) ?
				(in_out_vrr->btr.mid_point_in_us - frame_time_in_us) :
				(frame_time_in_us - in_out_vrr->btr.mid_point_in_us);
		}

		/* Calculate number of midPoint frames that could fit within
		 * the render time interval- take floor of this value
		 */
		mid_point_frames_floor = last_render_time_in_us /
				in_out_vrr->btr.mid_point_in_us;

		if (mid_point_frames_floor > 0) {

			frame_time_in_us = last_render_time_in_us /
				mid_point_frames_floor;
			delta_from_mid_point_in_us_2 =
				(in_out_vrr->btr.mid_point_in_us >
				frame_time_in_us) ?
				(in_out_vrr->btr.mid_point_in_us - frame_time_in_us) :
				(frame_time_in_us - in_out_vrr->btr.mid_point_in_us);
		}

		/* Choose number of frames to insert based on how close it
		 * can get to the mid point of the variable range.
		 */
		if (delta_from_mid_point_in_us_1 < delta_from_mid_point_in_us_2)
			frames_to_insert = mid_point_frames_ceil;
		else
			frames_to_insert = mid_point_frames_floor;

		/* Either we've calculated the number of frames to insert,
		 * or we need to insert min duration frames
		 */
		if (frames_to_insert > 0)
			inserted_frame_duration_in_us = last_render_time_in_us /
							frames_to_insert;

		if (inserted_frame_duration_in_us <
			(1000000 / in_out_vrr->max_refresh_in_uhz))
			inserted_frame_duration_in_us =
				(1000000 / in_out_vrr->max_refresh_in_uhz);

		/* Cache the calculated variables */
		in_out_vrr->btr.inserted_duration_in_us =
			inserted_frame_duration_in_us;
		in_out_vrr->btr.frames_to_insert = frames_to_insert;
		in_out_vrr->btr.frame_counter = frames_to_insert;

		in_out_vrr->adjust.v_total_min =
			calc_v_total_from_duration(stream, in_out_vrr,
				in_out_vrr->btr.inserted_duration_in_us);
		in_out_vrr->adjust.v_total_max =
				in_out_vrr->adjust.v_total_min;
	}
}

static void apply_fixed_refresh(struct core_freesync *core_freesync,
		const struct dc_stream_state *stream,
		unsigned int last_render_time_in_us,
		struct mod_vrr_params *in_out_vrr)
{
	bool update = false;
	unsigned int max_render_time_in_us = in_out_vrr->max_duration_in_us;

	if (last_render_time_in_us + BTR_EXIT_MARGIN < max_render_time_in_us) {
		/* Exit Fixed Refresh mode */
		if (in_out_vrr->fixed.fixed_active) {
			in_out_vrr->fixed.frame_counter++;

			if (in_out_vrr->fixed.frame_counter >
					FIXED_REFRESH_EXIT_FRAME_COUNT) {
				in_out_vrr->fixed.frame_counter = 0;
				in_out_vrr->fixed.fixed_active = false;
				in_out_vrr->fixed.target_refresh_in_uhz = 0;
				update = true;
			}
		}
	} else if (last_render_time_in_us > max_render_time_in_us) {
		/* Enter Fixed Refresh mode */
		if (!in_out_vrr->fixed.fixed_active) {
			in_out_vrr->fixed.frame_counter++;

			if (in_out_vrr->fixed.frame_counter >
					FIXED_REFRESH_ENTER_FRAME_COUNT) {
				in_out_vrr->fixed.frame_counter = 0;
				in_out_vrr->fixed.fixed_active = true;
				in_out_vrr->fixed.target_refresh_in_uhz =
						in_out_vrr->max_refresh_in_uhz;
				update = true;
			}
		}
	}

	if (update) {
		if (in_out_vrr->fixed.fixed_active) {
			in_out_vrr->adjust.v_total_min =
				calc_v_total_from_refresh(
				stream, in_out_vrr->max_refresh_in_uhz);
			in_out_vrr->adjust.v_total_max =
					in_out_vrr->adjust.v_total_min;
		} else {
			in_out_vrr->adjust.v_total_min =
				calc_v_total_from_refresh(
				stream, in_out_vrr->max_refresh_in_uhz);
			in_out_vrr->adjust.v_total_max =
				in_out_vrr->adjust.v_total_min;
		}
	}
}

static bool vrr_settings_require_update(struct core_freesync *core_freesync,
		struct mod_freesync_config *in_config,
		unsigned int min_refresh_in_uhz,
		unsigned int max_refresh_in_uhz,
		struct mod_vrr_params *in_vrr)
{
	if (in_vrr->state != in_config->state) {
		return true;
	} else if (in_vrr->state == VRR_STATE_ACTIVE_FIXED &&
			in_vrr->fixed.target_refresh_in_uhz !=
					in_config->min_refresh_in_uhz) {
		return true;
	} else if (in_vrr->min_refresh_in_uhz != min_refresh_in_uhz) {
		return true;
	} else if (in_vrr->max_refresh_in_uhz != max_refresh_in_uhz) {
		return true;
	}

	return false;
}

bool mod_freesync_get_vmin_vmax(struct mod_freesync *mod_freesync,
		const struct dc_stream_state *stream,
		unsigned int *vmin,
		unsigned int *vmax)
{
	*vmin = stream->adjust.v_total_min;
	*vmax = stream->adjust.v_total_max;

	return true;
}

bool mod_freesync_get_v_position(struct mod_freesync *mod_freesync,
		struct dc_stream_state *stream,
		unsigned int *nom_v_pos,
		unsigned int *v_pos)
{
	struct core_freesync *core_freesync = NULL;
	struct crtc_position position;

	if (mod_freesync == NULL)
		return false;

	core_freesync = MOD_FREESYNC_TO_CORE(mod_freesync);

	if (dc_stream_get_crtc_position(core_freesync->dc, &stream, 1,
					&position.vertical_count,
					&position.nominal_vcount)) {

		*nom_v_pos = position.nominal_vcount;
		*v_pos = position.vertical_count;

		return true;
	}

	return false;
}

void mod_freesync_build_vrr_infopacket(struct mod_freesync *mod_freesync,
		const struct dc_stream_state *stream,
		const struct mod_vrr_params *vrr,
		struct dc_info_packet *infopacket)
{
	/* SPD info packet for FreeSync */
	unsigned char checksum = 0;
	unsigned int idx, payload_size = 0;

	/* Check if Freesync is supported. Return if false. If true,
	 * set the corresponding bit in the info packet
	 */
	if (!vrr->supported)
		return;

	if (dc_is_hdmi_signal(stream->signal)) {

		/* HEADER */

		/* HB0  = Packet Type = 0x83 (Source Product
		 *	  Descriptor InfoFrame)
		 */
		infopacket->hb0 = DC_HDMI_INFOFRAME_TYPE_SPD;

		/* HB1  = Version = 0x01 */
		infopacket->hb1 = 0x01;

		/* HB2  = [Bits 7:5 = 0] [Bits 4:0 = Length = 0x08] */
		infopacket->hb2 = 0x08;

		payload_size = 0x08;

	} else if (dc_is_dp_signal(stream->signal)) {

		/* HEADER */

		/* HB0  = Secondary-data Packet ID = 0 - Only non-zero
		 *	  when used to associate audio related info packets
		 */
		infopacket->hb0 = 0x00;

		/* HB1  = Packet Type = 0x83 (Source Product
		 *	  Descriptor InfoFrame)
		 */
		infopacket->hb1 = DC_HDMI_INFOFRAME_TYPE_SPD;

		/* HB2  = [Bits 7:0 = Least significant eight bits -
		 *	  For INFOFRAME, the value must be 1Bh]
		 */
		infopacket->hb2 = 0x1B;

		/* HB3  = [Bits 7:2 = INFOFRAME SDP Version Number = 0x1]
		 *	  [Bits 1:0 = Most significant two bits = 0x00]
		 */
		infopacket->hb3 = 0x04;

		payload_size = 0x1B;
	}

	/* PB1 = 0x1A (24bit AMD IEEE OUI (0x00001A) - Byte 0) */
	infopacket->sb[1] = 0x1A;

	/* PB2 = 0x00 (24bit AMD IEEE OUI (0x00001A) - Byte 1) */
	infopacket->sb[2] = 0x00;

	/* PB3 = 0x00 (24bit AMD IEEE OUI (0x00001A) - Byte 2) */
	infopacket->sb[3] = 0x00;

	/* PB4 = Reserved */

	/* PB5 = Reserved */

	/* PB6 = [Bits 7:3 = Reserved] */

	/* PB6 = [Bit 0 = FreeSync Supported] */
	if (vrr->state != VRR_STATE_UNSUPPORTED)
		infopacket->sb[6] |= 0x01;

	/* PB6 = [Bit 1 = FreeSync Enabled] */
	if (vrr->state != VRR_STATE_DISABLED &&
			vrr->state != VRR_STATE_UNSUPPORTED)
		infopacket->sb[6] |= 0x02;

	/* PB6 = [Bit 2 = FreeSync Active] */
	if (vrr->state == VRR_STATE_ACTIVE_VARIABLE ||
			vrr->state == VRR_STATE_ACTIVE_FIXED)
		infopacket->sb[6] |= 0x04;

	/* PB7 = FreeSync Minimum refresh rate (Hz) */
	infopacket->sb[7] = (unsigned char)(vrr->min_refresh_in_uhz / 1000000);

	/* PB8 = FreeSync Maximum refresh rate (Hz)
	 * Note: We should never go above the field rate of the mode timing set.
	 */
	infopacket->sb[8] = (unsigned char)(vrr->max_refresh_in_uhz / 1000000);

	/* PB9 - PB27  = Reserved */

	/* Calculate checksum */
	checksum += infopacket->hb0;
	checksum += infopacket->hb1;
	checksum += infopacket->hb2;
	checksum += infopacket->hb3;

	for (idx = 1; idx <= payload_size; idx++)
		checksum += infopacket->sb[idx];

	/* PB0 = Checksum (one byte complement) */
	infopacket->sb[0] = (unsigned char)(0x100 - checksum);

	infopacket->valid = true;
}

void mod_freesync_build_vrr_params(struct mod_freesync *mod_freesync,
		const struct dc_stream_state *stream,
		struct mod_freesync_config *in_config,
		struct mod_vrr_params *in_out_vrr)
{
	struct core_freesync *core_freesync = NULL;
	unsigned long long nominal_field_rate_in_uhz = 0;
	bool nominal_field_rate_in_range = true;
	unsigned int refresh_range = 0;
	unsigned int min_refresh_in_uhz = 0;
	unsigned int max_refresh_in_uhz = 0;

	if (mod_freesync == NULL)
		return;

	core_freesync = MOD_FREESYNC_TO_CORE(mod_freesync);

	/* Calculate nominal field rate for stream */
	nominal_field_rate_in_uhz = stream->timing.pix_clk_khz;
	nominal_field_rate_in_uhz *= 1000ULL * 1000ULL * 1000ULL;
	nominal_field_rate_in_uhz = div_u64(nominal_field_rate_in_uhz,
						stream->timing.h_total);
	nominal_field_rate_in_uhz = div_u64(nominal_field_rate_in_uhz,
						stream->timing.v_total);

	min_refresh_in_uhz = in_config->min_refresh_in_uhz;
	max_refresh_in_uhz = in_config->max_refresh_in_uhz;

	// Don't allow min > max
	if (min_refresh_in_uhz > max_refresh_in_uhz)
		min_refresh_in_uhz = max_refresh_in_uhz;

	// Full range may be larger than current video timing, so cap at nominal
	if (max_refresh_in_uhz > nominal_field_rate_in_uhz)
		max_refresh_in_uhz = nominal_field_rate_in_uhz;

	/* Allow for some rounding error of actual video timing by taking ceil.
	 * For example, 144 Hz mode timing may actually be 143.xxx Hz when
	 * calculated from pixel rate and vertical/horizontal totals, but
	 * this should be allowed instead of blocking FreeSync.
	 */
	if ((min_refresh_in_uhz / 1000000) >
			((nominal_field_rate_in_uhz + 1000000 - 1) / 1000000))
		nominal_field_rate_in_range = false;

	// Full range may be larger than current video timing, so cap at nominal
	if (min_refresh_in_uhz > nominal_field_rate_in_uhz)
		min_refresh_in_uhz = nominal_field_rate_in_uhz;

	if (!vrr_settings_require_update(core_freesync,
			in_config, min_refresh_in_uhz, max_refresh_in_uhz,
			in_out_vrr))
		return;

	in_out_vrr->state = in_config->state;

	if ((in_config->state == VRR_STATE_UNSUPPORTED) ||
				(!nominal_field_rate_in_range)) {
		in_out_vrr->state = VRR_STATE_UNSUPPORTED;
		in_out_vrr->supported = false;
	} else {
		in_out_vrr->min_refresh_in_uhz = min_refresh_in_uhz;
		in_out_vrr->max_duration_in_us =
				calc_duration_in_us_from_refresh_in_uhz(
						min_refresh_in_uhz);

		in_out_vrr->max_refresh_in_uhz = max_refresh_in_uhz;
		in_out_vrr->min_duration_in_us =
				calc_duration_in_us_from_refresh_in_uhz(
						max_refresh_in_uhz);

		refresh_range = in_out_vrr->max_refresh_in_uhz -
				in_out_vrr->min_refresh_in_uhz;

		in_out_vrr->supported = true;
	}

	in_out_vrr->fixed.ramping_active = in_config->ramping;

	in_out_vrr->btr.btr_enabled = in_config->btr;
	if (in_out_vrr->max_refresh_in_uhz <
			2 * in_out_vrr->min_refresh_in_uhz)
		in_out_vrr->btr.btr_enabled = false;
	in_out_vrr->btr.btr_active = false;
	in_out_vrr->btr.inserted_duration_in_us = 0;
	in_out_vrr->btr.frames_to_insert = 0;
	in_out_vrr->btr.frame_counter = 0;
	in_out_vrr->btr.mid_point_in_us =
			in_out_vrr->min_duration_in_us +
				(in_out_vrr->max_duration_in_us -
				in_out_vrr->min_duration_in_us) / 2;

	if (in_out_vrr->state == VRR_STATE_UNSUPPORTED) {
		in_out_vrr->adjust.v_total_min = stream->timing.v_total;
		in_out_vrr->adjust.v_total_max = stream->timing.v_total;
	} else if (in_out_vrr->state == VRR_STATE_DISABLED) {
		in_out_vrr->adjust.v_total_min = stream->timing.v_total;
		in_out_vrr->adjust.v_total_max = stream->timing.v_total;
	} else if (in_out_vrr->state == VRR_STATE_INACTIVE) {
		in_out_vrr->adjust.v_total_min = stream->timing.v_total;
		in_out_vrr->adjust.v_total_max = stream->timing.v_total;
	} else if (in_out_vrr->state == VRR_STATE_ACTIVE_VARIABLE &&
			refresh_range >= MIN_REFRESH_RANGE_IN_US) {
		in_out_vrr->adjust.v_total_min =
			calc_v_total_from_refresh(stream,
				in_out_vrr->max_refresh_in_uhz);
		in_out_vrr->adjust.v_total_max =
			calc_v_total_from_refresh(stream,
				in_out_vrr->min_refresh_in_uhz);
	} else if (in_out_vrr->state == VRR_STATE_ACTIVE_FIXED) {
		in_out_vrr->fixed.target_refresh_in_uhz =
				in_out_vrr->min_refresh_in_uhz;
		if (in_out_vrr->fixed.ramping_active) {
			in_out_vrr->fixed.fixed_active = true;
		} else {
			in_out_vrr->fixed.fixed_active = true;
			in_out_vrr->adjust.v_total_min =
				calc_v_total_from_refresh(stream,
					in_out_vrr->fixed.target_refresh_in_uhz);
			in_out_vrr->adjust.v_total_max =
				in_out_vrr->adjust.v_total_min;
		}
	} else {
		in_out_vrr->state = VRR_STATE_INACTIVE;
		in_out_vrr->adjust.v_total_min = stream->timing.v_total;
		in_out_vrr->adjust.v_total_max = stream->timing.v_total;
	}
}

void mod_freesync_handle_preflip(struct mod_freesync *mod_freesync,
		const struct dc_plane_state *plane,
		const struct dc_stream_state *stream,
		unsigned int curr_time_stamp_in_us,
		struct mod_vrr_params *in_out_vrr)
{
	struct core_freesync *core_freesync = NULL;
	unsigned int last_render_time_in_us = 0;
	unsigned int average_render_time_in_us = 0;

	if (mod_freesync == NULL)
		return;

	core_freesync = MOD_FREESYNC_TO_CORE(mod_freesync);

	if (in_out_vrr->supported &&
			in_out_vrr->state == VRR_STATE_ACTIVE_VARIABLE) {
		unsigned int i = 0;
		unsigned int oldest_index = plane->time.index + 1;

		if (oldest_index >= DC_PLANE_UPDATE_TIMES_MAX)
			oldest_index = 0;

		last_render_time_in_us = curr_time_stamp_in_us -
				plane->time.prev_update_time_in_us;

		// Sum off all entries except oldest one
		for (i = 0; i < DC_PLANE_UPDATE_TIMES_MAX; i++) {
			average_render_time_in_us +=
					plane->time.time_elapsed_in_us[i];
		}
		average_render_time_in_us -=
				plane->time.time_elapsed_in_us[oldest_index];

		// Add render time for current flip
		average_render_time_in_us += last_render_time_in_us;
		average_render_time_in_us /= DC_PLANE_UPDATE_TIMES_MAX;

		if (in_out_vrr->btr.btr_enabled) {
			apply_below_the_range(core_freesync,
					stream,
					last_render_time_in_us,
					in_out_vrr);
		} else {
			apply_fixed_refresh(core_freesync,
				stream,
				last_render_time_in_us,
				in_out_vrr);
		}

	}
}

void mod_freesync_handle_v_update(struct mod_freesync *mod_freesync,
		const struct dc_stream_state *stream,
		struct mod_vrr_params *in_out_vrr)
{
	struct core_freesync *core_freesync = NULL;

	if ((mod_freesync == NULL) || (stream == NULL) || (in_out_vrr == NULL))
		return;

	core_freesync = MOD_FREESYNC_TO_CORE(mod_freesync);

	if (in_out_vrr->supported == false)
		return;

	/* Below the Range Logic */

	/* Only execute if in fullscreen mode */
	if (in_out_vrr->state == VRR_STATE_ACTIVE_VARIABLE &&
					in_out_vrr->btr.btr_active) {
		/* TODO: pass in flag for Pre-DCE12 ASIC
		 * in order for frame variable duration to take affect,
		 * it needs to be done one VSYNC early, which is at
		 * frameCounter == 1.
		 * For DCE12 and newer updates to V_TOTAL_MIN/MAX
		 * will take affect on current frame
		 */
		if (in_out_vrr->btr.frames_to_insert ==
				in_out_vrr->btr.frame_counter) {
			in_out_vrr->adjust.v_total_min =
				calc_v_total_from_duration(stream,
				in_out_vrr,
				in_out_vrr->btr.inserted_duration_in_us);
			in_out_vrr->adjust.v_total_max =
				in_out_vrr->adjust.v_total_min;
		}

		if (in_out_vrr->btr.frame_counter > 0)
			in_out_vrr->btr.frame_counter--;

		/* Restore FreeSync */
		if (in_out_vrr->btr.frame_counter == 0) {
			in_out_vrr->adjust.v_total_min =
				calc_v_total_from_refresh(stream,
				in_out_vrr->max_refresh_in_uhz);
			in_out_vrr->adjust.v_total_max =
				calc_v_total_from_refresh(stream,
				in_out_vrr->min_refresh_in_uhz);
		}
	}

	/* If in fullscreen freesync mode or in video, do not program
	 * static screen ramp values
	 */
	if (in_out_vrr->state == VRR_STATE_ACTIVE_VARIABLE)
		in_out_vrr->fixed.ramping_active = false;

	/* Gradual Static Screen Ramping Logic */
	/* Execute if ramp is active and user enabled freesync static screen*/
	if (in_out_vrr->state == VRR_STATE_ACTIVE_FIXED &&
				in_out_vrr->fixed.ramping_active) {
		update_v_total_for_static_ramp(
				core_freesync, stream, in_out_vrr);
	}
}

void mod_freesync_get_settings(struct mod_freesync *mod_freesync,
		const struct mod_vrr_params *vrr,
		unsigned int *v_total_min, unsigned int *v_total_max,
		unsigned int *event_triggers,
		unsigned int *window_min, unsigned int *window_max,
		unsigned int *lfc_mid_point_in_us,
		unsigned int *inserted_frames,
		unsigned int *inserted_duration_in_us)
{
	struct core_freesync *core_freesync = NULL;

	if (mod_freesync == NULL)
		return;

	core_freesync = MOD_FREESYNC_TO_CORE(mod_freesync);

	if (vrr->supported) {
		*v_total_min = vrr->adjust.v_total_min;
		*v_total_max = vrr->adjust.v_total_max;
		*event_triggers = 0;
		*lfc_mid_point_in_us = vrr->btr.mid_point_in_us;
		*inserted_frames = vrr->btr.frames_to_insert;
		*inserted_duration_in_us = vrr->btr.inserted_duration_in_us;
	}
}
