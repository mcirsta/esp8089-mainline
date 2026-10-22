/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2011 - 2014 Espressif System. */
#ifndef _ESP_SIF_H_
#define _ESP_SIF_H_

#include "esp_pub.h"
#include <linux/mmc/host.h>
#include <linux/string.h>

static inline int esp_sdio_prepare_tx(u8 *dma, u32 capacity, const u8 *data,
				      u32 length, u32 wire_length)
{
	if (!length || length > wire_length || wire_length > capacity)
		return -EINVAL;
	if (dma != data)
		memcpy(dma, data, length);
	memset(dma + length, 0, wire_length - length);
	return 0;
}

/*
 *  H/W SLC module definitions
 */

#define SIF_SLC_BLOCK_SIZE 512

/* S/W struct mapping to slc registers */
struct slc_host_regs {
	/* do NOT read token_rdata
	 *
	 __le32 pf_data;
	 __le32 token_rdata;
	 */
	__le32 intr_raw;
	__le32 state_w0;
	__le32 state_w1;
	__le32 config_w0;
	__le32 config_w1;
	__le32 intr_status;
	__le32 config_w2;
	__le32 config_w3;
	__le32 config_w4;
	__le32 token_wdata;
	__le32 intr_clear;
	__le32 intr_enable;
};

enum io_sync_type {
	ESP_SIF_NOSYNC = 0,
	ESP_SIF_SYNC,
};

struct esp_sdio_ctrl {
	struct sdio_func *func;
	struct esp_pub *epub;

	u8 *dma_buffer;

	bool off;
	const struct sdio_device_id *id;
	u32 slc_blk_sz;
	u32 target_id;
	u32 slc_window_end_addr;

	struct slc_host_regs slc_regs;
	atomic_t irq_installed;
};

#define SIF_TO_DEVICE 0x1
#define SIF_FROM_DEVICE 0x2

#define SIF_SYNC 0x00000010
#define SIF_ASYNC 0x00000020

#define SIF_BYTE_BASIS 0x00000040
#define SIF_BLOCK_BASIS 0x00000080

#define SIF_FIXED_ADDR 0x00000100
#define SIF_INC_ADDR 0x00000200

#define EPUB_TO_CTRL(_epub) (((struct esp_sdio_ctrl *)(_epub)->sif))

#define EPUB_TO_FUNC(_epub) (((struct esp_sdio_ctrl *)(_epub)->sif)->func)

void sdio_io_writeb(struct esp_pub *epub, u8 value, int addr, int *res);
u8 sdio_io_readb(struct esp_pub *epub, int addr, int *res);

int sif_enable_irq(struct esp_pub *epub);
void sif_disable_irq(struct esp_pub *epub);
void sif_disable_target_interrupt(struct esp_pub *epub);
void sif_halt_transport(struct esp_pub *epub, int error);

u32 sif_get_blksz(struct esp_pub *epub);
u32 sif_get_target_id(struct esp_pub *epub);

void sif_dsr(struct sdio_func *func);
int sif_io_raw(struct esp_pub *epub, u32 addr, u8 *buf, u32 len, u32 flag);
int sif_io_sync(struct esp_pub *epub, u32 addr, u8 *buf, u32 len, u32 flag);
int sif_lldesc_read_sync(struct esp_pub *epub, u8 *buf, u32 len);
int sif_lldesc_write_sync(struct esp_pub *epub, u8 *buf, u32 len);
int sif_lldesc_read_raw(struct esp_pub *epub, u8 *buf, u32 len, bool noround);
int sif_lldesc_write_raw(struct esp_pub *epub, u8 *buf, u32 len);

int esp_common_read(struct esp_pub *epub, u8 *buf, u32 len, int sync,
		    bool noround);
int esp_common_write(struct esp_pub *epub, u8 *buf, u32 len, int sync);
int esp_common_read_with_addr(struct esp_pub *epub, u32 addr, u8 *buf, u32 len,
			      int sync);
int esp_common_write_with_addr(struct esp_pub *epub, u32 addr, u8 *buf, u32 len,
			       int sync);

int esp_common_readbyte_with_addr(struct esp_pub *epub, u32 addr, u8 *buf,
				  int sync);
int esp_common_writebyte_with_addr(struct esp_pub *epub, u32 addr, u8 buf,
				   int sync);

int sif_read_reg_window(struct esp_pub *epub, unsigned int reg_addr,
			unsigned char *value);
int sif_write_reg_window(struct esp_pub *epub, unsigned int reg_addr,
			 unsigned char *value);
int sif_ack_target_read_err(struct esp_pub *epub);
int sif_had_io_enable(struct esp_pub *epub);

struct slc_host_regs *sif_get_regs(struct esp_pub *epub);

void sif_lock_bus(struct esp_pub *epub);
void sif_unlock_bus(struct esp_pub *epub);

int sif_interrupt_target(struct esp_pub *epub, u8 index);

int check_target_id(struct esp_pub *epub);

#define sif_reg_read_sync(epub, addr, buf, len)   \
	sif_io_sync((epub), (addr), (buf), (len), \
		    SIF_FROM_DEVICE | SIF_BYTE_BASIS | SIF_INC_ADDR)

#define sif_reg_write_sync(epub, addr, buf, len)  \
	sif_io_sync((epub), (addr), (buf), (len), \
		    SIF_TO_DEVICE | SIF_BYTE_BASIS | SIF_INC_ADDR)

#endif /* _ESP_SIF_H_ */
