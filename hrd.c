#include "hrd.h"

/* Print information about all IB devices in the system */
void hrd_ibv_devinfo(void)
{
	int num_devices = 0, dev_i;
	struct ibv_device **dev_list;
	struct ibv_context *context;
	struct ibv_device_attr device_attr;

	hrd_red_printf("HRD: printing IB dev info\n");
	
	dev_list = ibv_get_device_list(&num_devices);
	CPE(!dev_list, "Failed to get IB devices list", 0);
	
	for (dev_i = 0; dev_i < num_devices; dev_i ++) {
		context = ibv_open_device(dev_list[dev_i]);
		CPE(!context, "Couldn't get context", 0);

		memset(&device_attr, 0, sizeof(device_attr));
		if (ibv_query_device(context, &device_attr)) {
			printf("Could not query device: %d\n", dev_i);
			assert(false);
		}

		printf("IB device %d:\n", dev_i);
		printf("    Name: %s\n", dev_list[dev_i]->name);
		printf("    Device name: %s\n", dev_list[dev_i]->dev_name);
		printf("    GUID: %016lx\n", ibv_get_device_guid(dev_list[dev_i]));
		printf("    Node type: %d (-1: UNKNOWN, 1: CA, 4: RNIC)\n",
			dev_list[dev_i]->node_type);
		printf("    Transport type: %d (-1: UNKNOWN, 0: IB, 1: IWARP)\n",
			dev_list[dev_i]->transport_type);

		printf("    fw: %s\n", device_attr.fw_ver);
		printf("    max_qp: %d\n", device_attr.max_qp);
		printf("    max_cq: %d\n", device_attr.max_cq);
		printf("    max_mr: %d\n", device_attr.max_mr);
		printf("    max_pd: %d\n", device_attr.max_pd);
		printf("    max_ah: %d\n", device_attr.max_ah);
		printf("    phys_port_cnt: %hu\n", device_attr.phys_port_cnt);
	}
}

struct hrd_ctrl_blk *hrd_init_ctrl_blk(int id, int port_index, int node_id)
{
	// XXX: HRD_SS_WINDOW has nothing to do with the total number of
	// requests in flight. Maybe take as argument to hrd_init_ctrl_blk??
	hrd_red_printf("HRD_SS_WINDOW is set to %d. Maximum clients = %d\n",
		HRD_SS_WINDOW, HRD_Q_DEPTH / HRD_SS_WINDOW);

	struct hrd_ctrl_blk *cb = malloc(sizeof(struct hrd_ctrl_blk));
	cb->id = id;
	cb->port_index = port_index;
	cb->device_id = -1;
	cb->port_id = -1;
	cb->node_id = node_id;

	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;

	int num_devices = 0;
	dev_list = ibv_get_device_list(&num_devices);
	CPE(!dev_list, "Failed to get IB devices list", 0);

	int ports_to_discover = port_index;

	int device_id;

	/* Find the port with 0-based rank = ports_to_discover */
	for (device_id = 0; device_id < num_devices; device_id++) {

		struct ibv_context *context = ibv_open_device(dev_list[device_id]);
		CPE(!context, "Couldn't get context", 0);

		struct ibv_device_attr device_attr;
		memset(&device_attr, 0, sizeof(device_attr));
		if (ibv_query_device(context, &device_attr)) {
			printf("Could not query device: %d\n", device_id);
			assert(false);
			return NULL;
		}

		uint8_t port_id;
		for (port_id = 1; port_id <= device_attr.phys_port_cnt; port_id++) {
			if (ports_to_discover == 0) {
				printf("HRD: ctrl_blk %d "
					"port index %d device %d (guid = %016lx) port %d\n", 
					cb->id, port_index, device_id, 
					ibv_get_device_guid(dev_list[device_id]), port_id);
				cb->device_id = device_id;
				cb->port_id = port_id;
			}

			ports_to_discover--;
		}

		if (ibv_close_device(context)) {
			fprintf(stderr, "Couldn't release context\n");
			assert(false);
			return NULL;
		}
	}

	/* Found no suitable port */
	if (cb->device_id == -1) {
		printf("invalid port index: %d\n", port_index);
		assert(false);
		return NULL;
	}

	ib_dev = dev_list[cb->device_id];
	CPE(!ib_dev, "IB device not found", 0);

	/* Zero-out some fields */
	cb->recv_head = 0, cb->recv_tail = 0;
	cb->send_count = 0;
	cb->recv_area = NULL;
	cb->recv_area_mr = NULL;
	cb->remote_qp_attrs = NULL;

	cb->context = ibv_open_device(ib_dev);
	CPE(!cb->context, "Couldn't get context", 0);

	/* Create a protection domain */
	cb->pd = ibv_alloc_pd(cb->context);
	CPE(!cb->pd, "Couldn't allocate PD", 0);

	/* Allocate and register the receive area */
	int ib_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
				IBV_ACCESS_REMOTE_WRITE;
	int shm_flags = IPC_CREAT | 0666 | SHM_HUGETLB;

	/*
	 * Find the minimum number of hugepages required for registering as many
	 * mbufs as the datagram recv queue depth.
	 */
	int reg_size = M_2;
	while(reg_size < HRD_Q_DEPTH * sizeof(struct hrd_mbuf)) {
		reg_size += M_2;
	}
	printf("Id %d: registering recv area of size %d bytes\n",
		cb->id, reg_size);

	int sid = shmget(HRD_BASE_SHM_KEY + cb->id, reg_size, shm_flags);
	assert(sid >= 0);
	cb->recv_area = shmat(sid, 0, 0);
	assert(cb->recv_area != NULL);

	cb->recv_area_mr = ibv_reg_mr(cb->pd,
		(char *) cb->recv_area, reg_size, ib_flags);
	assert(cb->recv_area_mr != NULL);

	/* Create qp and transition it to RTS */
	hrd_create_qp(cb);
	hrd_modify_qp_to_rts(cb);

	/* Store the qp's attrs */
	cb->local_qp_attrs.lid = hrd_get_local_lid(cb->context, cb->port_id);
	cb->local_qp_attrs.qpn = cb->qp->qp_num;

	/*
	 * Allocate space for address handles for all possible CAs in the cluster.
	 * ibv_create_ah() will be called lazily on TX.
	 */
	cb->ah = malloc(HRD_MAX_LID * sizeof(void *));
	memset(cb->ah, 0, HRD_MAX_LID * sizeof(void *));

	/* Create an array in cb for holding work completions */
	cb->wc = malloc(HRD_Q_DEPTH * sizeof(struct ibv_wc));

	/* Fill the recv qp so that we're ready to process recvs */
	hrd_post_recv(cb, HRD_Q_DEPTH);

	return cb;
}

uint16_t hrd_rx_burst(struct hrd_ctrl_blk *cb, struct hrd_mbuf **rx_pkts,
	uint16_t nb_pkts)
{
	int i;

	nb_pkts = ibv_poll_cq(cb->recv_cq, nb_pkts, cb->wc);
	for(i = 0; i < nb_pkts; i ++) {
		CPE(cb->wc[i].status != IBV_WC_SUCCESS, "rx_burst poll_cq fail!\n", -1);

		rx_pkts[i] = (struct hrd_mbuf*)
			&cb->recv_area[cb->recv_tail & HRD_Q_DEPTH_];
		cb->recv_tail ++;		/* long long: don't care about overflow */

		/* Add route information */
		rx_pkts[i]->s_qpn = cb->wc[i].src_qp;
		rx_pkts[i]->s_lid = cb->wc[i].slid;
	}

	hrd_post_recv(cb, nb_pkts);

	return nb_pkts;
}

void hrd_tx_burst(struct hrd_ctrl_blk *cb, struct hrd_mbuf **tx_pkts,
	uint16_t nb_pkts)
{
	int i, ret;
	int d_lid, d_qpn, len;

	struct ibv_send_wr *bad_send_wr;

	cb->wr.opcode = IBV_WR_SEND;
	cb->wr.num_sge = 1;
	cb->wr.next = NULL;
	cb->wr.sg_list = &cb->sgl;

	for(i = 0; i < nb_pkts; i ++) {
		d_lid = tx_pkts[i]->d_lid;
		d_qpn = tx_pkts[i]->d_qpn;

		if (cb->ah[d_lid] == NULL) {
			/*
			 * XXX: this may have a race condition; use only one thread for an 
			 * instance of struct hrd_ctrl_blk!
			 */
			printf("First time using lid %d; creating ah\n", d_lid);
			struct ibv_ah_attr ah_attr = {
				.is_global		= 0,
				.dlid 			= d_lid,
				.sl				= 0,
				.src_path_bits	= 0,
				.port_num		= cb->port_id,
			};
			cb->ah[d_lid] = ibv_create_ah(cb->pd, &ah_attr);
			CPE(!cb->ah[d_lid], "Failed to create ah", d_lid);
		}
		
		cb->wr.wr.ud.ah = cb->ah[d_lid];
		cb->wr.wr.ud.remote_qpn = d_qpn;
		cb->wr.wr.ud.remote_qkey = 0x11111111;

		/*
		 * At the beginning of your window, post a signaled SEND. At the  end
		 * of the window, poll for its completion.
		 */
		cb->wr.send_flags = (cb->send_count & HRD_SS_WINDOW_) == 0 ?
			IBV_SEND_INLINE | IBV_SEND_SIGNALED : IBV_SEND_INLINE;
		if((cb->send_count & HRD_SS_WINDOW_) == HRD_SS_WINDOW_) {
			hrd_poll_send_cq(cb, 1);
		}

		cb->sgl.addr = (uint64_t) (unsigned long) &tx_pkts[i]->data_len;

		len = tx_pkts[i]->data_len;
		CPE(len < 0 || len > HRD_MAX_DATA, "Invalid TX data length", -1);
		cb->wr.sg_list->length = len + HRD_PKTMBUF_METADATA;

		ret = ibv_post_send(cb->qp, &cb->wr, &bad_send_wr);
		CPE(ret, "ibv_post_send error", ret);

		cb->send_count ++;
	}
}

uint16_t hrd_get_local_lid(struct ibv_context *context, int port_id)
{
	struct ibv_port_attr attr;
	if (ibv_query_port(context, port_id, &attr)) {
		return 0;
	}

	assert(attr.lid < HRD_MAX_LID);

	return attr.lid;
}

int close_ctx(struct hrd_ctrl_blk *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy datagram QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->send_cq)) {
		fprintf(stderr, "Couldn't destroy datagarm CQ\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->recv_cq)) {
		fprintf(stderr, "Couldn't destroy datagarm CQ\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx);

	return 0;
}

void hrd_create_qp(struct hrd_ctrl_blk *ctx)
{
	ctx->send_cq = ibv_create_cq(ctx->context, HRD_Q_DEPTH, NULL, NULL, 0);
	CPE(!ctx->send_cq, "Couldn't create datagram send CQ", 0);

	ctx->recv_cq = ibv_create_cq(ctx->context, HRD_Q_DEPTH, NULL, NULL, 0);
	CPE(!ctx->recv_cq, "Couldn't create datagram recv CQ", 0);

	struct ibv_qp_init_attr init_attr = {
		.send_cq = ctx->send_cq,
		.recv_cq = ctx->recv_cq,
		.cap     = {
			.max_send_wr  = HRD_Q_DEPTH,
			.max_recv_wr  = HRD_Q_DEPTH,
			.max_send_sge = 1,
			.max_recv_sge = 1,
			.max_inline_data = 256
		},
		.qp_type = IBV_QPT_UD
	};

	ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
	CPE(!ctx->qp, "Couldn't create datagram QP", 0);
}

void hrd_modify_qp_to_rts(struct hrd_ctrl_blk *ctx)
{
	/* Transition to init state */
	struct ibv_qp_attr init_attr = {
		.qp_state		= IBV_QPS_INIT,
		.pkey_index		= 0,
		.port_num		= ctx->port_id,
		.qkey 			= 0x11111111
	};

	if (ibv_modify_qp(ctx->qp, &init_attr,
		IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return;
	}

	/* Transition to ready-to-receive */
	struct ibv_qp_attr rtr_attr = {
		.qp_state		= IBV_QPS_RTR,
	};
	
	if (ibv_modify_qp(ctx->qp, &rtr_attr, IBV_QP_STATE)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		exit(-1);
	}
	
	/* Re-use rtr_attr for transition to ready-to-send */
	rtr_attr.qp_state = IBV_QPS_RTS;
	rtr_attr.sq_psn = lrand48() & 0xffffff;
	
	if(ibv_modify_qp(ctx->qp, &rtr_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		exit(-1);
	}
}

/* Post a RECV to a UD QP */
void hrd_post_recv(struct hrd_ctrl_blk *cb, int num_recvs)
{
	int ret;
	struct ibv_sge list = {
		.length = sizeof(struct hrd_mbuf),
		.lkey	= cb->recv_area_mr->lkey
	};

	struct ibv_recv_wr recv_wr = {
		.sg_list    = &list,
		.num_sge    = 1,
	};

	struct ibv_recv_wr *bad_wr;
	int i;
	for(i = 0; i < num_recvs; i++) {
		int req_area_index = (int) (cb->recv_head & HRD_Q_DEPTH_);
		recv_wr.sg_list->addr = (uintptr_t) cb->recv_area[req_area_index].grh;

		ret = ibv_post_recv(cb->qp, &recv_wr, &bad_wr);
		if(ret) {
			fprintf(stderr, "Error %d posting recv.\n", ret);
			exit(0);
		}

		cb->recv_head ++;		// long long: don't care about overflow
	}
}

void print_qp_attr(struct hrd_qp_attr dest)
{
	fflush(stdout);
	fprintf(stderr, "\tLID: %d QPN: %d\n", dest.lid, dest.qpn);
}

inline long long hrd_get_cycles()
{
	unsigned low, high;
	unsigned long long val;
	asm volatile ("rdtsc" : "=a" (low), "=d" (high));
	val = high;
	val = (val << 32) | low;
	return val;
}

void hrd_nano_sleep(int ns)
{
	long long start = hrd_get_cycles();
	long long end = start;
	int upp = (int) (2.1 * ns);
	while(end - start < upp) {
		end = hrd_get_cycles();
	}
}

/* Poll the CQ of this cb */
void hrd_poll_send_cq(struct hrd_ctrl_blk *cb, int num_comps)
{
	struct ibv_wc wc;
	int comps= 0;
	while(comps < num_comps) {
		int new_comps = ibv_poll_cq(cb->send_cq, 1, &wc);
		if(new_comps != 0) {
			comps += new_comps;
			if(wc.status != 0) {
				fprintf(stderr, "Bad wc status %d\n", wc.status);
				exit(0);
			}
		}
	}
}

/* Like printf, but red. Limited to 1000 characters. */
void hrd_red_printf(const char *format, ...)
{	
	#define RED_LIM 1000
	va_list args;
	int i;

	char buf1[RED_LIM], buf2[RED_LIM];
	memset(buf1, 0, RED_LIM);
	memset(buf2, 0, RED_LIM);

    va_start(args, format);

	/* Marshal the stuff to print in a buffer */
	vsnprintf(buf1, RED_LIM, format, args);

	/* Probably a bad check for buffer overflow */
	for(i = RED_LIM - 1; i >= RED_LIM - 50; i --) {
		assert(buf1[i] == 0);
	}

	/* Add markers for red color and reset color */
	snprintf(buf2, 1000, "\033[31m%s\033[0m", buf1);

	/* Probably another bad check for buffer overflow */
	for(i = RED_LIM - 1; i >= RED_LIM - 50; i --) {
		assert(buf2[i] == 0);
	}

	printf("%s", buf2);

    va_end(args);
}

void hrd_register_qp(struct hrd_ctrl_blk *cb) 
{
	assert(cb != NULL);

	memcached_server_st *servers = NULL;
	memcached_st *memc = memcached_create(NULL);
	memcached_return rc;

	char *key = "rdma_dpdk_servers";
	char *value = (char *) &cb->local_qp_attrs;

	printf("HRD: Id %d: Registering qp_attr: ", cb->id);
	print_qp_attr(cb->local_qp_attrs);

	memc = memcached_create(NULL);
	char *registry_ip = hrd_getenv("HRD_REGISTRY_IP");
	
	/* We run the memcached server on the default memcached port */
	servers = memcached_server_list_append(servers,
		registry_ip, MEMCACHED_DEFAULT_PORT, &rc);
	rc = memcached_server_push(memc, servers);

	CPE(rc != MEMCACHED_SUCCESS, "Couldn't add memcached server.\n", -1);

	rc = memcached_append(memc, key, strlen(key), value, 
		sizeof(struct hrd_qp_attr), (time_t) 0, (uint32_t) 0);

	if (rc == MEMCACHED_SUCCESS) {
		printf("HRD: Id %d: qp_attr registered successfully\n", cb->id);
	} else {
		printf("HRD: Id %d: Couldn't store qp_attr: %s\n", cb->id,
			memcached_strerror(memc, rc));
		exit(-1);
	}

	/* Cleanup on clean exit */
	memcached_quit(memc);
	memcached_server_list_free(servers);
}

int hrd_get_registered_qps(struct hrd_ctrl_blk *cb)
{
	memcached_server_st *servers = NULL;
	memcached_st *memc = memcached_create(NULL);
	memcached_return rc;

	char *key = "rdma_dpdk_servers";
	size_t value_length;
	uint32_t flags;

	int i, num_qps;

	printf("HRD: Id %d: Fetching registered qps\n", cb->id);
	memc = memcached_create(NULL);
	char *registry_ip = hrd_getenv("HRD_REGISTRY_IP");

	/* We run the server on the default memcached port: 11211 */
	servers = memcached_server_list_append(servers, 
		registry_ip, MEMCACHED_DEFAULT_PORT, &rc);
	rc = memcached_server_push(memc, servers);

	cb->remote_qp_attrs = NULL;
	cb->remote_qp_attrs = (struct hrd_qp_attr *) memcached_get(memc, 
		key, strlen(key), &value_length, &flags, &rc);

	if (rc == MEMCACHED_SUCCESS) {
		num_qps = (int) value_length / sizeof(struct hrd_qp_attr);
		printf("HRD: Id %d: Found %d registered qps:\n", cb->id, num_qps);

		for(i = 0; i < num_qps; i ++) {
			print_qp_attr(cb->remote_qp_attrs[i]);
		}
		
		/* Cleanup on clean exit */
		memcached_quit(memc);
		memcached_server_list_free(servers);

	} else {
		/* No need to free memcached resources because we're dead */
		printf("HRD: Id %d: Couldn't find registered qps: %s\n", cb->id,
			memcached_strerror(memc, rc));
		exit(-1);
	}

	return num_qps;
}

/* Return an environment variable if it is set */
char *hrd_getenv(const char *name)
{
	char *env = getenv(name);
	if(env == NULL) {
		fprintf(stderr, "Environment variable %s not set\n", name);
		exit(-1);
	}

	return env;
}

