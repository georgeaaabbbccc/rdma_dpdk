#include "hrd.h"

#define MAX_BURST_SIZE 16
#define WS_CLIENT 16
#define WS_CLIENT_ 15

void *run_server(void *arg)
{
	int i;
	int tid = *(int *) arg;
	struct hrd_ctrl_blk *cb = hrd_init_ctrl_blk(tid, 0, 0);
	hrd_register_qp(cb);

	struct timespec start, end;
	long long cur_send_count = 0, prev_send_count = 0;
	int num_poll_cq_calls = 0, num_polls;

	struct hrd_mbuf *rx_pkts_burst[MAX_BURST_SIZE];

	clock_gettime(CLOCK_REALTIME, &start);

	hrd_red_printf("Running server thread %d\n", tid);

	int tries = 0;
	while(1) {
		num_poll_cq_calls ++;
		num_polls = 0;
		while(num_polls == 0) {
			num_polls = hrd_rx_burst(cb, rx_pkts_burst, MAX_BURST_SIZE);
			tries ++;
			if(tries >= 100000000) {
				printf("Thread %d waiting for RX. "
					"recv_head = %lld, recv_tail = %lld, send_count = %lld\n",
					tid, cb->recv_head, cb->recv_tail, cb->send_count);
				tries = 0;
			}
		}
		
		for(i = 0; i < num_polls; i ++) {
			tries = 0;

			/* Send the packet back to where it came from */
			rx_pkts_burst[i]->d_lid = rx_pkts_burst[i]->s_lid;
			rx_pkts_burst[i]->d_qpn = rx_pkts_burst[i]->s_qpn;

			uint8_t *pkt_data = hrd_pktmbuf_mtod(rx_pkts_burst[i]);
			pkt_data[0] ++;
		}

		hrd_tx_burst(cb, rx_pkts_burst, num_polls);
		cur_send_count += num_polls;

		if(cur_send_count - prev_send_count >= M_1) {
			clock_gettime(CLOCK_REALTIME, &end);

			int sends_done = cur_send_count - prev_send_count;
			double seconds = (end.tv_sec - start.tv_sec) +
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
			fprintf(stderr, "Thread %d: IOPS: %f, avg. rx batch = %d\n", tid, 
				sends_done / seconds, sends_done / num_poll_cq_calls);

			clock_gettime(CLOCK_REALTIME, &start);
			prev_send_count = cur_send_count;
			num_poll_cq_calls = 0;
		}
	}
}

void *run_client(void *arg)
{
	int tid = *(int *) arg;
	struct hrd_ctrl_blk *cb = hrd_init_ctrl_blk(tid, 0, 0);
	cb->num_remote_qps = hrd_get_registered_qps(cb);

	struct timespec start, end;		//Sampled once every ITERS_PER_MEASUREMENT
	struct timespec op_start[WS_CLIENT], op_end[WS_CLIENT];
	long long total_nsec = 0;

	int num_resp = 0, num_req = 0, num_req_ = 0, sn;
	struct hrd_mbuf *tx_pkt, *rx_pkt;

	/* Initialize the tx and rx mbufs */
	tx_pkt = malloc(sizeof(struct hrd_mbuf));
	rx_pkt = malloc(sizeof(struct hrd_mbuf));

	hrd_red_printf("Starting client thread %d.\n", tid);
	clock_gettime(CLOCK_REALTIME, &start);

	while(1) {
		/* Performance measurement */
		if((num_req & M_1_) == M_1_ && num_req != 0) {
			fprintf(stderr, "\nThread %d completed %d requests\n", tid, num_req);
			clock_gettime(CLOCK_REALTIME, &end);
			double seconds = (end.tv_sec - start.tv_sec) +
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;

			fprintf(stderr, "IOPS = %f, seconds = %f\n", M_1 / seconds, seconds);
			
			double sgl_read_time = (double) total_nsec / M_1;
			fprintf(stderr, "Average op time = %f us\n", sgl_read_time / 1000);
			total_nsec = 0;

			clock_gettime(CLOCK_REALTIME, &start);
		}

		num_req_ = num_req & WS_CLIENT_;
		sn = num_req % cb->num_remote_qps;

		clock_gettime(CLOCK_REALTIME, &op_start[num_req_]);
		
		assert(cb->remote_qp_attrs != NULL);
		tx_pkt->d_lid = cb->remote_qp_attrs[sn].lid;
		tx_pkt->d_qpn = cb->remote_qp_attrs[sn].qpn;

		tx_pkt->data_len = 16;

		hrd_tx_burst(cb, &tx_pkt, 1);

		num_req ++;

		if(num_req - num_resp == WS_CLIENT) {
			int rws = num_resp & WS_CLIENT_;	/* Resp. window slot */

			/* Poll for a recv */
			int recv_comps = 0, tries = 0;
			while(recv_comps == 0) {
				recv_comps = hrd_rx_burst(cb, &rx_pkt, 1);
				tries ++;
				if(tries >= 100000000) {
					printf("Thread %d waiting for RX. "
						"recv_head = %lld, recv_tail = %lld, send_count = %lld\n",
						tid, cb->recv_head, cb->recv_tail, cb->send_count);
					tries = 0;
				}
			}

			/* printf("RX on qp %d\n", qp_i); */

			clock_gettime(CLOCK_REALTIME, &op_end[rws]);
			total_nsec += (op_end[rws].tv_sec - op_start[rws].tv_sec)* 1000000000 
				+ (op_end[rws].tv_nsec - op_start[rws].tv_nsec);
			num_resp ++;
		}
	}
}

int main(int argc, char *argv[])
{
	int i, is_client, num_threads;
	pthread_t *thread;
	int *tid;

	hrd_ibv_devinfo();

	srand48(getpid() * time(NULL));		/* Required for PSN */
	
	num_threads = atoi(argv[1]);

	if(argc == 2) {
		is_client = 1;
	} else {
		is_client = 0;
	}

	printf("Using %d threads\n", num_threads);
	thread = malloc(num_threads * sizeof(pthread_t));
	tid = malloc(num_threads * sizeof(int));

	for(i = 0; i < num_threads; i ++) {
		tid[i] = i;

		if(is_client) {
			pthread_create(&thread[i], NULL, run_client, &tid[i]);
		} else {
			pthread_create(&thread[i], NULL, run_server, &tid[i]);
		}
	}

	for(i = 0; i < num_threads; i ++) {
		pthread_join(thread[i], NULL);
		hrd_red_printf("Join successful\n");
	}

	return 0;
}
