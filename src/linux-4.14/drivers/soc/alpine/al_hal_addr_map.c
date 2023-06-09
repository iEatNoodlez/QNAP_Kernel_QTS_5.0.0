/*******************************************************************************
Copyright (C) 2015 Annapurna Labs Ltd.

This file may be licensed under the terms of the Annapurna Labs Commercial
License Agreement.

Alternatively, this file can be distributed under the terms of the GNU General
Public License V2 as published by the Free Software Foundation and can be
found at http://www.gnu.org/licenses/gpl-2.0.html

Alternatively, redistribution and use in source and binary forms, with or
without modification, are permitted provided that the following conditions are
met:

*     Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

*     Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in
the documentation and/or other materials provided with the
distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

/**
* @{
* @file   al_hal_addr_map.c
*
* @brief  includes Address Map HAL implementation
*
*/

#include "al_hal_addr_map.h"
#include "al_hal_pbs_regs.h"
#include "al_hal_pbs_utils.h"

#define AL_ADDR_MAP_REV_2_3_PASW_LOG2SIZE_MAX		48
#define AL_ADDR_MAP_REV_0_1_PASW_LOG2SIZE_MAX		40
#define AL_ADDR_MAP_PASW_LOG2SIZE_BASE				15

struct al_addr_map_dma_io_master_map_ent {
	enum al_addr_map_dma_io_master	master;
	int				bit_idx;
};

static const struct al_addr_map_dma_io_master_map_ent dma_io_master_map[] = {
	{ AL_ADDR_MAP_DMA_IO_MASTER_CRYPTO_0, 0 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_CRYPTO_1, 1 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_ETH_0, 2 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_ETH_1, 3 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_ETH_2, 4 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_ETH_3, 5 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_SATA_0, 6 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_SATA_1, 7 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_PCIE_0, 8 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_PCIE_1, 9 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_PCIE_2, 10 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_PCIE_3, 11 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_SPI_DBG, 12 },
	{ AL_ADDR_MAP_DMA_IO_MASTER_CPU_DBG, 13 },
};

static int al_addr_map_latch_set(
		void __iomem *pbs_regs_base,
		al_bool latch_en)
{
	struct al_pbs_regs *pbs_regs = (struct al_pbs_regs *)pbs_regs_base;

	if (latch_en == AL_TRUE)
		al_reg_write32(&pbs_regs->unit.latch_bars, 0);
	else
		al_reg_write32(&pbs_regs->unit.latch_bars, 1);

	return 0;
}

static int al_addr_map_get_bar_regs(
		struct al_pbs_regs *pbs_regs,
		enum al_addr_map_pasw pasw,
		uint32_t **bar_reg_high,
		uint32_t **bar_reg_low)
{
	switch (pasw) {
	case AL_ADDR_MAP_PASW_NB_DRAM0:
		*bar_reg_high = &pbs_regs->unit.dram_0_nb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_0_nb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_NB_DRAM1:
		*bar_reg_high = &pbs_regs->unit.dram_1_nb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_1_nb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_NB_DRAM2:
		*bar_reg_high = &pbs_regs->unit.dram_2_nb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_2_nb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_NB_DRAM3:
		*bar_reg_high = &pbs_regs->unit.dram_3_nb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_3_nb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_NB_MSIX:
		*bar_reg_high = &pbs_regs->unit.msix_nb_bar_high;
		*bar_reg_low = &pbs_regs->unit.msix_nb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_SB_DRAM0:
		*bar_reg_high = &pbs_regs->unit.dram_0_sb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_0_sb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_SB_DRAM1:
		*bar_reg_high = &pbs_regs->unit.dram_1_sb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_1_sb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_SB_DRAM2:
		*bar_reg_high = &pbs_regs->unit.dram_2_sb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_2_sb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_SB_DRAM3:
		*bar_reg_high = &pbs_regs->unit.dram_3_sb_bar_high;
		*bar_reg_low = &pbs_regs->unit.dram_3_sb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_SB_MSIX:
		*bar_reg_high = &pbs_regs->unit.msix_sb_bar_high;
		*bar_reg_low = &pbs_regs->unit.msix_sb_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_MEM0:
		*bar_reg_high = &pbs_regs->unit.pcie_mem0_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_mem0_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_MEM1:
		*bar_reg_high = &pbs_regs->unit.pcie_mem1_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_mem1_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_MEM2:
		*bar_reg_high = &pbs_regs->unit.pcie_mem2_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_mem2_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_MEM3:
		*bar_reg_high = &pbs_regs->unit.pcie_mem3_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_mem3_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_ECAM0:
		*bar_reg_high = &pbs_regs->unit.pcie_ext_ecam0_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_ext_ecam0_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_ECAM1:
		*bar_reg_high = &pbs_regs->unit.pcie_ext_ecam1_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_ext_ecam1_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_ECAM2:
		*bar_reg_high = &pbs_regs->unit.pcie_ext_ecam2_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_ext_ecam2_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_EXT_ECAM3:
		*bar_reg_high = &pbs_regs->unit.pcie_ext_ecam3_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_ext_ecam3_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PBS_SPI:
		*bar_reg_high = &pbs_regs->unit.pbs_spi_bar_high;
		*bar_reg_low = &pbs_regs->unit.pbs_spi_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PBS_NAND:
		*bar_reg_high = &pbs_regs->unit.pbs_nand_bar_high;
		*bar_reg_low = &pbs_regs->unit.pbs_nand_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PBS_NOR:
		*bar_reg_high = &pbs_regs->unit.pbs_nor_bar_high;
		*bar_reg_low = &pbs_regs->unit.pbs_nor_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PBS_INT_MEM:
		*bar_reg_high = &pbs_regs->unit.pbs_int_mem_bar_high;
		*bar_reg_low = &pbs_regs->unit.pbs_int_mem_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PBS_BOOT:
		*bar_reg_high = &pbs_regs->unit.pbs_boot_bar_high;
		*bar_reg_low = &pbs_regs->unit.pbs_boot_bar_low;
		break;
	case AL_ADDR_MAP_PASW_NB_INT:
		*bar_reg_high = &pbs_regs->unit.nb_int_bar_high;
		*bar_reg_low = &pbs_regs->unit.nb_int_bar_low;
		break;
	case AL_ADDR_MAP_PASW_NB_STM:
		*bar_reg_high = &pbs_regs->unit.nb_stm_bar_high;
		*bar_reg_low = &pbs_regs->unit.nb_stm_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_INT_ECAM:
		*bar_reg_high = &pbs_regs->unit.pcie_ecam_int_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_ecam_int_bar_low;
		break;
	case AL_ADDR_MAP_PASW_PCIE_INT_MEM:
		*bar_reg_high = &pbs_regs->unit.pcie_mem_int_bar_high;
		*bar_reg_low = &pbs_regs->unit.pcie_mem_int_bar_low;
		break;
	case AL_ADDR_MAP_PASW_SB_INT:
		*bar_reg_high = &pbs_regs->unit.sb_int_bar_high;
		*bar_reg_low = &pbs_regs->unit.sb_int_bar_low;
		break;
	default:
		al_err("addr_map: unknown pasw %d\n", pasw);
		return -EINVAL;
	}

	return 0;
}

int al_addr_map_pasw_set(
		void __iomem *pbs_regs_base,
		enum al_addr_map_pasw pasw,
		al_phys_addr_t base,
		uint32_t log2size)
{
	struct al_pbs_regs *pbs_regs = (struct al_pbs_regs *)pbs_regs_base;
	uint32_t dev_id;
	uint64_t size_mask;
	uint32_t base_low;
	uint32_t base_high;
	uint32_t *bar_reg_high;
	uint32_t *bar_reg_low;
	uint8_t	pasw_log2size_max;

	dev_id = al_pbs_dev_id_get(pbs_regs_base);

	if (dev_id == PBS_UNIT_CHIP_ID_DEV_ID_ALPINE_V1)
		pasw_log2size_max = AL_ADDR_MAP_REV_0_1_PASW_LOG2SIZE_MAX;
	else
		pasw_log2size_max = AL_ADDR_MAP_REV_2_3_PASW_LOG2SIZE_MAX;

	if (log2size > pasw_log2size_max) {
			al_err("addr_map: max pasw log2size is %d\n", pasw_log2size_max);
		return -EINVAL;
	}

	if (log2size < AL_ADDR_MAP_PASW_LOG2SIZE_BASE) {
		al_err("addr_map: min pasw log2size is 15 (PASW disabled)\n");
		return -EINVAL;
	}

	if (log2size > AL_ADDR_MAP_PASW_LOG2SIZE_BASE) {
		size_mask = ((uint64_t)1 << log2size) - 1;
		if (base & size_mask) {
			al_err("addr_map: pasw base has to be aligned to size\n"
					"base=0x%16" PRIx64 "\n"
					"log2size=%d\n"
					"size_mask=0x%16" PRIx64 "\n",
							base,
							log2size,
							size_mask);
			return -EINVAL;
		}
	}

	/* check that base+size fit max allowed size */
	if ((base + ((uint64_t)1 << log2size)) > ((uint64_t)1 << pasw_log2size_max)) {
		al_err("addr_map: base + size doesn't fit %dbit\n", pasw_log2size_max);
		return -EINVAL;
	}

	if (al_addr_map_get_bar_regs(pbs_regs, pasw,
					&bar_reg_high, &bar_reg_low))
		return -EINVAL;

	base_high = (uint32_t)((base >> 32) & 0xffffffff);
	base_low = (uint32_t)((base & 0xffffffff) |
			(log2size - AL_ADDR_MAP_PASW_LOG2SIZE_BASE));

	al_addr_map_latch_set(pbs_regs_base, AL_FALSE);

	al_reg_write32(bar_reg_high, base_high);
	al_reg_write32(bar_reg_low, base_low);

	al_addr_map_latch_set(pbs_regs_base, AL_TRUE);

	return 0;
}
EXPORT_SYMBOL(al_addr_map_pasw_set);

int al_addr_map_pasw_get(
		void __iomem *pbs_regs_base,
		enum al_addr_map_pasw pasw,
		al_phys_addr_t *base,
		uint32_t *log2size)
{
	struct al_pbs_regs *pbs_regs = (struct al_pbs_regs *)pbs_regs_base;
	uint32_t base_low;
	uint32_t base_high;
	uint32_t win_size;
	uint32_t *bar_reg_high;
	uint32_t *bar_reg_low;

	al_assert(base);
	al_assert(log2size);

	if (al_addr_map_get_bar_regs(pbs_regs, pasw,
			&bar_reg_high, &bar_reg_low))
		return -EINVAL;

	base_high = al_reg_read32(bar_reg_high);
	base_low = al_reg_read32(bar_reg_low);

	win_size = base_low & PBS_PASW_WIN_SIZE_MASK;
	*log2size = win_size + AL_ADDR_MAP_PASW_LOG2SIZE_BASE;

	*base = ((al_phys_addr_t)base_high << 32) |
			(al_phys_addr_t)(base_low &
					PBS_PASW_BAR_LOW_ADDR_HIGH_MASK);

	return 0;
}

int al_addr_map_dram_remap_set(
		void __iomem *pbs_regs_base,
		al_phys_addr_t dram_remap_base,
		al_phys_addr_t dram_remap_transl_base,
		uint8_t window_size)
{
	struct al_pbs_regs *pbs_regs;
	uint32_t sb2nb_remap_reg_val = 0;
	uint32_t remap_base_val;
	uint32_t remap_transl_base_val;

	al_assert(window_size < 4);
	al_assert(pbs_regs_base);
	al_assert(dram_remap_base >=
			(1 << PBS_UNIT_DRAM_SRC_REMAP_BASE_ADDR_SHIFT));
	al_assert(dram_remap_transl_base >=
			(1 << PBS_UNIT_DRAM_DST_REMAP_BASE_ADDR_SHIFT));

	pbs_regs = (struct al_pbs_regs *)pbs_regs_base;

	remap_base_val =
		(dram_remap_base & PBS_UNIT_DRAM_REMAP_BASE_ADDR_MASK) >>
				PBS_UNIT_DRAM_SRC_REMAP_BASE_ADDR_SHIFT;

	remap_transl_base_val =
		(dram_remap_transl_base & PBS_UNIT_DRAM_REMAP_BASE_ADDR_MASK) >>
				PBS_UNIT_DRAM_DST_REMAP_BASE_ADDR_SHIFT;

	sb2nb_remap_reg_val =
			(remap_base_val <<
				PBS_UNIT_SB2NB_REMAP_BASE_ADDR_SHIFT) |
			(remap_transl_base_val <<
				PBS_UNIT_SB2NB_REMAP_TRANSL_BASE_ADDR_SHIFT) |
			window_size;

	al_addr_map_latch_set(pbs_regs_base, AL_FALSE);

	al_reg_write32(&pbs_regs->unit.sb2nb_cfg_dram_remap,
						sb2nb_remap_reg_val);

	al_addr_map_latch_set(pbs_regs_base, AL_TRUE);

	return 0;
}

static int dma_io_master_bit_idx_get(
	enum al_addr_map_dma_io_master master)
{
	unsigned int i;

	for (i = 0; i < AL_ARR_SIZE(dma_io_master_map); i++)
		if (dma_io_master_map[i].master == master)
			return dma_io_master_map[i].bit_idx;

	return -EINVAL;
}

int al_addr_map_dma_io_master_cfg_set(
	void __iomem				*pbs_regs_base,
	enum al_addr_map_dma_io_master		master,
	enum al_addr_map_dma_io_master_cfg	cfg)
{
	struct al_pbs_regs *pbs_regs = (struct al_pbs_regs *)pbs_regs_base;
	int bit_idx;

	al_assert(pbs_regs_base);

	bit_idx = dma_io_master_bit_idx_get(master);
	if (bit_idx < 0)
		return bit_idx;

	al_reg_write32_masked(
		&pbs_regs->unit.dma_io_master_map,
		AL_BIT(bit_idx + PBS_UNIT_DMA_IO_MASTER_MAP_CNTL_SHIFT) |
		AL_BIT(bit_idx + PBS_UNIT_DMA_IO_MASTER_MAP_DFLT_SLV_SHIFT),
		(cfg == AL_ADDR_MAP_DMA_IO_MASTER_CFG_DRAM_ALWAYS) ?
		AL_BIT(bit_idx + PBS_UNIT_DMA_IO_MASTER_MAP_CNTL_SHIFT) :
		(cfg == AL_ADDR_MAP_DMA_IO_MASTER_CFG_DRAM_ON_MISS) ?
		AL_BIT(bit_idx + PBS_UNIT_DMA_IO_MASTER_MAP_DFLT_SLV_SHIFT) :
		0);

	return 0;
}

int al_addr_map_dma_io_master_cfg_get(
	void __iomem				*pbs_regs_base,
	enum al_addr_map_dma_io_master		master,
	enum al_addr_map_dma_io_master_cfg	*cfg)
{
	struct al_pbs_regs *pbs_regs = (struct al_pbs_regs *)pbs_regs_base;
	int bit_idx;
	uint32_t reg_val;

	al_assert(pbs_regs_base);
	al_assert(cfg);

	bit_idx = dma_io_master_bit_idx_get(master);
	if (bit_idx < 0)
		return bit_idx;

	reg_val = al_reg_read32(&pbs_regs->unit.dma_io_master_map);

	if (reg_val & AL_BIT(bit_idx + PBS_UNIT_DMA_IO_MASTER_MAP_CNTL_SHIFT))
		*cfg = AL_ADDR_MAP_DMA_IO_MASTER_CFG_DRAM_ALWAYS;
	else if (reg_val & AL_BIT(bit_idx + PBS_UNIT_DMA_IO_MASTER_MAP_DFLT_SLV_SHIFT))
		*cfg = AL_ADDR_MAP_DMA_IO_MASTER_CFG_DRAM_ON_MISS;
	else
		*cfg = AL_ADDR_MAP_DMA_IO_MASTER_CFG_ERROR_ON_MISS;

	return 0;
}

