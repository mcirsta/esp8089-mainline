/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2009- 2014 Espressif System. */
#ifndef _ESP_SIP_H
#define _ESP_SIP_H

#include "sip2_common.h"
#include "txreport.h"
#include "power.h"

#define SIP_CTRL_CREDIT_RESERVE 2

#define SIP_PKT_MAX_LEN (1024 * 16)

/* 16KB on normal X86 system, should check before porting to orhters */

#define SIP_TX_AGGR_BUF_SIZE (4 * PAGE_SIZE)
#define SIP_RX_AGGR_BUF_SIZE (4 * PAGE_SIZE)

struct sk_buff;

struct sip_pkt {
	struct list_head list;

	u8 *buf_begin;
	u32 buf_len;
	u8 *buf;
};

enum recalc_credit_state {
	RECALC_CREDIT_DISABLE = 0,
	RECALC_CREDIT_ENABLE = 1,
};

enum enqueue_prior {
	ENQUEUE_PRIOR_TAIL = 0,
	ENQUEUE_PRIOR_HEAD,
};

enum sip_state {
	SIP_INIT = 0,
	SIP_PREPARE_BOOT,
	SIP_BOOT,
	SIP_SEND_INIT,
	SIP_WAIT_BOOTUP,
	SIP_RUN,
	SIP_SUSPEND,
	SIP_STOP
};

enum sip_notifier {
	SIP_TX_DONE = 1,
	SIP_RX_DONE = 2,
};

#define SIP_CREDITS_LOW_THRESHOLD 64 //i.e. 4k

struct esp_sip {
	struct esp_reports reports;
	struct esp_power power;
	struct list_head free_ctrl_txbuf;
	struct list_head free_ctrl_rxbuf;

	u32 rxseq; /* sip pkt seq, should match target side */
	u32 txseq;
	u32 txdataseq;

	u8 to_host_seq;

	atomic_t state;
	spinlock_t lock; /* Boot control-buffer freelists. */
	/* Outer to skb queue locks; never held over I/O or synchronous waits. */
	spinlock_t state_lock; /* Stop vs publication of work, timers and TX. */
	atomic_t tx_credits;

	atomic_t tx_ask_credit_update;

	u8 *rawbuf; /* used in boot stage, free once chip is fully up */
	u8 *tx_aggr_buf;
	u8 *tx_aggr_write_ptr; /* update after insertion of each pkt */
	u8 *tx_aggr_lastpkt_ptr;

	struct mutex rx_mtx; /* Ordered receive parsing and boot events. */
	struct sk_buff_head rxq;
	struct work_struct rx_process_work;

	u16 tx_blksz;
	u16 rx_blksz;

	bool dump_rpbm_err;
	bool sendup_rpbm_pkt;
	bool rxabort_fixed;
	bool support_bgscan;
	u8 credit_to_reserve;

	atomic_t credit_status;
	struct timer_list credit_timer;

	atomic_t noise_floor;

	u32 tx_tot_len; /* total len for one transaction */
	u32 rx_tot_len;

	atomic_t rx_handling;
	atomic_t tx_data_pkt_queued;

	atomic_t data_tx_stopped;
	atomic_t tx_stopped;

	struct esp_pub *epub;
};

int sip_rx(struct esp_pub *epub);

int sip_write_memory(struct esp_sip *sip, u32 addr, const u8 *buf, u32 len);

void sip_credit_process(struct esp_pub *epub, u8 credits);

int sip_send_cmd(struct esp_sip *sip, int cid, u32 cmdlen, void *cmd);

struct esp_sip *sip_attach(struct esp_pub *epub);

int sip_post_init(struct esp_sip *sip, struct sip_evt_bootup2 *bevt);

void sip_detach(struct esp_sip *sip);
void sip_quiesce(struct esp_sip *sip);
void sip_stop(struct esp_sip *sip);
bool sip_set_state(struct esp_sip *sip, enum sip_state state);
bool sip_queue_work(struct esp_sip *sip, struct work_struct *work);
void sip_queue_report_poll(struct esp_sip *sip);
void sip_rearm_credit_timer(struct esp_sip *sip);
int sip_prepare_boot(struct esp_sip *sip);

void sip_txq_process(struct esp_pub *epub);

struct sk_buff *sip_alloc_ctrl_skbuf(struct esp_sip *sip, u16 len, u32 cid);

void sip_free_ctrl_skbuff(struct esp_sip *sip, struct sk_buff *skb);

bool sip_queue_need_stop(struct esp_sip *sip);
bool sip_queue_may_resume(struct esp_sip *sip);

void sip_tx_data_pkt_enqueue(struct esp_pub *epub, struct sk_buff *skb);

int sip_cmd_enqueue(struct esp_sip *sip, struct sk_buff *skb, int prior);

int sip_poll_bootup_event(struct esp_sip *sip);

int sip_poll_resetting_event(struct esp_sip *sip);

void sip_trigger_txq_process(struct esp_sip *sip);
void sip_maybe_start_tx_ba(struct esp_sip *sip, struct sk_buff *skb);

void sip_send_chip_init(struct esp_sip *sip);

#if IS_ENABLED(CONFIG_KUNIT)
struct esp_mac_rx_ctrl;
struct sk_buff *sip_parse_data_rx_info(struct esp_sip *sip,
				     struct sk_buff *skb, int pkt_len_enc,
				     int buf_len, struct esp_mac_rx_ctrl *ctrl,
				     int *pulled_len);
struct sk_buff_head;
void sip_rx_ampdu(struct esp_sip *sip, struct sk_buff *view,
		  struct esp_mac_rx_ctrl *ctrl, const __le32 *entries,
		  struct sk_buff_head *frames);
#endif

#endif
