/*
 * Copyright © 2006-2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "intel_drv.h"

/**
 * DOC: CDCLK / RAWCLK
 *
 * The display engine uses several different clocks to do its work. There
 * are two main clocks involved that aren't directly related to the actual
 * pixel clock or any symbol/bit clock of the actual output port. These
 * are the core display clock (CDCLK) and RAWCLK.
 *
 * CDCLK clocks most of the display pipe logic, and thus its frequency
 * must be high enough to support the rate at which pixels are flowing
 * through the pipes. Downscaling must also be accounted as that increases
 * the effective pixel rate.
 *
 * On several platforms the CDCLK frequency can be changed dynamically
 * to minimize power consumption for a given display configuration.
 * Typically changes to the CDCLK frequency require all the display pipes
 * to be shut down while the frequency is being changed.
 *
 * On SKL+ the DMC will toggle the CDCLK off/on during DC5/6 entry/exit.
 * DMC will not change the active CDCLK frequency however, so that part
 * will still be performed by the driver directly.
 *
 * RAWCLK is a fixed frequency clock, often used by various auxiliary
 * blocks such as AUX CH or backlight PWM. Hence the only thing we
 * really need to know about RAWCLK is its frequency so that various
 * dividers can be programmed correctly.
 */

static void fixed_133mhz_get_cdclk(struct drm_i915_private *dev_priv,
				   struct intel_cdclk_state *cdclk_state)
{
	cdclk_state->cdclk = 133333;
}

static void fixed_200mhz_get_cdclk(struct drm_i915_private *dev_priv,
				   struct intel_cdclk_state *cdclk_state)
{
	cdclk_state->cdclk = 200000;
}

static void fixed_266mhz_get_cdclk(struct drm_i915_private *dev_priv,
				   struct intel_cdclk_state *cdclk_state)
{
	cdclk_state->cdclk = 266667;
}

static void fixed_333mhz_get_cdclk(struct drm_i915_private *dev_priv,
				   struct intel_cdclk_state *cdclk_state)
{
	cdclk_state->cdclk = 333333;
}

static void fixed_400mhz_get_cdclk(struct drm_i915_private *dev_priv,
				   struct intel_cdclk_state *cdclk_state)
{
	cdclk_state->cdclk = 400000;
}

static void fixed_450mhz_get_cdclk(struct drm_i915_private *dev_priv,
				   struct intel_cdclk_state *cdclk_state)
{
	cdclk_state->cdclk = 450000;
}

static void i85x_get_cdclk(struct drm_i915_private *dev_priv,
			   struct intel_cdclk_state *cdclk_state)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	u16 hpllcc = 0;

	/*
	 * 852GM/852GMV only supports 133 MHz and the HPLLCC
	 * encoding is different :(
	 * FIXME is this the right way to detect 852GM/852GMV?
	 */
	if (pdev->revision == 0x1) {
		cdclk_state->cdclk = 133333;
		return;
	}

	pci_bus_read_config_word(pdev->bus,
				 PCI_DEVFN(0, 3), HPLLCC, &hpllcc);

	/* Assume that the hardware is in the high speed state.  This
	 * should be the default.
	 */
	switch (hpllcc & GC_CLOCK_CONTROL_MASK) {
	case GC_CLOCK_133_200:
	case GC_CLOCK_133_200_2:
	case GC_CLOCK_100_200:
		cdclk_state->cdclk = 200000;
		break;
	case GC_CLOCK_166_250:
		cdclk_state->cdclk = 250000;
		break;
	case GC_CLOCK_100_133:
		cdclk_state->cdclk = 133333;
		break;
	case GC_CLOCK_133_266:
	case GC_CLOCK_133_266_2:
	case GC_CLOCK_166_266:
		cdclk_state->cdclk = 266667;
		break;
	}
}

static void i915gm_get_cdclk(struct drm_i915_private *dev_priv,
			     struct intel_cdclk_state *cdclk_state)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	u16 gcfgc = 0;

	pci_read_config_word(pdev, GCFGC, &gcfgc);

	if (gcfgc & GC_LOW_FREQUENCY_ENABLE) {
		cdclk_state->cdclk = 133333;
		return;
	}

	switch (gcfgc & GC_DISPLAY_CLOCK_MASK) {
	case GC_DISPLAY_CLOCK_333_320_MHZ:
		cdclk_state->cdclk = 333333;
		break;
	default:
	case GC_DISPLAY_CLOCK_190_200_MHZ:
		cdclk_state->cdclk = 190000;
		break;
	}
}

static void i945gm_get_cdclk(struct drm_i915_private *dev_priv,
			     struct intel_cdclk_state *cdclk_state)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	u16 gcfgc = 0;

	pci_read_config_word(pdev, GCFGC, &gcfgc);

	if (gcfgc & GC_LOW_FREQUENCY_ENABLE) {
		cdclk_state->cdclk = 133333;
		return;
	}

	switch (gcfgc & GC_DISPLAY_CLOCK_MASK) {
	case GC_DISPLAY_CLOCK_333_320_MHZ:
		cdclk_state->cdclk = 320000;
		break;
	default:
	case GC_DISPLAY_CLOCK_190_200_MHZ:
		cdclk_state->cdclk = 200000;
		break;
	}
}

static unsigned int intel_hpll_vco(struct drm_i915_private *dev_priv)
{
	static const unsigned int blb_vco[8] = {
		[0] = 3200000,
		[1] = 4000000,
		[2] = 5333333,
		[3] = 4800000,
		[4] = 6400000,
	};
	static const unsigned int pnv_vco[8] = {
		[0] = 3200000,
		[1] = 4000000,
		[2] = 5333333,
		[3] = 4800000,
		[4] = 2666667,
	};
	static const unsigned int cl_vco[8] = {
		[0] = 3200000,
		[1] = 4000000,
		[2] = 5333333,
		[3] = 6400000,
		[4] = 3333333,
		[5] = 3566667,
		[6] = 4266667,
	};
	static const unsigned int elk_vco[8] = {
		[0] = 3200000,
		[1] = 4000000,
		[2] = 5333333,
		[3] = 4800000,
	};
	static const unsigned int ctg_vco[8] = {
		[0] = 3200000,
		[1] = 4000000,
		[2] = 5333333,
		[3] = 6400000,
		[4] = 2666667,
		[5] = 4266667,
	};
	const unsigned int *vco_table;
	unsigned int vco;
	uint8_t tmp = 0;

	/* FIXME other chipsets? */
	if (IS_GM45(dev_priv))
		vco_table = ctg_vco;
	else if (IS_G45(dev_priv))
		vco_table = elk_vco;
	else if (IS_I965GM(dev_priv))
		vco_table = cl_vco;
	else if (IS_PINEVIEW(dev_priv))
		vco_table = pnv_vco;
	else if (IS_G33(dev_priv))
		vco_table = blb_vco;
	else
		return 0;

	tmp = I915_READ(IS_MOBILE(dev_priv) ? HPLLVCO_MOBILE : HPLLVCO);

	vco = vco_table[tmp & 0x7];
	if (vco == 0)
		DRM_ERROR("Bad HPLL VCO (HPLLVCO=0x%02x)\n", tmp);
	else
		DRM_DEBUG_KMS("HPLL VCO %u kHz\n", vco);

	return vco;
}

static void g33_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_state *cdclk_state)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	static const uint8_t div_3200[] = { 12, 10,  8,  7, 5, 16 };
	static const uint8_t div_4000[] = { 14, 12, 10,  8, 6, 20 };
	static const uint8_t div_4800[] = { 20, 14, 12, 10, 8, 24 };
	static const uint8_t div_5333[] = { 20, 16, 12, 12, 8, 28 };
	const uint8_t *div_table;
	unsigned int cdclk_sel;
	uint16_t tmp = 0;

	cdclk_state->vco = intel_hpll_vco(dev_priv);

	pci_read_config_word(pdev, GCFGC, &tmp);

	cdclk_sel = (tmp >> 4) & 0x7;

	if (cdclk_sel >= ARRAY_SIZE(div_3200))
		goto fail;

	switch (cdclk_state->vco) {
	case 3200000:
		div_table = div_3200;
		break;
	case 4000000:
		div_table = div_4000;
		break;
	case 4800000:
		div_table = div_4800;
		break;
	case 5333333:
		div_table = div_5333;
		break;
	default:
		goto fail;
	}

	cdclk_state->cdclk = DIV_ROUND_CLOSEST(cdclk_state->vco,
					       div_table[cdclk_sel]);
	return;

fail:
	DRM_ERROR("Unable to determine CDCLK. HPLL VCO=%u kHz, CFGC=0x%08x\n",
		  cdclk_state->vco, tmp);
	cdclk_state->cdclk = 190476;
}

static void pnv_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_state *cdclk_state)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	u16 gcfgc = 0;

	pci_read_config_word(pdev, GCFGC, &gcfgc);

	switch (gcfgc & GC_DISPLAY_CLOCK_MASK) {
	case GC_DISPLAY_CLOCK_267_MHZ_PNV:
		cdclk_state->cdclk = 266667;
		break;
	case GC_DISPLAY_CLOCK_333_MHZ_PNV:
		cdclk_state->cdclk = 333333;
		break;
	case GC_DISPLAY_CLOCK_444_MHZ_PNV:
		cdclk_state->cdclk = 444444;
		break;
	case GC_DISPLAY_CLOCK_200_MHZ_PNV:
		cdclk_state->cdclk = 200000;
		break;
	default:
		DRM_ERROR("Unknown pnv display core clock 0x%04x\n", gcfgc);
	case GC_DISPLAY_CLOCK_133_MHZ_PNV:
		cdclk_state->cdclk = 133333;
		break;
	case GC_DISPLAY_CLOCK_167_MHZ_PNV:
		cdclk_state->cdclk = 166667;
		break;
	}
}

static void i965gm_get_cdclk(struct drm_i915_private *dev_priv,
			     struct intel_cdclk_state *cdclk_state)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	static const uint8_t div_3200[] = { 16, 10,  8 };
	static const uint8_t div_4000[] = { 20, 12, 10 };
	static const uint8_t div_5333[] = { 24, 16, 14 };
	const uint8_t *div_table;
	unsigned int cdclk_sel;
	uint16_t tmp = 0;

	cdclk_state->vco = intel_hpll_vco(dev_priv);

	pci_read_config_word(pdev, GCFGC, &tmp);

	cdclk_sel = ((tmp >> 8) & 0x1f) - 1;

	if (cdclk_sel >= ARRAY_SIZE(div_3200))
		goto fail;

	switch (cdclk_state->vco) {
	case 3200000:
		div_table = div_3200;
		break;
	case 4000000:
		div_table = div_4000;
		break;
	case 5333333:
		div_table = div_5333;
		break;
	default:
		goto fail;
	}

	cdclk_state->cdclk = DIV_ROUND_CLOSEST(cdclk_state->vco,
					       div_table[cdclk_sel]);
	return;

fail:
	DRM_ERROR("Unable to determine CDCLK. HPLL VCO=%u kHz, CFGC=0x%04x\n",
		  cdclk_state->vco, tmp);
	cdclk_state->cdclk = 200000;
}

static void gm45_get_cdclk(struct drm_i915_private *dev_priv,
			   struct intel_cdclk_state *cdclk_state)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	unsigned int cdclk_sel;
	uint16_t tmp = 0;

	cdclk_state->vco = intel_hpll_vco(dev_priv);

	pci_read_config_word(pdev, GCFGC, &tmp);

	cdclk_sel = (tmp >> 12) & 0x1;

	switch (cdclk_state->vco) {
	case 2666667:
	case 4000000:
	case 5333333:
		cdclk_state->cdclk = cdclk_sel ? 333333 : 222222;
		break;
	case 3200000:
		cdclk_state->cdclk = cdclk_sel ? 320000 : 228571;
		break;
	default:
		DRM_ERROR("Unable to determine CDCLK. HPLL VCO=%u, CFGC=0x%04x\n",
			  cdclk_state->vco, tmp);
		cdclk_state->cdclk = 222222;
		break;
	}
}

static void hsw_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_state *cdclk_state)
{
	uint32_t lcpll = I915_READ(LCPLL_CTL);
	uint32_t freq = lcpll & LCPLL_CLK_FREQ_MASK;

	if (lcpll & LCPLL_CD_SOURCE_FCLK)
		cdclk_state->cdclk = 800000;
	else if (I915_READ(FUSE_STRAP) & HSW_CDCLK_LIMIT)
		cdclk_state->cdclk = 450000;
	else if (freq == LCPLL_CLK_FREQ_450)
		cdclk_state->cdclk = 450000;
	else if (IS_HSW_ULT(dev_priv))
		cdclk_state->cdclk = 337500;
	else
		cdclk_state->cdclk = 540000;
}

static int vlv_calc_cdclk(struct drm_i915_private *dev_priv,
			  int max_pixclk)
{
	int freq_320 = (dev_priv->hpll_freq <<  1) % 320000 != 0 ?
		333333 : 320000;
	int limit = IS_CHERRYVIEW(dev_priv) ? 95 : 90;

	/*
	 * We seem to get an unstable or solid color picture at 200MHz.
	 * Not sure what's wrong. For now use 200MHz only when all pipes
	 * are off.
	 */
	if (!IS_CHERRYVIEW(dev_priv) &&
	    max_pixclk > freq_320*limit/100)
		return 400000;
	else if (max_pixclk > 266667*limit/100)
		return freq_320;
	else if (max_pixclk > 0)
		return 266667;
	else
		return 200000;
}

static void vlv_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_state *cdclk_state)
{
	cdclk_state->vco = vlv_get_hpll_vco(dev_priv);
	cdclk_state->cdclk = vlv_get_cck_clock(dev_priv, "cdclk",
					       CCK_DISPLAY_CLOCK_CONTROL,
					       cdclk_state->vco);
}

static void vlv_program_pfi_credits(struct drm_i915_private *dev_priv)
{
	unsigned int credits, default_credits;

	if (IS_CHERRYVIEW(dev_priv))
		default_credits = PFI_CREDIT(12);
	else
		default_credits = PFI_CREDIT(8);

	if (dev_priv->cdclk.hw.cdclk >= dev_priv->czclk_freq) {
		/* CHV suggested value is 31 or 63 */
		if (IS_CHERRYVIEW(dev_priv))
			credits = PFI_CREDIT_63;
		else
			credits = PFI_CREDIT(15);
	} else {
		credits = default_credits;
	}

	/*
	 * WA - write default credits before re-programming
	 * FIXME: should we also set the resend bit here?
	 */
	I915_WRITE(GCI_CONTROL, VGA_FAST_MODE_DISABLE |
		   default_credits);

	I915_WRITE(GCI_CONTROL, VGA_FAST_MODE_DISABLE |
		   credits | PFI_CREDIT_RESEND);

	/*
	 * FIXME is this guaranteed to clear
	 * immediately or should we poll for it?
	 */
	WARN_ON(I915_READ(GCI_CONTROL) & PFI_CREDIT_RESEND);
}

static void vlv_set_cdclk(struct drm_i915_private *dev_priv,
			  const struct intel_cdclk_state *cdclk_state)
{
	int cdclk = cdclk_state->cdclk;
	u32 val, cmd;

	/* There are cases where we can end up here with power domains
	 * off and a CDCLK frequency other than the minimum, like when
	 * issuing a modeset without actually changing any display after
	 * a system suspend.  So grab the PIPE-A domain, which covers
	 * the HW blocks needed for the following programming.
	 */
	intel_display_power_get(dev_priv, POWER_DOMAIN_PIPE_A);

	if (cdclk >= 320000) /* jump to highest voltage for 400MHz too */
		cmd = 2;
	else if (cdclk == 266667)
		cmd = 1;
	else
		cmd = 0;

	mutex_lock(&dev_priv->rps.hw_lock);
	val = vlv_punit_read(dev_priv, PUNIT_REG_DSPFREQ);
	val &= ~DSPFREQGUAR_MASK;
	val |= (cmd << DSPFREQGUAR_SHIFT);
	vlv_punit_write(dev_priv, PUNIT_REG_DSPFREQ, val);
	if (wait_for((vlv_punit_read(dev_priv, PUNIT_REG_DSPFREQ) &
		      DSPFREQSTAT_MASK) == (cmd << DSPFREQSTAT_SHIFT),
		     50)) {
		DRM_ERROR("timed out waiting for CDclk change\n");
	}
	mutex_unlock(&dev_priv->rps.hw_lock);

	mutex_lock(&dev_priv->sb_lock);

	if (cdclk == 400000) {
		u32 divider;

		divider = DIV_ROUND_CLOSEST(dev_priv->hpll_freq << 1,
					    cdclk) - 1;

		/* adjust cdclk divider */
		val = vlv_cck_read(dev_priv, CCK_DISPLAY_CLOCK_CONTROL);
		val &= ~CCK_FREQUENCY_VALUES;
		val |= divider;
		vlv_cck_write(dev_priv, CCK_DISPLAY_CLOCK_CONTROL, val);

		if (wait_for((vlv_cck_read(dev_priv, CCK_DISPLAY_CLOCK_CONTROL) &
			      CCK_FREQUENCY_STATUS) == (divider << CCK_FREQUENCY_STATUS_SHIFT),
			     50))
			DRM_ERROR("timed out waiting for CDclk change\n");
	}

	/* adjust self-refresh exit latency value */
	val = vlv_bunit_read(dev_priv, BUNIT_REG_BISOC);
	val &= ~0x7f;

	/*
	 * For high bandwidth configs, we set a higher latency in the bunit
	 * so that the core display fetch happens in time to avoid underruns.
	 */
	if (cdclk == 400000)
		val |= 4500 / 250; /* 4.5 usec */
	else
		val |= 3000 / 250; /* 3.0 usec */
	vlv_bunit_write(dev_priv, BUNIT_REG_BISOC, val);

	mutex_unlock(&dev_priv->sb_lock);

	intel_update_cdclk(dev_priv);

	vlv_program_pfi_credits(dev_priv);

	intel_display_power_put(dev_priv, POWER_DOMAIN_PIPE_A);
}

static void chv_set_cdclk(struct drm_i915_private *dev_priv,
			  const struct intel_cdclk_state *cdclk_state)
{
	int cdclk = cdclk_state->cdclk;
	u32 val, cmd;

	switch (cdclk) {
	case 333333:
	case 320000:
	case 266667:
	case 200000:
		break;
	default:
		MISSING_CASE(cdclk);
		return;
	}

	/* There are cases where we can end up here with power domains
	 * off and a CDCLK frequency other than the minimum, like when
	 * issuing a modeset without actually changing any display after
	 * a system suspend.  So grab the PIPE-A domain, which covers
	 * the HW blocks needed for the following programming.
	 */
	intel_display_power_get(dev_priv, POWER_DOMAIN_PIPE_A);

	/*
	 * Specs are full of misinformation, but testing on actual
	 * hardware has shown that we just need to write the desired
	 * CCK divider into the Punit register.
	 */
	cmd = DIV_ROUND_CLOSEST(dev_priv->hpll_freq << 1, cdclk) - 1;

	mutex_lock(&dev_priv->rps.hw_lock);
	val = vlv_punit_read(dev_priv, PUNIT_REG_DSPFREQ);
	val &= ~DSPFREQGUAR_MASK_CHV;
	val |= (cmd << DSPFREQGUAR_SHIFT_CHV);
	vlv_punit_write(dev_priv, PUNIT_REG_DSPFREQ, val);
	if (wait_for((vlv_punit_read(dev_priv, PUNIT_REG_DSPFREQ) &
		      DSPFREQSTAT_MASK_CHV) == (cmd << DSPFREQSTAT_SHIFT_CHV),
		     50)) {
		DRM_ERROR("timed out waiting for CDclk change\n");
	}
	mutex_unlock(&dev_priv->rps.hw_lock);

	intel_update_cdclk(dev_priv);

	vlv_program_pfi_credits(dev_priv);

	intel_display_power_put(dev_priv, POWER_DOMAIN_PIPE_A);
}

static int bdw_calc_cdclk(int max_pixclk)
{
	if (max_pixclk > 540000)
		return 675000;
	else if (max_pixclk > 450000)
		return 540000;
	else if (max_pixclk > 337500)
		return 450000;
	else
		return 337500;
}

static void bdw_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_state *cdclk_state)
{
	uint32_t lcpll = I915_READ(LCPLL_CTL);
	uint32_t freq = lcpll & LCPLL_CLK_FREQ_MASK;

	if (lcpll & LCPLL_CD_SOURCE_FCLK)
		cdclk_state->cdclk = 800000;
	else if (I915_READ(FUSE_STRAP) & HSW_CDCLK_LIMIT)
		cdclk_state->cdclk = 450000;
	else if (freq == LCPLL_CLK_FREQ_450)
		cdclk_state->cdclk = 450000;
	else if (freq == LCPLL_CLK_FREQ_54O_BDW)
		cdclk_state->cdclk = 540000;
	else if (freq == LCPLL_CLK_FREQ_337_5_BDW)
		cdclk_state->cdclk = 337500;
	else
		cdclk_state->cdclk = 675000;
}

static void bdw_set_cdclk(struct drm_i915_private *dev_priv,
			  const struct intel_cdclk_state *cdclk_state)
{
	int cdclk = cdclk_state->cdclk;
	uint32_t val, data;
	int ret;

	if (WARN((I915_READ(LCPLL_CTL) &
		  (LCPLL_PLL_DISABLE | LCPLL_PLL_LOCK |
		   LCPLL_CD_CLOCK_DISABLE | LCPLL_ROOT_CD_CLOCK_DISABLE |
		   LCPLL_CD2X_CLOCK_DISABLE | LCPLL_POWER_DOWN_ALLOW |
		   LCPLL_CD_SOURCE_FCLK)) != LCPLL_PLL_LOCK,
		 "trying to change cdclk frequency with cdclk not enabled\n"))
		return;

	mutex_lock(&dev_priv->rps.hw_lock);
	ret = sandybridge_pcode_write(dev_priv,
				      BDW_PCODE_DISPLAY_FREQ_CHANGE_REQ, 0x0);
	mutex_unlock(&dev_priv->rps.hw_lock);
	if (ret) {
		DRM_ERROR("failed to inform pcode about cdclk change\n");
		return;
	}

	val = I915_READ(LCPLL_CTL);
	val |= LCPLL_CD_SOURCE_FCLK;
	I915_WRITE(LCPLL_CTL, val);

	if (wait_for_us(I915_READ(LCPLL_CTL) &
			LCPLL_CD_SOURCE_FCLK_DONE, 1))
		DRM_ERROR("Switching to FCLK failed\n");

	val = I915_READ(LCPLL_CTL);
	val &= ~LCPLL_CLK_FREQ_MASK;

	switch (cdclk) {
	case 450000:
		val |= LCPLL_CLK_FREQ_450;
		data = 0;
		break;
	case 540000:
		val |= LCPLL_CLK_FREQ_54O_BDW;
		data = 1;
		break;
	case 337500:
		val |= LCPLL_CLK_FREQ_337_5_BDW;
		data = 2;
		break;
	case 675000:
		val |= LCPLL_CLK_FREQ_675_BDW;
		data = 3;
		break;
	default:
		WARN(1, "invalid cdclk frequency\n");
		return;
	}

	I915_WRITE(LCPLL_CTL, val);

	val = I915_READ(LCPLL_CTL);
	val &= ~LCPLL_CD_SOURCE_FCLK;
	I915_WRITE(LCPLL_CTL, val);

	if (wait_for_us((I915_READ(LCPLL_CTL) &
			LCPLL_CD_SOURCE_FCLK_DONE) == 0, 1))
		DRM_ERROR("Switching back to LCPLL failed\n");

	mutex_lock(&dev_priv->rps.hw_lock);
	sandybridge_pcode_write(dev_priv, HSW_PCODE_DE_WRITE_FREQ_REQ, data);
	mutex_unlock(&dev_priv->rps.hw_lock);

	I915_WRITE(CDCLK_FREQ, DIV_ROUND_CLOSEST(cdclk, 1000) - 1);

	intel_update_cdclk(dev_priv);

	WARN(cdclk != dev_priv->cdclk.hw.cdclk,
	     "cdclk requested %d kHz but got %d kHz\n",
	     cdclk, dev_priv->cdclk.hw.cdclk);
}

static int skl_calc_cdclk(int max_pixclk, int vco)
{
	if (vco == 8640000) {
		if (max_pixclk > 540000)
			return 617143;
		else if (max_pixclk > 432000)
			return 540000;
		else if (max_pixclk > 308571)
			return 432000;
		else
			return 308571;
	} else {
		if (max_pixclk > 540000)
			return 675000;
		else if (max_pixclk > 450000)
			return 540000;
		else if (max_pixclk > 337500)
			return 450000;
		else
			return 337500;
	}
}

static void skl_dpll0_update(struct drm_i915_private *dev_priv,
			     struct intel_cdclk_state *cdclk_state)
{
	u32 val;

	cdclk_state->ref = 24000;
	cdclk_state->vco = 0;

	val = I915_READ(LCPLL1_CTL);
	if ((val & LCPLL_PLL_ENABLE) == 0)
		return;

	if (WARN_ON((val & LCPLL_PLL_LOCK) == 0))
		return;

	val = I915_READ(DPLL_CTRL1);

	if (WARN_ON((val & (DPLL_CTRL1_HDMI_MODE(SKL_DPLL0) |
			    DPLL_CTRL1_SSC(SKL_DPLL0) |
			    DPLL_CTRL1_OVERRIDE(SKL_DPLL0))) !=
		    DPLL_CTRL1_OVERRIDE(SKL_DPLL0)))
		return;

	switch (val & DPLL_CTRL1_LINK_RATE_MASK(SKL_DPLL0)) {
	case DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_810, SKL_DPLL0):
	case DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_1350, SKL_DPLL0):
	case DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_1620, SKL_DPLL0):
	case DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_2700, SKL_DPLL0):
		cdclk_state->vco = 8100000;
		break;
	case DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_1080, SKL_DPLL0):
	case DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_2160, SKL_DPLL0):
		cdclk_state->vco = 8640000;
		break;
	default:
		MISSING_CASE(val & DPLL_CTRL1_LINK_RATE_MASK(SKL_DPLL0));
		break;
	}
}

static void skl_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_state *cdclk_state)
{
	u32 cdctl;

	skl_dpll0_update(dev_priv, cdclk_state);

	cdclk_state->cdclk = cdclk_state->ref;

	if (cdclk_state->vco == 0)
		return;

	cdctl = I915_READ(CDCLK_CTL);

	if (cdclk_state->vco == 8640000) {
		switch (cdctl & CDCLK_FREQ_SEL_MASK) {
		case CDCLK_FREQ_450_432:
			cdclk_state->cdclk = 432000;
			break;
		case CDCLK_FREQ_337_308:
			cdclk_state->cdclk = 308571;
			break;
		case CDCLK_FREQ_540:
			cdclk_state->cdclk = 540000;
			break;
		case CDCLK_FREQ_675_617:
			cdclk_state->cdclk = 617143;
			break;
		default:
			MISSING_CASE(cdctl & CDCLK_FREQ_SEL_MASK);
			break;
		}
	} else {
		switch (cdctl & CDCLK_FREQ_SEL_MASK) {
		case CDCLK_FREQ_450_432:
			cdclk_state->cdclk = 450000;
			break;
		case CDCLK_FREQ_337_308:
			cdclk_state->cdclk = 337500;
			break;
		case CDCLK_FREQ_540:
			cdclk_state->cdclk = 540000;
			break;
		case CDCLK_FREQ_675_617:
			cdclk_state->cdclk = 675000;
			break;
		default:
			MISSING_CASE(cdctl & CDCLK_FREQ_SEL_MASK);
			break;
		}
	}
}

/* convert from kHz to .1 fixpoint MHz with -1MHz offset */
static int skl_cdclk_decimal(int cdclk)
{
	return DIV_ROUND_CLOSEST(cdclk - 1000, 500);
}

static void skl_set_preferred_cdclk_vco(struct drm_i915_private *dev_priv,
					int vco)
{
	bool changed = dev_priv->skl_preferred_vco_freq != vco;

	dev_priv->skl_preferred_vco_freq = vco;

	if (changed)
		intel_update_max_cdclk(dev_priv);
}

static void skl_dpll0_enable(struct drm_i915_private *dev_priv, int vco)
{
	u32 val;

	WARN_ON(vco != 8100000 && vco != 8640000);

	/*
	 * We always enable DPLL0 with the lowest link rate possible, but still
	 * taking into account the VCO required to operate the eDP panel at the
	 * desired frequency. The usual DP link rates operate with a VCO of
	 * 8100 while the eDP 1.4 alternate link rates need a VCO of 8640.
	 * The modeset code is responsible for the selection of the exact link
	 * rate later on, with the constraint of choosing a frequency that
	 * works with vco.
	 */
	val = I915_READ(DPLL_CTRL1);

	val &= ~(DPLL_CTRL1_HDMI_MODE(SKL_DPLL0) | DPLL_CTRL1_SSC(SKL_DPLL0) |
		 DPLL_CTRL1_LINK_RATE_MASK(SKL_DPLL0));
	val |= DPLL_CTRL1_OVERRIDE(SKL_DPLL0);
	if (vco == 8640000)
		val |= DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_1080,
					    SKL_DPLL0);
	else
		val |= DPLL_CTRL1_LINK_RATE(DPLL_CTRL1_LINK_RATE_810,
					    SKL_DPLL0);

	I915_WRITE(DPLL_CTRL1, val);
	POSTING_READ(DPLL_CTRL1);

	I915_WRITE(LCPLL1_CTL, I915_READ(LCPLL1_CTL) | LCPLL_PLL_ENABLE);

	if (intel_wait_for_register(dev_priv,
				    LCPLL1_CTL, LCPLL_PLL_LOCK, LCPLL_PLL_LOCK,
				    5))
		DRM_ERROR("DPLL0 not locked\n");

	dev_priv->cdclk.hw.vco = vco;

	/* We'll want to keep using the current vco from now on. */
	skl_set_preferred_cdclk_vco(dev_priv, vco);
}

static void skl_dpll0_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(LCPLL1_CTL, I915_READ(LCPLL1_CTL) & ~LCPLL_PLL_ENABLE);
	if (intel_wait_for_register(dev_priv,
				   LCPLL1_CTL, LCPLL_PLL_LOCK, 0,
				   1))
		DRM_ERROR("Couldn't disable DPLL0\n");

	dev_priv->cdclk.hw.vco = 0;
}

static void skl_set_cdclk(struct drm_i915_private *dev_priv,
			  const struct intel_cdclk_state *cdclk_state)
{
	int cdclk = cdclk_state->cdclk;
	int vco = cdclk_state->vco;
	u32 freq_select, pcu_ack, cdclk_ctl;
	int ret;

	WARN_ON((cdclk == 24000) != (vco == 0));

	mutex_lock(&dev_priv->rps.hw_lock);
	ret = skl_pcode_request(dev_priv, SKL_PCODE_CDCLK_CONTROL,
				SKL_CDCLK_PREPARE_FOR_CHANGE,
				SKL_CDCLK_READY_FOR_CHANGE,
				SKL_CDCLK_READY_FOR_CHANGE, 3);
	mutex_unlock(&dev_priv->rps.hw_lock);
	if (ret) {
		DRM_ERROR("Failed to inform PCU about cdclk change (%d)\n",
			  ret);
		return;
	}

	/* Choose frequency for this cdclk */
	switch (cdclk) {
	case 450000:
	case 432000:
		freq_select = CDCLK_FREQ_450_432;
		pcu_ack = 1;
		break;
	case 540000:
		freq_select = CDCLK_FREQ_540;
		pcu_ack = 2;
		break;
	case 308571:
	case 337500:
	default:
		freq_select = CDCLK_FREQ_337_308;
		pcu_ack = 0;
		break;
	case 617143:
	case 675000:
		freq_select = CDCLK_FREQ_675_617;
		pcu_ack = 3;
		break;
	}

	if (dev_priv->cdclk.hw.vco != 0 &&
	    dev_priv->cdclk.hw.vco != vco)
		skl_dpll0_disable(dev_priv);

	cdclk_ctl = I915_READ(CDCLK_CTL);

	if (dev_priv->cdclk.hw.vco != vco) {
		/* Wa Display #1183: skl,kbl,cfl */
		cdclk_ctl &= ~(CDCLK_FREQ_SEL_MASK | CDCLK_FREQ_DECIMAL_MASK);
		cdclk_ctl |= freq_select | skl_cdclk_decimal(cdclk);
		I915_WRITE(CDCLK_CTL, cdclk_ctl);
	}

	/* Wa Display #1183: skl,kbl,cfl */
	cdclk_ctl |= CDCLK_DIVMUX_CD_OVERRIDE;
	I915_WRITE(CDCLK_CTL, cdclk_ctl);
	POSTING_READ(CDCLK_CTL);

	if (dev_priv->cdclk.hw.vco != vco)
		skl_dpll0_enable(dev_priv, vco);

	/* Wa Display #1183: skl,kbl,cfl */
	cdclk_ctl &= ~(CDCLK_FREQ_SEL_MASK | CDCLK_FREQ_DECIMAL_MASK);
	I915_WRITE(CDCLK_CTL, cdclk_ctl);

	cdclk_ctl |= freq_select | skl_cdclk_decimal(cdclk);
	I915_WRITE(CDCLK_CTL, cdclk_ctl);

	/* Wa Display #1183: skl,kbl,cfl */
	cdclk_ctl &= ~CDCLK_DIVMUX_CD_OVERRIDE;
	I915_WRITE(CDCLK_CTL, cdclk_ctl);
	POSTING_READ(CDCLK_CTL);

	/* inform PCU of the change */
	mutex_lock(&dev_priv->rps.hw_lock);
	sandybridge_pcode_write(dev_priv, SKL_PCODE_CDCLK_CONTROL, pcu_ack);
	mutex_unlock(&dev_priv->rps.hw_lock);

	intel_update_cdclk(dev_priv);
}

static void skl_sanitize_cdclk(struct drm_i915_private *dev_priv)
{
	uint32_t cdctl, expected;

	/*
	 * check if the pre-os initialized the display
	 * There is SWF18 scratchpad register defined which is set by the
	 * pre-os which can be used by the OS drivers to check the status
	 */
	if ((I915_READ(SWF_ILK(0x18)) & 0x00FFFFFF) == 0)
		goto sanitize;

	intel_update_cdclk(dev_priv);
	/* Is PLL enabled and locked ? */
	if (dev_priv->cdclk.hw.vco == 0 ||
	    dev_priv->cdclk.hw.cdclk == dev_priv->cdclk.hw.ref)
		goto sanitize;

	/* DPLL okay; verify the cdclock
	 *
	 * Noticed in some instances that the freq selection is correct but
	 * decimal part is programmed wrong from BIOS where pre-os does not
	 * enable display. Verify the same as well.
	 */
	cdctl = I915_READ(CDCLK_CTL);
	expected = (cdctl & CDCLK_FREQ_SEL_MASK) |
		skl_cdclk_decimal(dev_priv->cdclk.hw.cdclk);
	if (cdctl == expected)
		/* All well; nothing to sanitize */
		return;

sanitize:
	DRM_DEBUG_KMS("Sanitizing cdclk programmed by pre-os\n");

	/* force cdclk programming */
	dev_priv->cdclk.hw.cdclk = 0;
	/* force full PLL disable + enable */
	dev_priv->cdclk.hw.vco = -1;
}

/**
 * skl_init_cdclk - Initialize CDCLK on SKL
 * @dev_priv: i915 device
 *
 * Initialize CDCLK for SKL and derivatives. This is generally
 * done only during the display core initialization sequence,
 * after which the DMC will take care of turning CDCLK off/on
 * as needed.
 */
void skl_init_cdclk(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_state cdclk_state;

	skl_sanitize_cdclk(dev_priv);

	if (dev_priv->cdclk.hw.cdclk != 0 &&
	    dev_priv->cdclk.hw.vco != 0) {
		/*
		 * Use the current vco as our initial
		 * guess as to what the preferred vco is.
		 */
		if (dev_priv->skl_preferred_vco_freq == 0)
			skl_set_preferred_cdclk_vco(dev_priv,
						    dev_priv->cdclk.hw.vco);
		return;
	}

	cdclk_state = dev_priv->cdclk.hw;

	cdclk_state.vco = dev_priv->skl_preferred_vco_freq;
	if (cdclk_state.vco == 0)
		cdclk_state.vco = 8100000;
	cdclk_state.cdclk = skl_calc_cdclk(0, cdclk_state.vco);

	skl_set_cdclk(dev_priv, &cdclk_state);
}

/**
 * skl_uninit_cdclk - Uninitialize CDCLK on SKL
 * @dev_priv: i915 device
 *
 * Uninitialize CDCLK for SKL and derivatives. This is done only
 * during the display core uninitialization sequence.
 */
void skl_uninit_cdclk(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_state cdclk_state = dev_priv->cdclk.hw;

	cdclk_state.cdclk = cdclk_state.ref;
	cdclk_state.vco = 0;

	skl_set_cdclk(dev_priv, &cdclk_state);
}

static int bxt_calc_cdclk(int max_pixclk)
{
	if (max_pixclk > 576000)
		return 624000;
	else if (max_pixclk > 384000)
		return 576000;
	else if (max_pixclk > 288000)
		return 384000;
	else if (max_pixclk > 144000)
		return 288000;
	else
		return 144000;
}

static int glk_calc_cdclk(int max_pixclk)
{
	/*
	 * FIXME: Avoid using a pixel clock that is more than 99% of the cdclk
	 * as a temporary workaround. Use a higher cdclk instead. (Note that
	 * intel_compute_max_dotclk() limits the max pixel clock to 99% of max
	 * cdclk.)
	 */
	if (max_pixclk > DIV_ROUND_UP(2 * 158400 * 99, 100))
		return 316800;
#ifdef CONFIG_MACH_QNAPTS
	//abhay.kumar@intel.com: GLK will support minimum cdclk as 158.4 to enable Audio.
	else
		return 158400;
#else
	else if (max_pixclk > DIV_ROUND_UP(2 * 79200 * 99, 100))
		return 158400;
	else
		return 79200;
#endif
}

static int bxt_de_pll_vco(struct drm_i915_private *dev_priv, int cdclk)
{
	int ratio;

	if (cdclk == dev_priv->cdclk.hw.ref)
		return 0;

	switch (cdclk) {
	default:
		MISSING_CASE(cdclk);
	case 144000:
	case 288000:
	case 384000:
	case 576000:
		ratio = 60;
		break;
	case 624000:
		ratio = 65;
		break;
	}

	return dev_priv->cdclk.hw.ref * ratio;
}

static int glk_de_pll_vco(struct drm_i915_private *dev_priv, int cdclk)
{
	int ratio;

	if (cdclk == dev_priv->cdclk.hw.ref)
		return 0;

	switch (cdclk) {
	default:
		MISSING_CASE(cdclk);
	case  79200:
	case 158400:
	case 316800:
		ratio = 33;
		break;
	}

	return dev_priv->cdclk.hw.ref * ratio;
}

static void bxt_de_pll_update(struct drm_i915_private *dev_priv,
			      struct intel_cdclk_state *cdclk_state)
{
	u32 val;

	cdclk_state->ref = 19200;
	cdclk_state->vco = 0;

	val = I915_READ(BXT_DE_PLL_ENABLE);
	if ((val & BXT_DE_PLL_PLL_ENABLE) == 0)
		return;

	if (WARN_ON((val & BXT_DE_PLL_LOCK) == 0))
		return;

	val = I915_READ(BXT_DE_PLL_CTL);
	cdclk_state->vco = (val & BXT_DE_PLL_RATIO_MASK) * cdclk_state->ref;
}

static void bxt_get_cdclk(struct drm_i915_private *dev_priv,
			  struct intel_cdclk_state *cdclk_state)
{
	u32 divider;
	int div;

	bxt_de_pll_update(dev_priv, cdclk_state);

	cdclk_state->cdclk = cdclk_state->ref;

	if (cdclk_state->vco == 0)
		return;

	divider = I915_READ(CDCLK_CTL) & BXT_CDCLK_CD2X_DIV_SEL_MASK;

	switch (divider) {
	case BXT_CDCLK_CD2X_DIV_SEL_1:
		div = 2;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_1_5:
		WARN(IS_GEMINILAKE(dev_priv), "Unsupported divider\n");
		div = 3;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_2:
		div = 4;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_4:
		div = 8;
		break;
	default:
		MISSING_CASE(divider);
		return;
	}

	cdclk_state->cdclk = DIV_ROUND_CLOSEST(cdclk_state->vco, div);
}

static void bxt_de_pll_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(BXT_DE_PLL_ENABLE, 0);

	/* Timeout 200us */
	if (intel_wait_for_register(dev_priv,
				    BXT_DE_PLL_ENABLE, BXT_DE_PLL_LOCK, 0,
				    1))
		DRM_ERROR("timeout waiting for DE PLL unlock\n");

	dev_priv->cdclk.hw.vco = 0;
}

static void bxt_de_pll_enable(struct drm_i915_private *dev_priv, int vco)
{
	int ratio = DIV_ROUND_CLOSEST(vco, dev_priv->cdclk.hw.ref);
	u32 val;

	val = I915_READ(BXT_DE_PLL_CTL);
	val &= ~BXT_DE_PLL_RATIO_MASK;
	val |= BXT_DE_PLL_RATIO(ratio);
	I915_WRITE(BXT_DE_PLL_CTL, val);

	I915_WRITE(BXT_DE_PLL_ENABLE, BXT_DE_PLL_PLL_ENABLE);

	/* Timeout 200us */
	if (intel_wait_for_register(dev_priv,
				    BXT_DE_PLL_ENABLE,
				    BXT_DE_PLL_LOCK,
				    BXT_DE_PLL_LOCK,
				    1))
		DRM_ERROR("timeout waiting for DE PLL lock\n");

	dev_priv->cdclk.hw.vco = vco;
}

static void bxt_set_cdclk(struct drm_i915_private *dev_priv,
			  const struct intel_cdclk_state *cdclk_state)
{
	int cdclk = cdclk_state->cdclk;
	int vco = cdclk_state->vco;
	u32 val, divider;
	int ret;

	/* cdclk = vco / 2 / div{1,1.5,2,4} */
	switch (DIV_ROUND_CLOSEST(vco, cdclk)) {
	case 8:
		divider = BXT_CDCLK_CD2X_DIV_SEL_4;
		break;
	case 4:
		divider = BXT_CDCLK_CD2X_DIV_SEL_2;
		break;
	case 3:
		WARN(IS_GEMINILAKE(dev_priv), "Unsupported divider\n");
		divider = BXT_CDCLK_CD2X_DIV_SEL_1_5;
		break;
	case 2:
		divider = BXT_CDCLK_CD2X_DIV_SEL_1;
		break;
	default:
		WARN_ON(cdclk != dev_priv->cdclk.hw.ref);
		WARN_ON(vco != 0);

		divider = BXT_CDCLK_CD2X_DIV_SEL_1;
		break;
	}

	/* Inform power controller of upcoming frequency change */
	mutex_lock(&dev_priv->rps.hw_lock);
	ret = sandybridge_pcode_write(dev_priv, HSW_PCODE_DE_WRITE_FREQ_REQ,
				      0x80000000);
	mutex_unlock(&dev_priv->rps.hw_lock);

	if (ret) {
		DRM_ERROR("PCode CDCLK freq change notify failed (err %d, freq %d)\n",
			  ret, cdclk);
		return;
	}

	if (dev_priv->cdclk.hw.vco != 0 &&
	    dev_priv->cdclk.hw.vco != vco)
		bxt_de_pll_disable(dev_priv);

	if (dev_priv->cdclk.hw.vco != vco)
		bxt_de_pll_enable(dev_priv, vco);

	val = divider | skl_cdclk_decimal(cdclk);
	/*
	 * FIXME if only the cd2x divider needs changing, it could be done
	 * without shutting off the pipe (if only one pipe is active).
	 */
	val |= BXT_CDCLK_CD2X_PIPE_NONE;
	/*
	 * Disable SSA Precharge when CD clock frequency < 500 MHz,
	 * enable otherwise.
	 */
	if (cdclk >= 500000)
		val |= BXT_CDCLK_SSA_PRECHARGE_ENABLE;
	I915_WRITE(CDCLK_CTL, val);

	mutex_lock(&dev_priv->rps.hw_lock);
	ret = sandybridge_pcode_write(dev_priv, HSW_PCODE_DE_WRITE_FREQ_REQ,
				      DIV_ROUND_UP(cdclk, 25000));
	mutex_unlock(&dev_priv->rps.hw_lock);

	if (ret) {
		DRM_ERROR("PCode CDCLK freq set failed, (err %d, freq %d)\n",
			  ret, cdclk);
		return;
	}

	intel_update_cdclk(dev_priv);
}

static void bxt_sanitize_cdclk(struct drm_i915_private *dev_priv)
{
	u32 cdctl, expected;

	intel_update_cdclk(dev_priv);

	if (dev_priv->cdclk.hw.vco == 0 ||
	    dev_priv->cdclk.hw.cdclk == dev_priv->cdclk.hw.ref)
		goto sanitize;

	/* DPLL okay; verify the cdclock
	 *
	 * Some BIOS versions leave an incorrect decimal frequency value and
	 * set reserved MBZ bits in CDCLK_CTL at least during exiting from S4,
	 * so sanitize this register.
	 */
	cdctl = I915_READ(CDCLK_CTL);
	/*
	 * Let's ignore the pipe field, since BIOS could have configured the
	 * dividers both synching to an active pipe, or asynchronously
	 * (PIPE_NONE).
	 */
	cdctl &= ~BXT_CDCLK_CD2X_PIPE_NONE;

	expected = (cdctl & BXT_CDCLK_CD2X_DIV_SEL_MASK) |
		skl_cdclk_decimal(dev_priv->cdclk.hw.cdclk);
	/*
	 * Disable SSA Precharge when CD clock frequency < 500 MHz,
	 * enable otherwise.
	 */
	if (dev_priv->cdclk.hw.cdclk >= 500000)
		expected |= BXT_CDCLK_SSA_PRECHARGE_ENABLE;

	if (cdctl == expected)
		/* All well; nothing to sanitize */
		return;

sanitize:
	DRM_DEBUG_KMS("Sanitizing cdclk programmed by pre-os\n");

	/* force cdclk programming */
	dev_priv->cdclk.hw.cdclk = 0;

	/* force full PLL disable + enable */
	dev_priv->cdclk.hw.vco = -1;
}

/**
 * bxt_init_cdclk - Initialize CDCLK on BXT
 * @dev_priv: i915 device
 *
 * Initialize CDCLK for BXT and derivatives. This is generally
 * done only during the display core initialization sequence,
 * after which the DMC will take care of turning CDCLK off/on
 * as needed.
 */
void bxt_init_cdclk(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_state cdclk_state;

	bxt_sanitize_cdclk(dev_priv);

	if (dev_priv->cdclk.hw.cdclk != 0 &&
	    dev_priv->cdclk.hw.vco != 0)
		return;

	cdclk_state = dev_priv->cdclk.hw;

	/*
	 * FIXME:
	 * - The initial CDCLK needs to be read from VBT.
	 *   Need to make this change after VBT has changes for BXT.
	 */
	if (IS_GEMINILAKE(dev_priv)) {
		cdclk_state.cdclk = glk_calc_cdclk(0);
		cdclk_state.vco = glk_de_pll_vco(dev_priv, cdclk_state.cdclk);
	} else {
		cdclk_state.cdclk = bxt_calc_cdclk(0);
		cdclk_state.vco = bxt_de_pll_vco(dev_priv, cdclk_state.cdclk);
	}

	bxt_set_cdclk(dev_priv, &cdclk_state);
}

/**
 * bxt_uninit_cdclk - Uninitialize CDCLK on BXT
 * @dev_priv: i915 device
 *
 * Uninitialize CDCLK for BXT and derivatives. This is done only
 * during the display core uninitialization sequence.
 */
void bxt_uninit_cdclk(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_state cdclk_state = dev_priv->cdclk.hw;

	cdclk_state.cdclk = cdclk_state.ref;
	cdclk_state.vco = 0;

	bxt_set_cdclk(dev_priv, &cdclk_state);
}

static int cnl_calc_cdclk(int max_pixclk)
{
	if (max_pixclk > 336000)
		return 528000;
	else if (max_pixclk > 168000)
		return 336000;
	else
		return 168000;
}

static void cnl_cdclk_pll_update(struct drm_i915_private *dev_priv,
				 struct intel_cdclk_state *cdclk_state)
{
	u32 val;

	if (I915_READ(SKL_DSSM) & CNL_DSSM_CDCLK_PLL_REFCLK_24MHz)
		cdclk_state->ref = 24000;
	else
		cdclk_state->ref = 19200;

	cdclk_state->vco = 0;

	val = I915_READ(BXT_DE_PLL_ENABLE);
	if ((val & BXT_DE_PLL_PLL_ENABLE) == 0)
		return;

	if (WARN_ON((val & BXT_DE_PLL_LOCK) == 0))
		return;

	cdclk_state->vco = (val & CNL_CDCLK_PLL_RATIO_MASK) * cdclk_state->ref;
}

static void cnl_get_cdclk(struct drm_i915_private *dev_priv,
			 struct intel_cdclk_state *cdclk_state)
{
	u32 divider;
	int div;

	cnl_cdclk_pll_update(dev_priv, cdclk_state);

	cdclk_state->cdclk = cdclk_state->ref;

	if (cdclk_state->vco == 0)
		return;

	divider = I915_READ(CDCLK_CTL) & BXT_CDCLK_CD2X_DIV_SEL_MASK;

	switch (divider) {
	case BXT_CDCLK_CD2X_DIV_SEL_1:
		div = 2;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_2:
		div = 4;
		break;
	default:
		MISSING_CASE(divider);
		return;
	}

	cdclk_state->cdclk = DIV_ROUND_CLOSEST(cdclk_state->vco, div);
}

static void cnl_cdclk_pll_disable(struct drm_i915_private *dev_priv)
{
	u32 val;

	val = I915_READ(BXT_DE_PLL_ENABLE);
	val &= ~BXT_DE_PLL_PLL_ENABLE;
	I915_WRITE(BXT_DE_PLL_ENABLE, val);

	/* Timeout 200us */
	if (wait_for((I915_READ(BXT_DE_PLL_ENABLE) & BXT_DE_PLL_LOCK) == 0, 1))
		DRM_ERROR("timout waiting for CDCLK PLL unlock\n");

	dev_priv->cdclk.hw.vco = 0;
}

static void cnl_cdclk_pll_enable(struct drm_i915_private *dev_priv, int vco)
{
	int ratio = DIV_ROUND_CLOSEST(vco, dev_priv->cdclk.hw.ref);
	u32 val;

	val = CNL_CDCLK_PLL_RATIO(ratio);
	I915_WRITE(BXT_DE_PLL_ENABLE, val);

	val |= BXT_DE_PLL_PLL_ENABLE;
	I915_WRITE(BXT_DE_PLL_ENABLE, val);

	/* Timeout 200us */
	if (wait_for((I915_READ(BXT_DE_PLL_ENABLE) & BXT_DE_PLL_LOCK) != 0, 1))
		DRM_ERROR("timout waiting for CDCLK PLL lock\n");

	dev_priv->cdclk.hw.vco = vco;
}

static void cnl_set_cdclk(struct drm_i915_private *dev_priv,
			  const struct intel_cdclk_state *cdclk_state)
{
	int cdclk = cdclk_state->cdclk;
	int vco = cdclk_state->vco;
	u32 val, divider, pcu_ack;
	int ret;

	mutex_lock(&dev_priv->rps.hw_lock);
	ret = skl_pcode_request(dev_priv, SKL_PCODE_CDCLK_CONTROL,
				SKL_CDCLK_PREPARE_FOR_CHANGE,
				SKL_CDCLK_READY_FOR_CHANGE,
				SKL_CDCLK_READY_FOR_CHANGE, 3);
	mutex_unlock(&dev_priv->rps.hw_lock);
	if (ret) {
		DRM_ERROR("Failed to inform PCU about cdclk change (%d)\n",
			  ret);
		return;
	}

	/* cdclk = vco / 2 / div{1,2} */
	switch (DIV_ROUND_CLOSEST(vco, cdclk)) {
	case 4:
		divider = BXT_CDCLK_CD2X_DIV_SEL_2;
		break;
	case 2:
		divider = BXT_CDCLK_CD2X_DIV_SEL_1;
		break;
	default:
		WARN_ON(cdclk != dev_priv->cdclk.hw.ref);
		WARN_ON(vco != 0);

		divider = BXT_CDCLK_CD2X_DIV_SEL_1;
		break;
	}

	switch (cdclk) {
	case 528000:
		pcu_ack = 2;
		break;
	case 336000:
		pcu_ack = 1;
		break;
	case 168000:
	default:
		pcu_ack = 0;
		break;
	}

	if (dev_priv->cdclk.hw.vco != 0 &&
	    dev_priv->cdclk.hw.vco != vco)
		cnl_cdclk_pll_disable(dev_priv);

	if (dev_priv->cdclk.hw.vco != vco)
		cnl_cdclk_pll_enable(dev_priv, vco);

	val = divider | skl_cdclk_decimal(cdclk);
	/*
	 * FIXME if only the cd2x divider needs changing, it could be done
	 * without shutting off the pipe (if only one pipe is active).
	 */
	val |= BXT_CDCLK_CD2X_PIPE_NONE;
	I915_WRITE(CDCLK_CTL, val);

	/* inform PCU of the change */
	mutex_lock(&dev_priv->rps.hw_lock);
	sandybridge_pcode_write(dev_priv, SKL_PCODE_CDCLK_CONTROL, pcu_ack);
	mutex_unlock(&dev_priv->rps.hw_lock);

	intel_update_cdclk(dev_priv);
}

static int cnl_cdclk_pll_vco(struct drm_i915_private *dev_priv, int cdclk)
{
	int ratio;

	if (cdclk == dev_priv->cdclk.hw.ref)
		return 0;

	switch (cdclk) {
	default:
		MISSING_CASE(cdclk);
	case 168000:
	case 336000:
		ratio = dev_priv->cdclk.hw.ref == 19200 ? 35 : 28;
		break;
	case 528000:
		ratio = dev_priv->cdclk.hw.ref == 19200 ? 55 : 44;
		break;
	}

	return dev_priv->cdclk.hw.ref * ratio;
}

static void cnl_sanitize_cdclk(struct drm_i915_private *dev_priv)
{
	u32 cdctl, expected;

	intel_update_cdclk(dev_priv);

	if (dev_priv->cdclk.hw.vco == 0 ||
	    dev_priv->cdclk.hw.cdclk == dev_priv->cdclk.hw.ref)
		goto sanitize;

	/* DPLL okay; verify the cdclock
	 *
	 * Some BIOS versions leave an incorrect decimal frequency value and
	 * set reserved MBZ bits in CDCLK_CTL at least during exiting from S4,
	 * so sanitize this register.
	 */
	cdctl = I915_READ(CDCLK_CTL);
	/*
	 * Let's ignore the pipe field, since BIOS could have configured the
	 * dividers both synching to an active pipe, or asynchronously
	 * (PIPE_NONE).
	 */
	cdctl &= ~BXT_CDCLK_CD2X_PIPE_NONE;

	expected = (cdctl & BXT_CDCLK_CD2X_DIV_SEL_MASK) |
		   skl_cdclk_decimal(dev_priv->cdclk.hw.cdclk);

	if (cdctl == expected)
		/* All well; nothing to sanitize */
		return;

sanitize:
	DRM_DEBUG_KMS("Sanitizing cdclk programmed by pre-os\n");

	/* force cdclk programming */
	dev_priv->cdclk.hw.cdclk = 0;

	/* force full PLL disable + enable */
	dev_priv->cdclk.hw.vco = -1;
}

/**
 * cnl_init_cdclk - Initialize CDCLK on CNL
 * @dev_priv: i915 device
 *
 * Initialize CDCLK for CNL. This is generally
 * done only during the display core initialization sequence,
 * after which the DMC will take care of turning CDCLK off/on
 * as needed.
 */
void cnl_init_cdclk(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_state cdclk_state;

	cnl_sanitize_cdclk(dev_priv);

	if (dev_priv->cdclk.hw.cdclk != 0 &&
	    dev_priv->cdclk.hw.vco != 0)
		return;

	cdclk_state = dev_priv->cdclk.hw;

	cdclk_state.cdclk = cnl_calc_cdclk(0);
	cdclk_state.vco = cnl_cdclk_pll_vco(dev_priv, cdclk_state.cdclk);

	cnl_set_cdclk(dev_priv, &cdclk_state);
}

/**
 * cnl_uninit_cdclk - Uninitialize CDCLK on CNL
 * @dev_priv: i915 device
 *
 * Uninitialize CDCLK for CNL. This is done only
 * during the display core uninitialization sequence.
 */
void cnl_uninit_cdclk(struct drm_i915_private *dev_priv)
{
	struct intel_cdclk_state cdclk_state = dev_priv->cdclk.hw;

	cdclk_state.cdclk = cdclk_state.ref;
	cdclk_state.vco = 0;

	cnl_set_cdclk(dev_priv, &cdclk_state);
}

/**
 * intel_cdclk_state_compare - Determine if two CDCLK states differ
 * @a: first CDCLK state
 * @b: second CDCLK state
 *
 * Returns:
 * True if the CDCLK states are identical, false if they differ.
 */
bool intel_cdclk_state_compare(const struct intel_cdclk_state *a,
			       const struct intel_cdclk_state *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/**
 * intel_set_cdclk - Push the CDCLK state to the hardware
 * @dev_priv: i915 device
 * @cdclk_state: new CDCLK state
 *
 * Program the hardware based on the passed in CDCLK state,
 * if necessary.
 */
void intel_set_cdclk(struct drm_i915_private *dev_priv,
		     const struct intel_cdclk_state *cdclk_state)
{
	if (intel_cdclk_state_compare(&dev_priv->cdclk.hw, cdclk_state))
		return;

	if (WARN_ON_ONCE(!dev_priv->display.set_cdclk))
		return;

	DRM_DEBUG_DRIVER("Changing CDCLK to %d kHz, VCO %d kHz, ref %d kHz\n",
			 cdclk_state->cdclk, cdclk_state->vco,
			 cdclk_state->ref);

	dev_priv->display.set_cdclk(dev_priv, cdclk_state);
}

static int bdw_adjust_min_pipe_pixel_rate(struct intel_crtc_state *crtc_state,
					  int pixel_rate)
{
	struct drm_i915_private *dev_priv =
		to_i915(crtc_state->base.crtc->dev);

	/* pixel rate mustn't exceed 95% of cdclk with IPS on BDW */
	if (IS_BROADWELL(dev_priv) && crtc_state->ips_enabled)
		pixel_rate = DIV_ROUND_UP(pixel_rate * 100, 95);

	/* BSpec says "Do not use DisplayPort with CDCLK less than 432 MHz,
	 * audio enabled, port width x4, and link rate HBR2 (5.4 GHz), or else
	 * there may be audio corruption or screen corruption." This cdclk
	 * restriction for GLK is 316.8 MHz and since GLK can output two
	 * pixels per clock, the pixel rate becomes 2 * 316.8 MHz.
	 */
	if (intel_crtc_has_dp_encoder(crtc_state) &&
	    crtc_state->has_audio &&
	    crtc_state->port_clock >= 540000 &&
	    crtc_state->lane_count == 4) {
		if (IS_CANNONLAKE(dev_priv))
			pixel_rate = max(316800, pixel_rate);
		else if (IS_GEMINILAKE(dev_priv))
			pixel_rate = max(2 * 316800, pixel_rate);
		else
			pixel_rate = max(432000, pixel_rate);
	}

        /*
         * According to BSpec, "The CD clock frequency must be at least twice
         * the frequency of the Azalia BCLK." and BCLK is 96 MHz by default.
         *
         * FIXME: Check the actual, not default, BCLK being used.
         *
         * FIXME: This does not depend on ->has_audio because the higher CDCLK
         * is required for audio probe, also when there are no audio capable
         * displays connected at probe time. This leads to unnecessarily high
         * CDCLK when audio is not required.
         *
         * FIXME: This limit is only applied when there are displays connected
         * at probe time. If we probe without displays, we'll still end up using
         * the platform minimum CDCLK, failing audio probe.
	 */
	if (INTEL_GEN(dev_priv) >= 9) {
		if (IS_GEMINILAKE(dev_priv))
			pixel_rate = max(2 * 2 * 96000, pixel_rate);
		else
			pixel_rate = max(2 * 96000, pixel_rate);
	}

	return pixel_rate;
}

/* compute the max rate for new configuration */
static int intel_max_pixel_rate(struct drm_atomic_state *state)
{
	struct intel_atomic_state *intel_state = to_intel_atomic_state(state);
	struct drm_i915_private *dev_priv = to_i915(state->dev);
	struct drm_crtc *crtc;
	struct drm_crtc_state *cstate;
	struct intel_crtc_state *crtc_state;
	unsigned int max_pixel_rate = 0, i;
	enum pipe pipe;

	memcpy(intel_state->min_pixclk, dev_priv->min_pixclk,
	       sizeof(intel_state->min_pixclk));

	for_each_new_crtc_in_state(state, crtc, cstate, i) {
		int pixel_rate;

		crtc_state = to_intel_crtc_state(cstate);
		if (!crtc_state->base.enable) {
			intel_state->min_pixclk[i] = 0;
			continue;
		}

		pixel_rate = crtc_state->pixel_rate;

		if (IS_BROADWELL(dev_priv) || INTEL_GEN(dev_priv) >= 9)
			pixel_rate =
				bdw_adjust_min_pipe_pixel_rate(crtc_state,
							       pixel_rate);

		intel_state->min_pixclk[i] = pixel_rate;
	}

	for_each_pipe(dev_priv, pipe)
		max_pixel_rate = max(intel_state->min_pixclk[pipe],
				     max_pixel_rate);

	return max_pixel_rate;
}

static int vlv_modeset_calc_cdclk(struct drm_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->dev);
	int max_pixclk = intel_max_pixel_rate(state);
	struct intel_atomic_state *intel_state =
		to_intel_atomic_state(state);
	int cdclk;

	cdclk = vlv_calc_cdclk(dev_priv, max_pixclk);

	if (cdclk > dev_priv->max_cdclk_freq) {
		DRM_DEBUG_KMS("requested cdclk (%d kHz) exceeds max (%d kHz)\n",
			      cdclk, dev_priv->max_cdclk_freq);
		return -EINVAL;
	}

	intel_state->cdclk.logical.cdclk = cdclk;

	if (!intel_state->active_crtcs) {
		cdclk = vlv_calc_cdclk(dev_priv, 0);

		intel_state->cdclk.actual.cdclk = cdclk;
	} else {
		intel_state->cdclk.actual =
			intel_state->cdclk.logical;
	}

	return 0;
}

static int bdw_modeset_calc_cdclk(struct drm_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->dev);
	struct intel_atomic_state *intel_state = to_intel_atomic_state(state);
	int max_pixclk = intel_max_pixel_rate(state);
	int cdclk;

	/*
	 * FIXME should also account for plane ratio
	 * once 64bpp pixel formats are supported.
	 */
	cdclk = bdw_calc_cdclk(max_pixclk);

	if (cdclk > dev_priv->max_cdclk_freq) {
		DRM_DEBUG_KMS("requested cdclk (%d kHz) exceeds max (%d kHz)\n",
			      cdclk, dev_priv->max_cdclk_freq);
		return -EINVAL;
	}

	intel_state->cdclk.logical.cdclk = cdclk;

	if (!intel_state->active_crtcs) {
		cdclk = bdw_calc_cdclk(0);

		intel_state->cdclk.actual.cdclk = cdclk;
	} else {
		intel_state->cdclk.actual =
			intel_state->cdclk.logical;
	}

	return 0;
}

static int skl_modeset_calc_cdclk(struct drm_atomic_state *state)
{
	struct intel_atomic_state *intel_state = to_intel_atomic_state(state);
	struct drm_i915_private *dev_priv = to_i915(state->dev);
	const int max_pixclk = intel_max_pixel_rate(state);
	int cdclk, vco;

	vco = intel_state->cdclk.logical.vco;
	if (!vco)
		vco = dev_priv->skl_preferred_vco_freq;

	/*
	 * FIXME should also account for plane ratio
	 * once 64bpp pixel formats are supported.
	 */
	cdclk = skl_calc_cdclk(max_pixclk, vco);

	if (cdclk > dev_priv->max_cdclk_freq) {
		DRM_DEBUG_KMS("requested cdclk (%d kHz) exceeds max (%d kHz)\n",
			      cdclk, dev_priv->max_cdclk_freq);
		return -EINVAL;
	}

	intel_state->cdclk.logical.vco = vco;
	intel_state->cdclk.logical.cdclk = cdclk;

	if (!intel_state->active_crtcs) {
		cdclk = skl_calc_cdclk(0, vco);

		intel_state->cdclk.actual.vco = vco;
		intel_state->cdclk.actual.cdclk = cdclk;
	} else {
		intel_state->cdclk.actual =
			intel_state->cdclk.logical;
	}

	return 0;
}

static int bxt_modeset_calc_cdclk(struct drm_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->dev);
	int max_pixclk = intel_max_pixel_rate(state);
	struct intel_atomic_state *intel_state =
		to_intel_atomic_state(state);
	int cdclk, vco;

	if (IS_GEMINILAKE(dev_priv)) {
		cdclk = glk_calc_cdclk(max_pixclk);
		vco = glk_de_pll_vco(dev_priv, cdclk);
	} else {
		cdclk = bxt_calc_cdclk(max_pixclk);
		vco = bxt_de_pll_vco(dev_priv, cdclk);
	}

	if (cdclk > dev_priv->max_cdclk_freq) {
		DRM_DEBUG_KMS("requested cdclk (%d kHz) exceeds max (%d kHz)\n",
			      cdclk, dev_priv->max_cdclk_freq);
		return -EINVAL;
	}

	intel_state->cdclk.logical.vco = vco;
	intel_state->cdclk.logical.cdclk = cdclk;

	if (!intel_state->active_crtcs) {
		if (IS_GEMINILAKE(dev_priv)) {
			cdclk = glk_calc_cdclk(0);
			vco = glk_de_pll_vco(dev_priv, cdclk);
		} else {
			cdclk = bxt_calc_cdclk(0);
			vco = bxt_de_pll_vco(dev_priv, cdclk);
		}

		intel_state->cdclk.actual.vco = vco;
		intel_state->cdclk.actual.cdclk = cdclk;
	} else {
		intel_state->cdclk.actual =
			intel_state->cdclk.logical;
	}

	return 0;
}

static int cnl_modeset_calc_cdclk(struct drm_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->dev);
	struct intel_atomic_state *intel_state =
		to_intel_atomic_state(state);
	int max_pixclk = intel_max_pixel_rate(state);
	int cdclk, vco;

	cdclk = cnl_calc_cdclk(max_pixclk);
	vco = cnl_cdclk_pll_vco(dev_priv, cdclk);

	if (cdclk > dev_priv->max_cdclk_freq) {
		DRM_DEBUG_KMS("requested cdclk (%d kHz) exceeds max (%d kHz)\n",
			      cdclk, dev_priv->max_cdclk_freq);
		return -EINVAL;
	}

	intel_state->cdclk.logical.vco = vco;
	intel_state->cdclk.logical.cdclk = cdclk;

	if (!intel_state->active_crtcs) {
		cdclk = cnl_calc_cdclk(0);
		vco = cnl_cdclk_pll_vco(dev_priv, cdclk);

		intel_state->cdclk.actual.vco = vco;
		intel_state->cdclk.actual.cdclk = cdclk;
	} else {
		intel_state->cdclk.actual =
			intel_state->cdclk.logical;
	}

	return 0;
}

static int intel_compute_max_dotclk(struct drm_i915_private *dev_priv)
{
	int max_cdclk_freq = dev_priv->max_cdclk_freq;

	if (IS_GEMINILAKE(dev_priv))
		/*
		 * FIXME: Limiting to 99% as a temporary workaround. See
		 * glk_calc_cdclk() for details.
		 */
		return 2 * max_cdclk_freq * 99 / 100;
	else if (INTEL_INFO(dev_priv)->gen >= 9 ||
		 IS_HASWELL(dev_priv) || IS_BROADWELL(dev_priv))
		return max_cdclk_freq;
	else if (IS_CHERRYVIEW(dev_priv))
		return max_cdclk_freq*95/100;
	else if (INTEL_INFO(dev_priv)->gen < 4)
		return 2*max_cdclk_freq*90/100;
	else
		return max_cdclk_freq*90/100;
}

/**
 * intel_update_max_cdclk - Determine the maximum support CDCLK frequency
 * @dev_priv: i915 device
 *
 * Determine the maximum CDCLK frequency the platform supports, and also
 * derive the maximum dot clock frequency the maximum CDCLK frequency
 * allows.
 */
void intel_update_max_cdclk(struct drm_i915_private *dev_priv)
{
	if (IS_CANNONLAKE(dev_priv)) {
		dev_priv->max_cdclk_freq = 528000;
	} else if (IS_GEN9_BC(dev_priv)) {
		u32 limit = I915_READ(SKL_DFSM) & SKL_DFSM_CDCLK_LIMIT_MASK;
		int max_cdclk, vco;

		vco = dev_priv->skl_preferred_vco_freq;
		WARN_ON(vco != 8100000 && vco != 8640000);

		/*
		 * Use the lower (vco 8640) cdclk values as a
		 * first guess. skl_calc_cdclk() will correct it
		 * if the preferred vco is 8100 instead.
		 */
		if (limit == SKL_DFSM_CDCLK_LIMIT_675)
			max_cdclk = 617143;
		else if (limit == SKL_DFSM_CDCLK_LIMIT_540)
			max_cdclk = 540000;
		else if (limit == SKL_DFSM_CDCLK_LIMIT_450)
			max_cdclk = 432000;
		else
			max_cdclk = 308571;

		dev_priv->max_cdclk_freq = skl_calc_cdclk(max_cdclk, vco);
	} else if (IS_GEMINILAKE(dev_priv)) {
		dev_priv->max_cdclk_freq = 316800;
	} else if (IS_BROXTON(dev_priv)) {
		dev_priv->max_cdclk_freq = 624000;
	} else if (IS_BROADWELL(dev_priv))  {
		/*
		 * FIXME with extra cooling we can allow
		 * 540 MHz for ULX and 675 Mhz for ULT.
		 * How can we know if extra cooling is
		 * available? PCI ID, VTB, something else?
		 */
		if (I915_READ(FUSE_STRAP) & HSW_CDCLK_LIMIT)
			dev_priv->max_cdclk_freq = 450000;
		else if (IS_BDW_ULX(dev_priv))
			dev_priv->max_cdclk_freq = 450000;
		else if (IS_BDW_ULT(dev_priv))
			dev_priv->max_cdclk_freq = 540000;
		else
			dev_priv->max_cdclk_freq = 675000;
	} else if (IS_CHERRYVIEW(dev_priv)) {
		dev_priv->max_cdclk_freq = 320000;
	} else if (IS_VALLEYVIEW(dev_priv)) {
		dev_priv->max_cdclk_freq = 400000;
	} else {
		/* otherwise assume cdclk is fixed */
		dev_priv->max_cdclk_freq = dev_priv->cdclk.hw.cdclk;
	}

	dev_priv->max_dotclk_freq = intel_compute_max_dotclk(dev_priv);

	DRM_DEBUG_DRIVER("Max CD clock rate: %d kHz\n",
			 dev_priv->max_cdclk_freq);

	DRM_DEBUG_DRIVER("Max dotclock rate: %d kHz\n",
			 dev_priv->max_dotclk_freq);
}

/**
 * intel_update_cdclk - Determine the current CDCLK frequency
 * @dev_priv: i915 device
 *
 * Determine the current CDCLK frequency.
 */
void intel_update_cdclk(struct drm_i915_private *dev_priv)
{
	dev_priv->display.get_cdclk(dev_priv, &dev_priv->cdclk.hw);

	DRM_DEBUG_DRIVER("Current CD clock rate: %d kHz, VCO: %d kHz, ref: %d kHz\n",
			 dev_priv->cdclk.hw.cdclk, dev_priv->cdclk.hw.vco,
			 dev_priv->cdclk.hw.ref);

	/*
	 * 9:0 CMBUS [sic] CDCLK frequency (cdfreq):
	 * Programmng [sic] note: bit[9:2] should be programmed to the number
	 * of cdclk that generates 4MHz reference clock freq which is used to
	 * generate GMBus clock. This will vary with the cdclk freq.
	 */
	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv))
		I915_WRITE(GMBUSFREQ_VLV,
			   DIV_ROUND_UP(dev_priv->cdclk.hw.cdclk, 1000));
}

static int cnp_rawclk(struct drm_i915_private *dev_priv)
{
	u32 rawclk;
	int divider, fraction;

	if (I915_READ(SFUSE_STRAP) & SFUSE_STRAP_RAW_FREQUENCY) {
		/* 24 MHz */
		divider = 24000;
		fraction = 0;
	} else {
		/* 19.2 MHz */
		divider = 19000;
		fraction = 200;
	}

	rawclk = CNP_RAWCLK_DIV((divider / 1000) - 1);
	if (fraction)
		rawclk |= CNP_RAWCLK_FRAC(DIV_ROUND_CLOSEST(1000,
							    fraction) - 1);

	I915_WRITE(PCH_RAWCLK_FREQ, rawclk);
	return divider + fraction;
}

static int pch_rawclk(struct drm_i915_private *dev_priv)
{
	return (I915_READ(PCH_RAWCLK_FREQ) & RAWCLK_FREQ_MASK) * 1000;
}

static int vlv_hrawclk(struct drm_i915_private *dev_priv)
{
	/* RAWCLK_FREQ_VLV register updated from power well code */
	return vlv_get_cck_clock_hpll(dev_priv, "hrawclk",
				      CCK_DISPLAY_REF_CLOCK_CONTROL);
}

static int g4x_hrawclk(struct drm_i915_private *dev_priv)
{
	uint32_t clkcfg;

	/* hrawclock is 1/4 the FSB frequency */
	clkcfg = I915_READ(CLKCFG);
	switch (clkcfg & CLKCFG_FSB_MASK) {
	case CLKCFG_FSB_400:
		return 100000;
	case CLKCFG_FSB_533:
		return 133333;
	case CLKCFG_FSB_667:
		return 166667;
	case CLKCFG_FSB_800:
		return 200000;
	case CLKCFG_FSB_1067:
	case CLKCFG_FSB_1067_ALT:
		return 266667;
	case CLKCFG_FSB_1333:
	case CLKCFG_FSB_1333_ALT:
		return 333333;
	default:
		return 133333;
	}
}

/**
 * intel_update_rawclk - Determine the current RAWCLK frequency
 * @dev_priv: i915 device
 *
 * Determine the current RAWCLK frequency. RAWCLK is a fixed
 * frequency clock so this needs to done only once.
 */
void intel_update_rawclk(struct drm_i915_private *dev_priv)
{

	if (HAS_PCH_CNP(dev_priv))
		dev_priv->rawclk_freq = cnp_rawclk(dev_priv);
	else if (HAS_PCH_SPLIT(dev_priv))
		dev_priv->rawclk_freq = pch_rawclk(dev_priv);
	else if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv))
		dev_priv->rawclk_freq = vlv_hrawclk(dev_priv);
	else if (IS_G4X(dev_priv) || IS_PINEVIEW(dev_priv))
		dev_priv->rawclk_freq = g4x_hrawclk(dev_priv);
	else
		/* no rawclk on other platforms, or no need to know it */
		return;

	DRM_DEBUG_DRIVER("rawclk rate: %d kHz\n", dev_priv->rawclk_freq);
}

/**
 * intel_init_cdclk_hooks - Initialize CDCLK related modesetting hooks
 * @dev_priv: i915 device
 */
void intel_init_cdclk_hooks(struct drm_i915_private *dev_priv)
{
	if (IS_CHERRYVIEW(dev_priv)) {
		dev_priv->display.set_cdclk = chv_set_cdclk;
		dev_priv->display.modeset_calc_cdclk =
			vlv_modeset_calc_cdclk;
	} else if (IS_VALLEYVIEW(dev_priv)) {
		dev_priv->display.set_cdclk = vlv_set_cdclk;
		dev_priv->display.modeset_calc_cdclk =
			vlv_modeset_calc_cdclk;
	} else if (IS_BROADWELL(dev_priv)) {
		dev_priv->display.set_cdclk = bdw_set_cdclk;
		dev_priv->display.modeset_calc_cdclk =
			bdw_modeset_calc_cdclk;
	} else if (IS_GEN9_LP(dev_priv)) {
		dev_priv->display.set_cdclk = bxt_set_cdclk;
		dev_priv->display.modeset_calc_cdclk =
			bxt_modeset_calc_cdclk;
	} else if (IS_GEN9_BC(dev_priv)) {
		dev_priv->display.set_cdclk = skl_set_cdclk;
		dev_priv->display.modeset_calc_cdclk =
			skl_modeset_calc_cdclk;
	} else if (IS_CANNONLAKE(dev_priv)) {
		dev_priv->display.set_cdclk = cnl_set_cdclk;
		dev_priv->display.modeset_calc_cdclk =
			cnl_modeset_calc_cdclk;
	}

	if (IS_CANNONLAKE(dev_priv))
		dev_priv->display.get_cdclk = cnl_get_cdclk;
	else if (IS_GEN9_BC(dev_priv))
		dev_priv->display.get_cdclk = skl_get_cdclk;
	else if (IS_GEN9_LP(dev_priv))
		dev_priv->display.get_cdclk = bxt_get_cdclk;
	else if (IS_BROADWELL(dev_priv))
		dev_priv->display.get_cdclk = bdw_get_cdclk;
	else if (IS_HASWELL(dev_priv))
		dev_priv->display.get_cdclk = hsw_get_cdclk;
	else if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv))
		dev_priv->display.get_cdclk = vlv_get_cdclk;
	else if (IS_GEN6(dev_priv) || IS_IVYBRIDGE(dev_priv))
		dev_priv->display.get_cdclk = fixed_400mhz_get_cdclk;
	else if (IS_GEN5(dev_priv))
		dev_priv->display.get_cdclk = fixed_450mhz_get_cdclk;
	else if (IS_GM45(dev_priv))
		dev_priv->display.get_cdclk = gm45_get_cdclk;
	else if (IS_G45(dev_priv))
		dev_priv->display.get_cdclk = g33_get_cdclk;
	else if (IS_I965GM(dev_priv))
		dev_priv->display.get_cdclk = i965gm_get_cdclk;
	else if (IS_I965G(dev_priv))
		dev_priv->display.get_cdclk = fixed_400mhz_get_cdclk;
	else if (IS_PINEVIEW(dev_priv))
		dev_priv->display.get_cdclk = pnv_get_cdclk;
	else if (IS_G33(dev_priv))
		dev_priv->display.get_cdclk = g33_get_cdclk;
	else if (IS_I945GM(dev_priv))
		dev_priv->display.get_cdclk = i945gm_get_cdclk;
	else if (IS_I945G(dev_priv))
		dev_priv->display.get_cdclk = fixed_400mhz_get_cdclk;
	else if (IS_I915GM(dev_priv))
		dev_priv->display.get_cdclk = i915gm_get_cdclk;
	else if (IS_I915G(dev_priv))
		dev_priv->display.get_cdclk = fixed_333mhz_get_cdclk;
	else if (IS_I865G(dev_priv))
		dev_priv->display.get_cdclk = fixed_266mhz_get_cdclk;
	else if (IS_I85X(dev_priv))
		dev_priv->display.get_cdclk = i85x_get_cdclk;
	else if (IS_I845G(dev_priv))
		dev_priv->display.get_cdclk = fixed_200mhz_get_cdclk;
	else { /* 830 */
		WARN(!IS_I830(dev_priv),
		     "Unknown platform. Assuming 133 MHz CDCLK\n");
		dev_priv->display.get_cdclk = fixed_133mhz_get_cdclk;
	}
}
