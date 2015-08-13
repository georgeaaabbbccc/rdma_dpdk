#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <malloc.h>
#include <time.h>
#include <infiniband/verbs.h>
#include <libmemcached/memcached.h>
#include "hrd_sizes.h"

#define HRD_REGITRY_IP "128.2.211.34"	/* Anuj's desktop */

#define HRD_DEFAULT_IB_PORT 1

#define HRD_SS_WINDOW 16	/* Window size for selectively-signaled SENDs */
#define HRD_SS_WINDOW_ 15

#define HRD_Q_DEPTH 1024	/* Depth of SEND and RECV queues */
#define HRD_Q_DEPTH_ 1023

#define CPE(val, msg, err_code) \
	if(val) { fprintf(stderr, msg); fprintf(stderr, " Error %d \n", err_code); \
	exit(err_code);}

#define HRD_BASE_SHM_KEY 0

#define HRD_MAX_DATA 2044	/* Make sizeof(hrd_pktmbuf) = Max UD payload */
#define HRD_PKTMBUF_METADATA 4

struct hrd_mbuf {
	/* Some routing information */
	int s_lid, s_qpn;		/* Filled by ib_dpdk during rx_burst */
	int d_lid, d_qpn;		/* Filled by application during tx_burst */

	/* RECV DMA will start here */
	uint8_t grh[40];
	
	/* SEND payload will start here */
	int data_len;			/* Amount of data */
	uint8_t buf_addr[HRD_MAX_DATA];
};
#define hrd_pktmbuf_mtod(m) (m->buf_addr)			/* Mbuf to data */

#define HRD_MAX_LID 255		/* Maximum LID of an HCA in a local cluster */
struct hrd_qp_attr {
	int lid;
	int qpn;
};

/*
 * All the hrd-related control information
 * Each process needs only one datagram queue pair.
 */
struct hrd_ctrl_blk {
	int id;						/* A machine-unique ID of this process */
	int port_index;				/* 0-based across all devices */
	int device_id;				/* Device id */
	int port_id;				/* 1-based within this device */
	int node_id;				/* NUMA node id */

	struct ibv_context *context;
	struct ibv_pd *pd;
	
	struct ibv_cq *send_cq, *recv_cq;
	struct ibv_qp *qp;

	struct hrd_qp_attr local_qp_attrs;

	int num_remote_qps;
	struct hrd_qp_attr *remote_qp_attrs;
	struct ibv_ah **ah;			/* Address handles for all subnet nodes */

	volatile struct hrd_mbuf *recv_area;
	struct ibv_mr *recv_area_mr;

	struct ibv_send_wr wr;
	struct ibv_sge sgl;
	struct ibv_wc *wc;			/* Array of work completions */

	/* Recvs are posted for recv_head and polled from recv_tail */
	long long recv_head, recv_tail;
	long long send_count;		/* For selective signaling */
};

void hrd_ibv_devinfo(void);
uint16_t hrd_get_local_lid(struct ibv_context *context, int port_id);

struct hrd_ctrl_blk *hrd_init_ctrl_blk(int id, int port_index, int node_id);

uint16_t hrd_rx_burst(struct hrd_ctrl_blk *cb, struct hrd_mbuf **rx_pkts,
	uint16_t nb_pkts);

void hrd_tx_burst(struct hrd_ctrl_blk *cb, struct hrd_mbuf **rx_pkts,
	uint16_t nb_pkts);

void hrd_create_qp(struct hrd_ctrl_blk *ctx);

void hrd_register_qp(struct hrd_ctrl_blk *cb);
int hrd_get_registered_qps(struct hrd_ctrl_blk *cb);

void hrd_modify_qp_to_rts(struct hrd_ctrl_blk *ctx);
void hrd_post_recv(struct hrd_ctrl_blk *cb, int num_recvs);

int hrd_close_ctrl_blk(struct hrd_ctrl_blk *ctx);

void hrd_print_qp_attr(struct hrd_qp_attr);

void hrd_poll_send_cq(struct hrd_ctrl_blk *cb, int num_comps);

void hrd_red_printf(const char *format, ...);
void hrd_nano_sleep(int ns);
inline long long hrd_get_cycles();
char *hrd_getenv(const char *name);
