/*
 * Copyright 2015 Advanced Micro Devices, Inc.
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

#ifndef __DAL_HW_SHARED_H__
#define __DAL_HW_SHARED_H__

#include "os_types.h"
#include "fixed31_32.h"
#include "dc_hw_types.h"

/******************************************************************************
 * Data types shared between different Virtual HW blocks
 ******************************************************************************/

#define MAX_PIPES 6

struct gamma_curve {
	uint32_t offset;
	uint32_t segments_num;
};

struct curve_points {
	struct fixed31_32 x;
	struct fixed31_32 y;
	struct fixed31_32 offset;
	struct fixed31_32 slope;

	uint32_t custom_float_x;
	uint32_t custom_float_y;
	uint32_t custom_float_offset;
	uint32_t custom_float_slope;
};

struct pwl_result_data {
	struct fixed31_32 red;
	struct fixed31_32 green;
	struct fixed31_32 blue;

	struct fixed31_32 delta_red;
	struct fixed31_32 delta_green;
	struct fixed31_32 delta_blue;

	uint32_t red_reg;
	uint32_t green_reg;
	uint32_t blue_reg;

	uint32_t delta_red_reg;
	uint32_t delta_green_reg;
	uint32_t delta_blue_reg;
};

struct pwl_params {
	struct gamma_curve arr_curve_points[34];
	struct curve_points arr_points[2];
	struct pwl_result_data rgb_resulted[256 + 3];
	uint32_t hw_points_num;
};

/* move to dpp
 * while we are moving functionality out of opp to dpp to align
 * HW programming to HW IP, we define these struct in hw_shared
 * so we can still compile while refactoring
 */

enum lb_pixel_depth {
	/* do not change the values because it is used as bit vector */
	LB_PIXEL_DEPTH_18BPP = 1,
	LB_PIXEL_DEPTH_24BPP = 2,
	LB_PIXEL_DEPTH_30BPP = 4,
	LB_PIXEL_DEPTH_36BPP = 8
};

enum graphics_csc_adjust_type {
	GRAPHICS_CSC_ADJUST_TYPE_BYPASS = 0,
	GRAPHICS_CSC_ADJUST_TYPE_HW, /* without adjustments */
	GRAPHICS_CSC_ADJUST_TYPE_SW  /*use adjustments */
};

enum ipp_degamma_mode {
	IPP_DEGAMMA_MODE_BYPASS,
	IPP_DEGAMMA_MODE_HW_sRGB,
	IPP_DEGAMMA_MODE_HW_xvYCC,
	IPP_DEGAMMA_MODE_USER_PWL
};

enum ipp_output_format {
	IPP_OUTPUT_FORMAT_12_BIT_FIX,
	IPP_OUTPUT_FORMAT_16_BIT_BYPASS,
	IPP_OUTPUT_FORMAT_FLOAT
};

enum expansion_mode {
	EXPANSION_MODE_DYNAMIC,
	EXPANSION_MODE_ZERO
};

struct default_adjustment {
	enum lb_pixel_depth lb_color_depth;
	enum dc_color_space out_color_space;
	enum dc_color_space in_color_space;
	enum dc_color_depth color_depth;
	enum pixel_format surface_pixel_format;
	enum graphics_csc_adjust_type csc_adjust_type;
	bool force_hw_default;
};


struct out_csc_color_matrix {
	enum dc_color_space color_space;
	uint16_t regval[12];
};


enum opp_regamma {
	OPP_REGAMMA_BYPASS = 0,
	OPP_REGAMMA_SRGB,
	OPP_REGAMMA_XVYCC,
	OPP_REGAMMA_USER
};

struct dc_bias_and_scale {
	uint16_t scale_red;
	uint16_t bias_red;
	uint16_t scale_green;
	uint16_t bias_green;
	uint16_t scale_blue;
	uint16_t bias_blue;
};

enum test_pattern_dyn_range {
	TEST_PATTERN_DYN_RANGE_VESA = 0,
	TEST_PATTERN_DYN_RANGE_CEA
};

enum test_pattern_mode {
	TEST_PATTERN_MODE_COLORSQUARES_RGB = 0,
	TEST_PATTERN_MODE_COLORSQUARES_YCBCR601,
	TEST_PATTERN_MODE_COLORSQUARES_YCBCR709,
	TEST_PATTERN_MODE_VERTICALBARS,
	TEST_PATTERN_MODE_HORIZONTALBARS,
	TEST_PATTERN_MODE_SINGLERAMP_RGB,
	TEST_PATTERN_MODE_DUALRAMP_RGB
};

enum test_pattern_color_format {
	TEST_PATTERN_COLOR_FORMAT_BPC_6 = 0,
	TEST_PATTERN_COLOR_FORMAT_BPC_8,
	TEST_PATTERN_COLOR_FORMAT_BPC_10,
	TEST_PATTERN_COLOR_FORMAT_BPC_12
};

enum controller_dp_test_pattern {
	CONTROLLER_DP_TEST_PATTERN_D102 = 0,
	CONTROLLER_DP_TEST_PATTERN_SYMBOLERROR,
	CONTROLLER_DP_TEST_PATTERN_PRBS7,
	CONTROLLER_DP_TEST_PATTERN_COLORSQUARES,
	CONTROLLER_DP_TEST_PATTERN_VERTICALBARS,
	CONTROLLER_DP_TEST_PATTERN_HORIZONTALBARS,
	CONTROLLER_DP_TEST_PATTERN_COLORRAMP,
	CONTROLLER_DP_TEST_PATTERN_VIDEOMODE,
	CONTROLLER_DP_TEST_PATTERN_RESERVED_8,
	CONTROLLER_DP_TEST_PATTERN_RESERVED_9,
	CONTROLLER_DP_TEST_PATTERN_RESERVED_A,
	CONTROLLER_DP_TEST_PATTERN_COLORSQUARES_CEA
};

enum dp_pixel_encoding_type {
	DP_PIXEL_ENCODING_TYPE_RGB444		= 0x00000000,
	DP_PIXEL_ENCODING_TYPE_YCBCR422		= 0x00000001,
	DP_PIXEL_ENCODING_TYPE_YCBCR444		= 0x00000002,
	DP_PIXEL_ENCODING_TYPE_RGB_WIDE_GAMUT	= 0x00000003,
	DP_PIXEL_ENCODING_TYPE_Y_ONLY		= 0x00000004,
	DP_PIXEL_ENCODING_TYPE_YCBCR420		= 0x00000005
};

enum dp_component_depth {
	DP_COMPONENT_PIXEL_DEPTH_6BPC		= 0x00000000,
	DP_COMPONENT_PIXEL_DEPTH_8BPC		= 0x00000001,
	DP_COMPONENT_PIXEL_DEPTH_10BPC		= 0x00000002,
	DP_COMPONENT_PIXEL_DEPTH_12BPC		= 0x00000003,
	DP_COMPONENT_PIXEL_DEPTH_16BPC		= 0x00000004
};

enum dc_lut_mode {
	LUT_BYPASS,
	LUT_RAM_A,
	LUT_RAM_B
};
#endif /* __DAL_HW_SHARED_H__ */
