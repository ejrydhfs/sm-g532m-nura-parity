/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CCCI kernel RPC: wire format and engine internals.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/port_kernel.h and port_kernel.c.  Original
 * copyright (C) 2015 MediaTek Inc., GPL-2.0.
 *
 * The op ids, argument layouts and result codes below are the modem
 * firmware ABI and are copied verbatim from the downstream header; they must
 * not be changed.  The registration API itself is the frozen scaffold API in
 * include/linux/ccci.h.
 */
#ifndef __CCCI_RPC_H__
#define __CCCI_RPC_H__

#include <linux/ccci.h>
#include <linux/skbuff.h>
#include <linux/types.h>

struct ccci_port;

/* ------------------------------------------------------------------ */
/* RPC op ids (downstream eccci/port_kernel.h:107-146 RPC_OP_ID)       */
/* ------------------------------------------------------------------ */
enum ccci_rpc_op_id {
	IPC_RPC_CPSVC_SECURE_ALGO_OP	= 0x2001,
	IPC_RPC_GET_SECRO_OP		= 0x2002,

	IPC_RPC_GET_TDD_EINT_NUM_OP	= 0x4001,
	IPC_RPC_GET_GPIO_NUM_OP		= 0x4002,
	IPC_RPC_GET_ADC_NUM_OP		= 0x4003,
	IPC_RPC_GET_EMI_CLK_TYPE_OP	= 0x4004,
	IPC_RPC_GET_EINT_ATTR_OP	= 0x4005,
	IPC_RPC_GET_GPIO_VAL_OP		= 0x4006,
	IPC_RPC_GET_ADC_VAL_OP		= 0x4007,
	IPC_RPC_GET_RF_CLK_BUF_OP	= 0x4008,
	IPC_RPC_GET_GPIO_ADC_OP		= 0x4009,
	IPC_RPC_USIM2NFC_OP		= 0x400A,
	IPC_RPC_DSP_EMI_MPU_SETTING	= 0x400B,
	IPC_RPC_CCCI_QUEUE_MAPPING	= 0x400C,

	IPC_RPC_IT_OP			= 0x4321,
};

/* ------------------------------------------------------------------ */
/* Wire format (downstream port_kernel.h:147-242)                      */
/* ------------------------------------------------------------------ */
struct rpc_pkt {
	unsigned int len;
	void *buf;
} __packed;

struct rpc_buffer {
	struct ccci_header header;
	u32 op_id;
	u32 para_num;
	u8 buffer[];
} __packed;

#define RPC_REQ_BUFFER_NUM	2	/* concurrent requests the MD may have */
#define RPC_MAX_ARG_NUM		6	/* arguments per request */
#define RPC_MAX_BUF_SIZE	2048
#define RPC_API_RESP_ID		0xFFFF0000

/* Result codes, downstream FS_* */
#define FS_NO_ERROR		0
#define FS_NO_OP		(-1)
#define FS_PARAM_ERROR		(-2)
#define FS_NO_FEATURE		(-3)
#define FS_NO_MATCH		(-4)
#define FS_FUNC_FAIL		(-5)
#define FS_ERROR_RESERVED	(-6)
#define FS_MEM_OVERFLOW		(-7)

/* ------------------------------------------------------------------ */
/* GPIO / ADC / clock buffer payloads                                  */
/* ------------------------------------------------------------------ */
/* CONFIG_MTK_TC1_FEATURE is not set in this ROM build (no LGE op ids),
 * therefore the v1 GPIO count is 3 as in downstream port_kernel.h:173.
 */
#define GPIO_MAX_COUNT			3
#define GPIO_MAX_COUNT_V2		10
#define GPIO_PIN_NAME_STR_MAX_LEN	34
#define ADC_CH_NAME_STR_MAX_LEN		33

enum ccci_rpc_req_mask {
	RPC_REQ_GPIO_PIN	= BIT(0),
	RPC_REQ_GPIO_VALUE	= BIT(1),
	RPC_REQ_ADC_PIN		= BIT(4),
	RPC_REQ_ADC_VALUE	= BIT(5),
};

struct ccci_rpc_gpio_adc_input {
	u8 reqMask;
	u8 gpioValidPinMask;
	char gpioPinName[GPIO_MAX_COUNT][GPIO_PIN_NAME_STR_MAX_LEN];
	u32 gpioPinNum[GPIO_MAX_COUNT];
	char adcChName[ADC_CH_NAME_STR_MAX_LEN];
	u32 adcChNum;
	u32 adcChMeasCount;
} __packed;

struct ccci_rpc_gpio_adc_output {
	u32 gpioPinNum[GPIO_MAX_COUNT];
	u32 gpioPinValue[GPIO_MAX_COUNT];
	u32 adcChNum;
	u32 adcChMeasSum;
} __packed;

struct ccci_rpc_gpio_adc_input_v2 {
	u16 reqMask;
	u16 gpioValidPinMask;
	char gpioPinName[GPIO_MAX_COUNT_V2][GPIO_PIN_NAME_STR_MAX_LEN];
	u32 gpioPinNum[GPIO_MAX_COUNT_V2];
	char adcChName[ADC_CH_NAME_STR_MAX_LEN];
	u32 adcChNum;
	u32 adcChMeasCount;
} __packed;

struct ccci_rpc_gpio_adc_output_v2 {
	u32 gpioPinNum[GPIO_MAX_COUNT_V2];
	u32 gpioPinValue[GPIO_MAX_COUNT_V2];
	u32 adcChNum;
	u32 adcChMeasSum;
} __packed;

struct ccci_rpc_dsp_emi_mpu_input {
	u32 request;
} __packed;

struct ccci_rpc_usim2nfs {
	u8 lock_vsim1;
} __packed;

#define MAX_CHANNEL_QUEUE_MAPPING	10
struct ccci_rpc_queue_mapping {
	u32 ch_cnt;
	struct {
		u32 channel;
		union {
			u32 queue_type;
			u32 hif_q_type;
		};
	} ch_queue_mapping[MAX_CHANNEL_QUEUE_MAPPING];
} __packed;

#define CLKBUF_MAX_COUNT	4
struct ccci_rpc_clkbuf_result {
	u16 CLKBuf_Count;
	u8 CLKBuf_Status[CLKBUF_MAX_COUNT];
	u8 CLKBuf_SWCtrl_Status[CLKBUF_MAX_COUNT];
} __packed;

/* ------------------------------------------------------------------ */
/* SIM hot plug EINT attribute query                                   */
/* (downstream port_kernel.h:265-300, FEATURE_GET_MD_EINT_ATTR_DTS)    */
/* ------------------------------------------------------------------ */
#define CCCI_RPC_MD_SIM_MAX	16

enum sim_hot_plug_eint_query_type {
	SIM_HOT_PLUG_EINT_NUMBER,
	SIM_HOT_PLUG_EINT_DEBOUNCETIME,
	SIM_HOT_PLUG_EINT_POLARITY,
	SIM_HOT_PLUG_EINT_SENSITIVITY,
	SIM_HOT_PLUG_EINT_SOCKETTYPE,
	SIM_HOT_PLUG_EINT_DEDICATEDEN,
	SIM_HOT_PLUG_EINT_SRCPIN,

	SIM_HOT_PLUG_EINT_MAX,
};

enum sim_hot_plug_eint_query_err {
	ERR_SIM_HOT_PLUG_NULL_POINTER = -13,
	ERR_SIM_HOT_PLUG_QUERY_TYPE,
	ERR_SIM_HOT_PLUG_QUERY_STRING,
};

/* ------------------------------------------------------------------ */
/* Engine                                                              */
/* ------------------------------------------------------------------ */

/*
 * Response staging arena.  Before a handler is called the engine sets every
 * rsp->args[i].buf to this arena (rsp->args[i].len stays 0).  A handler may
 * lay its results out inside the arena and point the args at them; it may
 * also point an argument at any other buffer that remains valid until the
 * handler returns.  The engine copies the arguments into the Rx skb and
 * sends it back on CCCI_RPC_TX.
 */
#define CCCI_RPC_ARENA_SIZE	RPC_MAX_BUF_SIZE

/*
 * Longest modem-supplied name (GPIO/ADC/EINT) the engine copies out of a
 * request: the downstream buffers are 64 bytes (port_kernel.c:1492,1509) and
 * the wire names are at most 34 bytes (GPIO_PIN_NAME_STR_MAX_LEN).
 */
#define CCCI_RPC_NAME_MAX	64

int ccci_rpc_init(void);
void ccci_rpc_exit(void);

/*
 * ccci_rpc_handle - answer one CCCI_RPC_RX request
 *
 * Always consumes @skb (either sends it back with the response or frees it)
 * and must be called from process context (the kernel port thread).
 */
void ccci_rpc_handle(struct ccci_port *port, struct sk_buff *skb);

#endif /* __CCCI_RPC_H__ */
