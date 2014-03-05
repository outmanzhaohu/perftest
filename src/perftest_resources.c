#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>

#include "perftest_resources.h"
#include "config.h"

#ifdef HAVE_VERBS_EXP
static enum ibv_exp_wr_opcode opcode_verbs_array[] = {IBV_EXP_WR_SEND,IBV_EXP_WR_RDMA_WRITE,IBV_EXP_WR_RDMA_READ};
static enum ibv_exp_wr_opcode opcode_atomic_array[] = {IBV_EXP_WR_ATOMIC_CMP_AND_SWP,IBV_EXP_WR_ATOMIC_FETCH_AND_ADD};
#else
static enum ibv_wr_opcode opcode_verbs_array[] = {IBV_WR_SEND,IBV_WR_RDMA_WRITE,IBV_WR_RDMA_READ};
static enum ibv_wr_opcode opcode_atomic_array[] = {IBV_WR_ATOMIC_CMP_AND_SWP,IBV_WR_ATOMIC_FETCH_AND_ADD};
#endif

struct perftest_parameters* duration_param;
int cycle_buffer=4096;

/******************************************************************************
 * Beginning
 ******************************************************************************/
static int check_for_contig_pages_support(struct ibv_context *context)
{

	int answer;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		fprintf(stderr, "Couldn't get device attributes\n");
		return FAILURE;
	}

	/*
	 * We assume device driver support contig pages by enabling 23 bit in
	 * device_cap_flag. this is defined as IBV_DEVICE_MR_ALLOCATE.
	 * Warning: this bit can represent others things in different devices.
	 */
	answer = attr.device_cap_flags & (1 << 23) ? SUCCESS : FAILURE;
	return answer;
}

#ifdef HAVE_XRCD
/******************************************************************************
 *
 ******************************************************************************/
static int ctx_xrcd_create(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	char *tmp_file_name;
	struct ibv_xrcd_init_attr xrcd_init_attr;

	memset(&xrcd_init_attr , 0 , sizeof xrcd_init_attr);

	tmp_file_name = (user_param->machine == SERVER) ? SERVER_FD : CLIENT_FD;

	ctx->fd = open(tmp_file_name, O_RDONLY | O_CREAT, S_IRUSR | S_IRGRP);
	if (ctx->fd < 0) {
		fprintf(stderr,"Error opening file %s errno: %s", tmp_file_name,strerror(errno));
		return FAILURE;
	}

	xrcd_init_attr.comp_mask = IBV_XRCD_INIT_ATTR_FD | IBV_XRCD_INIT_ATTR_OFLAGS;
	xrcd_init_attr.fd = ctx->fd;
	xrcd_init_attr.oflags = O_CREAT ;

	ctx->xrc_domain = ibv_open_xrcd(ctx->context,&xrcd_init_attr);
	if (ctx->xrc_domain == NULL) {
		fprintf(stderr,"Error opening XRC domain\n");
		return FAILURE;
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_xrc_srq_create(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	struct ibv_srq_init_attr_ex srq_init_attr;

	memset(&srq_init_attr, 0, sizeof(srq_init_attr));

	srq_init_attr.attr.max_wr = user_param->rx_depth;
	srq_init_attr.attr.max_sge = 1;
	srq_init_attr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_XRCD | IBV_SRQ_INIT_ATTR_CQ | IBV_SRQ_INIT_ATTR_PD;
	srq_init_attr.srq_type = IBV_SRQT_XRC;
	srq_init_attr.xrcd = ctx->xrc_domain;

	if(user_param->verb == SEND)
		srq_init_attr.cq = ctx->recv_cq;
	else
		srq_init_attr.cq = ctx->send_cq;

	srq_init_attr.pd = ctx->pd;
	ctx->srq = ibv_create_srq_ex(ctx->context,&srq_init_attr);
	if (ctx->srq == NULL) {
		fprintf(stderr," Couldn't open XRC SRQ\n");
		return FAILURE;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static struct ibv_qp *ctx_xrc_qp_create(struct pingpong_context *ctx,struct perftest_parameters *user_param,int qp_index)
{
	struct ibv_qp* qp = NULL;
	struct ibv_qp_init_attr_ex qp_init_attr;
	int num_of_qps = user_param->num_of_qps / 2;

	memset(&qp_init_attr, 0, sizeof(qp_init_attr));

	if ( (!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
						|| ((user_param->duplex || user_param->tst == LAT) && (qp_index >= num_of_qps))) {
		qp_init_attr.qp_type = IBV_QPT_XRC_RECV;
		qp_init_attr.comp_mask = IBV_QP_INIT_ATTR_XRCD;
		qp_init_attr.xrcd = ctx->xrc_domain;
		qp_init_attr.cap.max_recv_wr  = user_param->rx_depth;
		qp_init_attr.cap.max_recv_sge = 1;
		qp_init_attr.cap.max_inline_data = user_param->inline_size;

	} else {
		qp_init_attr.qp_type = IBV_QPT_XRC_SEND;
		qp_init_attr.send_cq = ctx->send_cq;
		qp_init_attr.cap.max_send_wr = user_param->tx_depth;
		qp_init_attr.cap.max_send_sge = 1;
		qp_init_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
		qp_init_attr.pd = ctx->pd;
		qp_init_attr.cap.max_inline_data = user_param->inline_size;
	}

	qp = ibv_create_qp_ex(ctx->context,&qp_init_attr);
	return qp;
}
#endif

#ifdef HAVE_DC
/******************************************************************************
 *
 ******************************************************************************/
static struct ibv_qp *ctx_dc_qp_create(struct pingpong_context *ctx,struct perftest_parameters *user_param,int qp_index)
{
	struct ibv_qp_init_attr_ex qp_init_attr;
	struct ibv_qp* qp = NULL;

	memset(&qp_init_attr, 0, sizeof(struct ibv_qp_init_attr_ex));

	qp_init_attr.send_cq = ctx->send_cq;
	qp_init_attr.recv_cq = (user_param->verb == SEND) ? ctx->recv_cq : ctx->send_cq;
	qp_init_attr.cap.max_inline_data = user_param->inline_size;
	qp_init_attr.pd = ctx->pd;
	qp_init_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
#ifdef HAVE_VERBS_EXP
	qp_init_attr.qp_type = IBV_EXP_QPT_DC_INI;
#else
	qp_init_attr.qp_type = IBV_QPT_DC_INI;
#endif
	qp_init_attr.srq = NULL;
	qp_init_attr.cap.max_send_wr  = user_param->tx_depth;
	qp_init_attr.cap.max_send_sge = MAX_SEND_SGE;

	qp = ibv_create_qp_ex(ctx->context,&qp_init_attr);
	return qp;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_dc_tgt_create(struct pingpong_context *ctx,struct perftest_parameters *user_param,int dct_index)
{
	struct ibv_exp_device_attr dattr;
	int err;
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;
	int port_num;

	//in dc with bidirectional,
	//there are send qps and recv qps. the actual number of send/recv qps
	//is num_of_qps / 2.
	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	// first half of qps are for ib_port and second half are for ib_port2
	// in dc with bidirectional, the first half of qps are dc_ini qps and
	// the second half are dc_tgts . the first half of the send/recv qps
	// are for ib_port1 and the second half are for ib_port2
	if (user_param->dualport == ON && (dct_index % num_of_qps >= num_of_qps_per_port))
		port_num = user_param->ib_port2;
	else
		port_num = user_param->ib_port;

	dattr.comp_mask = IBV_EXP_DEVICE_ATTR_FLAGS2 |
		IBV_EXP_DEVICE_DC_RD_REQ |
		IBV_EXP_DEVICE_DC_RD_RES;

	err = ibv_exp_query_device(ctx->context, &dattr);
	if (err) {
		printf("couldn't query device extended attributes\n");
		return -1;
	} else {
		if (!(dattr.comp_mask & IBV_EXP_DEVICE_ATTR_FLAGS2)) {
			printf("no extended capability flgas\n");
			return -1;
		}
		if (!(dattr.device_cap_flags2 & IBV_EXP_DEVICE_DC_TRANSPORT)) {
			printf("DC transport not enabled\n");
			return -1;
		}

		if (!(dattr.comp_mask & IBV_EXP_DEVICE_DC_RD_REQ)) {
			printf("no report on max requestor rdma/atomic resources\n");
			return -1;
		}

		if (!(dattr.comp_mask & IBV_EXP_DEVICE_DC_RD_RES)) {
			printf("no report on max responder rdma/atomic resources\n");
			return -1;
		}
	}

	struct ibv_exp_dct_init_attr dctattr = {
		.pd = ctx->pd,
		.cq = (user_param->verb == SEND && (user_param->duplex || user_param->tst == LAT)) ? ctx->recv_cq : ctx->send_cq,
		.srq = ctx->srq,
		.dc_key = user_param->dct_key,
		.port = port_num,
		.access_flags = IBV_ACCESS_REMOTE_WRITE,
		.min_rnr_timer = 2,
		.tclass = 0,
		.flow_label = 0,
		.mtu = user_param->curr_mtu,
		.pkey_index = user_param->pkey_index,
		.gid_index = user_param->gid_index,
		.hop_limit = 1,
		.inline_size = user_param->inline_recv_size,
	};

	ctx->dct[dct_index] = ibv_exp_create_dct(ctx->context, &dctattr);
	if (!ctx->dct[dct_index]) {
		printf("create dct failed\n");
		return -1;
	}

	struct ibv_exp_dct_attr dcqattr;

	err = ibv_exp_query_dct(ctx->dct[dct_index], &dcqattr);
	if (err) {
		printf("query dct failed\n");
		return -1;
	} else if (dcqattr.dc_key != user_param->dct_key) {
		printf("queried dckry (0x%llx) is different then provided at create (0x%llx)\n",
			   (unsigned long long)dcqattr.dc_key,
			   (unsigned long long)user_param->dct_key);
		return -1;
	} else if (dcqattr.state != IBV_EXP_DCT_STATE_ACTIVE) {
		printf("state is not active %d\n", dcqattr.state);
		return -1;
	}

	return 0;
}
#endif

#ifdef HAVE_RSS_EXP
static struct ibv_qp *ctx_rss_eth_qp_create(struct pingpong_context *ctx,struct perftest_parameters *user_param,int qp_index)
{

	struct ibv_exp_qp_init_attr attr;
	struct ibv_qp* qp = NULL;

	memset(&attr, 0, sizeof(struct ibv_exp_qp_init_attr));

	attr.send_cq = ctx->send_cq;
	attr.recv_cq = ctx->recv_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_inline_data = user_param->inline_size;
	attr.cap.max_recv_wr  = user_param->rx_depth;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.qp_type = IBV_QPT_RAW_PACKET;
	attr.comp_mask = IBV_EXP_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_QPG;
	attr.pd = ctx->pd;

	if (qp_index == 0) { //rss parent
	#ifdef HAVE_VERBS_EXP
		attr.qpg.qpg_type = IBV_EXP_QPG_PARENT;
	#else
		attr.qpg.qpg_type = IBV_QPG_PARENT;
	#endif
		attr.qpg.qpg_parent = NULL;
		attr.qpg.parent_attrib.tss_child_count = 0;
		attr.qpg.parent_attrib.rss_child_count = 2;
	} else { //rss childs
	#ifdef HAVE_VERBS_EXP
		attr.qpg.qpg_type = IBV_EXP_QPG_CHILD_RX;
	#else
		attr.qpg.qpg_type = IBV_QPG_CHILD_RX;
	#endif 
		attr.qpg.qpg_parent = ctx->qp[0];
	}
	qp = ibv_exp_create_qp(ctx->context,&attr);


	return qp;
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int check_add_port(char **service,int port,
				   const char *servername,
				   struct addrinfo *hints,
				   struct addrinfo **res) {

	int number;

	if (asprintf(service,"%d", port) < 0) {
		return FAILURE;
	 }

	number = getaddrinfo(servername,*service,hints,res);

	if (number < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(number), servername, port);
		return FAILURE;
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_rdma_resources(struct pingpong_context *ctx,
						  struct perftest_parameters *user_param) {

	int is_udp_ps = user_param->connection_type == UD || user_param->connection_type == RawEth;

	enum rdma_port_space port_space = (is_udp_ps) ? RDMA_PS_UDP : RDMA_PS_TCP;

	ctx->cm_channel = rdma_create_event_channel();
	if (ctx->cm_channel == NULL) {
		fprintf(stderr, " rdma_create_event_channel failed\n");
		return FAILURE;
	}

	if (user_param->machine == CLIENT) {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id,NULL,port_space)) {
			fprintf(stderr,"rdma_create_id failed\n");
			return FAILURE;
		}

	} else {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id_control,NULL,port_space)) {
			fprintf(stderr,"rdma_create_id failed\n");
			return FAILURE;
		}

	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
struct ibv_device* ctx_find_dev(const char *ib_devname) {

	int num_of_device;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev = NULL;

	dev_list = ibv_get_device_list(&num_of_device);

	if (num_of_device <= 0) {
		fprintf(stderr," Did not detect devices \n");
		fprintf(stderr," If device exists, check if driver is up\n");
		return NULL;
	}

	if (!ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			exit (1);
		}
	} else {
		for (; (ib_dev = *dev_list); ++dev_list)
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		if (!ib_dev)
			fprintf(stderr, "IB device %s not found\n", ib_devname);
	}
	return ib_dev;
}

/******************************************************************************
 *
 ******************************************************************************/
void alloc_ctx(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int tarr_size;

	tarr_size = (user_param->noPeak) ? 1 : user_param->iters*user_param->num_of_qps;
	ALLOCATE(user_param->tposted,cycles_t,tarr_size);
	memset(user_param->tposted, 0, sizeof(cycles_t)*tarr_size);

	if (user_param->tst == LAT && user_param->test_type == DURATION)
		ALLOCATE(user_param->tcompleted,cycles_t,1);

	ALLOCATE(ctx->qp,struct ibv_qp*,user_param->num_of_qps);

	#ifdef HAVE_DC
	if (user_param->connection_type == DC) {
	#ifdef HAVE_VERBS_EXP
		ALLOCATE(ctx->dct,struct ibv_exp_dct*,user_param->num_of_qps);
	#else
		ALLOCATE(ctx->dct,struct ibv_dct*,user_param->num_of_qps);
	#endif
	}
	#endif

	if ((user_param->tst == BW ) && (user_param->machine == CLIENT || user_param->duplex)) {

		ALLOCATE(user_param->tcompleted,cycles_t,tarr_size);
		memset(user_param->tcompleted, 0, sizeof(cycles_t)*tarr_size);

		ALLOCATE(ctx->my_addr,uint64_t,user_param->num_of_qps);
		ALLOCATE(ctx->rem_addr,uint64_t,user_param->num_of_qps);
		ALLOCATE(ctx->scnt,int,user_param->num_of_qps);
		ALLOCATE(ctx->ccnt,int,user_param->num_of_qps);
		memset(ctx->scnt, 0, user_param->num_of_qps * sizeof (int));
		memset(ctx->ccnt, 0, user_param->num_of_qps * sizeof (int));

	} else if ((user_param->tst == BW ) && user_param->verb == SEND && user_param->machine == SERVER) {

		ALLOCATE(ctx->my_addr,uint64_t,user_param->num_of_qps);
		ALLOCATE(user_param->tcompleted,cycles_t,1);
	}

	if (user_param->machine == CLIENT || user_param->tst == LAT || user_param->duplex) {

		ALLOCATE(ctx->sge_list,struct ibv_sge,user_param->num_of_qps*user_param->post_list);
	#ifdef HAVE_VERBS_EXP
		ALLOCATE(ctx->wr,struct ibv_exp_send_wr,user_param->num_of_qps*user_param->post_list);
	#else
		ALLOCATE(ctx->wr,struct ibv_send_wr,user_param->num_of_qps*user_param->post_list);
	#endif
		if ((user_param->verb == SEND && user_param->connection_type == UD ) || user_param->connection_type == DC) {
			ALLOCATE(ctx->ah,struct ibv_ah*,user_param->num_of_qps);
		}
		if (user_param->connection_type == DC) {
		#ifdef HAVE_VERBS_EXP
			ALLOCATE(ctx->wr,struct ibv_exp_send_wr,user_param->num_of_qps*user_param->post_list);
		#else
			ALLOCATE(ctx->wr,struct ibv_send_wr,user_param->num_of_qps*user_param->post_list);
		#endif
		}
	}

	if (user_param->verb == SEND && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) {

		ALLOCATE(ctx->recv_sge_list,struct ibv_sge,user_param->num_of_qps);
		ALLOCATE(ctx->rwr,struct ibv_recv_wr,user_param->num_of_qps);
		ALLOCATE(ctx->rx_buffer_addr,uint64_t,user_param->num_of_qps);
	}
    if (user_param->mac_fwd == ON ) 
        cycle_buffer = user_param->size * user_param->rx_depth;

	ctx->size = user_param->size;
	ctx->buff_size = BUFF_SIZE(ctx->size)*2*user_param->num_of_qps;

    user_param->buff_size = ctx->buff_size;
	if (user_param->connection_type == UD)
		ctx->buff_size += CACHE_LINE_SIZE;
}

/******************************************************************************
 *
 ******************************************************************************/
int destroy_ctx(struct pingpong_context *ctx,
				struct perftest_parameters *user_param)  {

	int i, first;
	int test_result = 0;
	int num_of_qps = user_param->num_of_qps;

	//in dc with bidirectional,
	//there are send qps and recv qps. the actual number of send/recv qps
	//is num_of_qps / 2.
	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
	}
	/* RSS parent should be last */
	if (user_param->use_rss)
		first = 1;
	else
		first = 0;
	for (i = first; i < user_param->num_of_qps; i++) {

		if (( (user_param->connection_type == DC && !((!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
								|| ((user_param->duplex || user_param->tst == LAT) && (i >= num_of_qps)))) || 
										user_param->connection_type == UD) && (user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) {
			if (ibv_destroy_ah(ctx->ah[i])) {
				fprintf(stderr, "failed to destroy AH\n");
				test_result = 1;
			}
		}
		#ifdef HAVE_DC
		if (user_param->connection_type == DC &&   ((!(user_param->duplex || user_param->tst == LAT) 
			&& (user_param->machine == SERVER)) || ((user_param->duplex || user_param->tst == LAT) && (i >= num_of_qps)))) {
				if (ibv_exp_destroy_dct(ctx->dct[i])) {
					fprintf(stderr, "failed to destroy dct\n");
					test_result = 1;
				}
				if ( i == user_param->num_of_qps -1 )
					return test_result;
		} else
		#endif
		if (ibv_destroy_qp(ctx->qp[i])) {
			fprintf(stderr, " Couldn't destroy QP - %s\n",strerror(errno));
			test_result = 1;
		}
	}

	if (user_param->use_rss) {
		if (user_param->connection_type == UD && (user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) {
			if (ibv_destroy_ah(ctx->ah[0])) {
				fprintf(stderr, "failed to destroy AH\n");
				test_result = 1;
			}
		}

		if (ibv_destroy_qp(ctx->qp[0])) {
			fprintf(stderr, " Couldn't destroy QP - %s\n",strerror(errno));
			test_result = 1;
		}
	}
	if (user_param->srq_exists) {
		if (ibv_destroy_srq(ctx->srq)) {
			fprintf(stderr, "Couldn't destroy SRQ\n");
			test_result = 1;
		}
	}
#ifdef HAVE_XRCD

	if (user_param->use_xrc) {

		if (ibv_close_xrcd(ctx->xrc_domain)) {
			fprintf(stderr, "Couldn't destroy XRC domain\n");
			test_result = 1;
		}

		if (ctx->fd >= 0 && close(ctx->fd)) {
			fprintf(stderr, "Couldn't close the file for the XRC Domain\n");
			test_result = 1;
		}

	}

#endif

	if (ibv_destroy_cq(ctx->send_cq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}


	if (user_param->verb == SEND && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex || (ctx->channel)) ) {
		if (!(user_param->connection_type == DC && user_param->machine == SERVER)) {
			if (ibv_destroy_cq(ctx->recv_cq)) {
				fprintf(stderr, "failed to destroy CQ\n");
				test_result = 1;
			}
		}
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "failed to deregister MR\n");
		test_result = 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "failed to deallocate PD\n");
		test_result = 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "failed to close event channel\n");
			test_result = 1;
		}
	}

	if (user_param->work_rdma_cm == OFF) {

		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "failed to close device context\n");
			test_result = 1;
		}
	}

	if (ctx->is_contig_supported == FAILURE)
		free(ctx->buf);

	free(ctx->qp);

	if ((user_param->tst == BW ) && (user_param->machine == CLIENT || user_param->duplex)) {

		free(user_param->tposted);
		free(user_param->tcompleted);
		free(ctx->my_addr);
		free(ctx->rem_addr);
		free(ctx->scnt);
		free(ctx->ccnt);
	}
	else if ((user_param->tst == BW ) && user_param->verb == SEND && user_param->machine == SERVER) {

		free(user_param->tposted);
		free(user_param->tcompleted);
		free(ctx->my_addr);
	}

	if (user_param->machine == CLIENT || user_param->tst == LAT || user_param->duplex) {

		free(ctx->sge_list);
		free(ctx->wr);
	}

	if (user_param->verb == SEND && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) {

		free(ctx->rx_buffer_addr);
		free(ctx->recv_sge_list);
		free(ctx->rwr);
	}

	return test_result;
}

#ifdef HAVE_VERBS_EXP

static struct ibv_qp* ctx_qp_create_inline_recv(struct pingpong_context *ctx,
							 struct perftest_parameters *user_param) {

	struct ibv_exp_qp_init_attr attr;
	struct ibv_exp_device_attr dattr;
	struct ibv_qp* qp = NULL;
	int ret;

	memset(&attr, 0, sizeof(attr));
	memset(&dattr, 0, sizeof(dattr));

	dattr.comp_mask |= IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ;
	ret = ibv_exp_query_device(ctx->context, &dattr);
	if (ret) {
			printf("  Couldn't query device for inline-receive capabilities.\n");
		} else if (!(dattr.comp_mask & IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ)) {
			printf("  Inline-receive not supported by driver.\n");
		} else if (dattr.inline_recv_sz < user_param->inline_recv_size) {
			printf("  Max inline-receive(%d) < Requested inline-receive(%d).\n",
			       dattr.inline_recv_sz, user_param->inline_recv_size);
	}

	attr.send_cq = ctx->send_cq;
	attr.recv_cq = (user_param->verb == SEND) ? ctx->recv_cq : ctx->send_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_inline_data = user_param->inline_size;
	attr.max_inl_recv = user_param->inline_recv_size;
	attr.pd = ctx->pd;
	attr.comp_mask = IBV_EXP_QP_INIT_ATTR_PD;
	attr.comp_mask |= IBV_EXP_QP_INIT_ATTR_INL_RECV;

	if (user_param->use_srq && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) {
		attr.srq = ctx->srq;
		attr.cap.max_recv_wr  = 0;
		attr.cap.max_recv_sge = 0;
	} else {
		attr.srq = NULL;
		attr.cap.max_recv_wr  = user_param->rx_depth;
		attr.cap.max_recv_sge = MAX_RECV_SGE;
	}

	switch (user_param->connection_type) {

		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
#ifdef HAVE_XRCD
		case XRC : attr.qp_type = IBV_QPT_XRC; break;
#endif
#ifdef HAVE_RAW_ETH
		case RawEth : attr.qp_type = IBV_QPT_RAW_PACKET; break;
#endif
		default:  fprintf(stderr, "Unknown connection type \n");
			return NULL;
	}

	qp = ibv_exp_create_qp(ctx->context, &attr);

	if (user_param->inline_recv_size > attr.max_inl_recv)
			printf("  Actual inline-receive(%d) < requested inline-receive(%d)\n",
			       attr.max_inl_recv, user_param->inline_recv_size);

	return qp;
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int ctx_init(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int i;
	int flags = IBV_ACCESS_LOCAL_WRITE;
	int num_of_qps = user_param->num_of_qps / 2;
	int init_flag = 0;
	int only_dct = 0;
	int tx_buffer_depth = user_param->tx_depth;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_reg_mr_in reg_mr_exp_in;
	int exp_flags = 0;
	#endif

	if (user_param->connection_type == DC) {
		only_dct = (user_param->machine == SERVER && !(user_param->duplex || user_param->tst == LAT));
	}

	ctx->is_contig_supported  = check_for_contig_pages_support(ctx->context);

	// Allocating buffer for data, in case driver not support contig pages.
	if (ctx->is_contig_supported == FAILURE) {

		ctx->buf = memalign(sysconf(_SC_PAGESIZE),ctx->buff_size);
		if (!ctx->buf) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			exit(1);
		}

		memset(ctx->buf, 0,ctx->buff_size);

	} else {
		ctx->buf = NULL;
	#ifdef HAVE_VERBS_EXP
		exp_flags = IBV_EXP_ACCESS_ALLOCATE_MR;
	#else
		flags |= (1 << 5);
	#endif
	}

	// Allocating an event channel if requested.
	if (user_param->use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			return FAILURE;
		}
	}

	// Allocating the Protection domain.
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return FAILURE;
	}

	if (user_param->verb == WRITE) {
		flags |= IBV_ACCESS_REMOTE_WRITE;

	} else if (user_param->verb == READ) {
		flags |= IBV_ACCESS_REMOTE_READ;

	if (user_param->transport_type == IBV_TRANSPORT_IWARP)
		flags |= IBV_ACCESS_REMOTE_WRITE;

	} else if (user_param->verb == ATOMIC) {
		flags |= IBV_ACCESS_REMOTE_ATOMIC;
	}

	// Allocating Memory region and assigning our buffer to it.
#ifdef HAVE_VERBS_EXP
	reg_mr_exp_in.pd = ctx->pd;
	reg_mr_exp_in.addr = ctx->buf;
	reg_mr_exp_in.length = ctx->buff_size;
	reg_mr_exp_in.access = flags;
	reg_mr_exp_in.exp_access = exp_flags;
	if (ctx->is_contig_supported == SUCCESS)
		ctx->mr = ibv_exp_reg_mr(&reg_mr_exp_in);
	else
		ctx->mr = ibv_reg_mr(ctx->pd,ctx->buf,ctx->buff_size,flags);
#else
	ctx->mr = ibv_reg_mr(ctx->pd,ctx->buf,ctx->buff_size,flags);
#endif
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return FAILURE;
	}

	if (ctx->is_contig_supported == SUCCESS)
		ctx->buf = ctx->mr->addr;

	if (only_dct == 1)
		tx_buffer_depth = user_param->rx_depth;

	ctx->send_cq = ibv_create_cq(ctx->context,tx_buffer_depth*user_param->num_of_qps,NULL,ctx->channel,0);
	if (!ctx->send_cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return FAILURE;
	}
	if ((user_param->connection_type == DC && only_dct == 0) || ((user_param->verb == SEND && user_param->connection_type != DC))) {
		ctx->recv_cq = ibv_create_cq(ctx->context,user_param->rx_depth*user_param->num_of_qps,NULL,ctx->channel,0);
		if (!ctx->recv_cq) {
			fprintf(stderr, "Couldn't create a receiver CQ\n");
			return FAILURE;
		}
	}

#ifdef HAVE_XRCD
	if (user_param->use_xrc) {

		if (ctx_xrcd_create(ctx,user_param)) {
			fprintf(stderr, "Couldn't create XRC resources\n");
			return FAILURE;
		}

		if (ctx_xrc_srq_create(ctx,user_param)) {
			fprintf(stderr, "Couldn't create SRQ XRC resources\n");
			return FAILURE;
		}
	}
#endif

	if (user_param->use_srq && !user_param->use_xrc && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) {

		struct ibv_srq_init_attr attr = {
				.attr = {
					//when using sreq, rx_depth sets the max_wr
					.max_wr  = user_param->rx_depth,
					.max_sge = 1
				}
		};

		ctx->srq = ibv_create_srq(ctx->pd, &attr);
		if (!ctx->srq)  {
				fprintf(stderr, "Couldn't create SRQ\n");
				return FAILURE;
		}
	}
#ifdef HAVE_RSS_EXP
	if (user_param->use_rss) {
		struct ibv_exp_device_attr attr;

		attr.comp_mask = IBV_EXP_DEVICE_ATTR_FLAGS2 |
				 IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ;
		if (ibv_exp_query_device(ctx->context, &attr)) {
			fprintf(stderr, "Experimental ibv_exp_query_device.\n");
			exit(1);
		}

		if (!((attr.device_cap_flags2 & IBV_EXP_DEVICE_QPG) &&
		      (attr.device_cap_flags2 & IBV_EXP_DEVICE_UD_RSS) &&
		      (attr.comp_mask & IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ) &&
		      (attr.max_rss_tbl_sz > 0))) {
			fprintf(stderr, "RSS not supported .\n");
			exit(1);
		}

		/* num of qps includes the parent */
		if (user_param->num_of_qps > attr.max_rss_tbl_sz + 1) {
			fprintf(stderr, "RSS limit is %d .\n",
				attr.max_rss_tbl_sz);
			exit(1);
		}
	}
#endif

	for (i=0; i < user_param->num_of_qps; i++) {

		if(user_param->connection_type == DC) {
			#ifdef HAVE_DC
				if ( (!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
								|| ((user_param->duplex || user_param->tst == LAT) && (i >= num_of_qps))) {

					ctx_dc_tgt_create(ctx,user_param,i);
					//in order to not change anything in the test
					ALLOCATE(ctx->qp[i],struct ibv_qp,1);
					ctx->qp[i]->qp_num = ctx->dct[i]->dct_num;
				}
				else {
					ctx->qp[i] = ctx_dc_qp_create(ctx,user_param,i);
				}
			#endif
				if (ctx->qp[i] == NULL) {
					fprintf(stderr," Unable to create DC QP.\n");
					return FAILURE;
				}
		} else if (user_param->use_rss && user_param->connection_type == RawEth) {
			#ifdef HAVE_RSS_EXP
			ctx->qp[i] = ctx_rss_eth_qp_create(ctx,user_param,i);
			#endif
			if (ctx->qp[i] == NULL) {
				fprintf(stderr," Unable to create RSS QP.\n");
				return FAILURE;
			}
		} else if (user_param->use_xrc) {
			#ifdef HAVE_XRCD
				ctx->qp[i] = ctx_xrc_qp_create(ctx,user_param,i);
			#endif
				if (ctx->qp[i] == NULL) {
					fprintf(stderr," Unable to create XRC QP.\n");
					return FAILURE;
				}
		} else if (user_param->inline_recv_size) {
			#ifdef HAVE_VERBS_EXP
			ctx->qp[i] = ctx_qp_create_inline_recv(ctx,user_param);
			#endif
			if (ctx->qp[i] == NULL) {
				fprintf(stderr," Unable to create Inline Recv QP.\n");
				return FAILURE;
			}
		} else {
			ctx->qp[i] = ctx_qp_create(ctx,user_param);
			if (ctx->qp[i] == NULL) {
				fprintf(stderr," Unable to create QP.\n");
				return FAILURE;
			}
		}

		if (user_param->work_rdma_cm == OFF) {
			#ifdef HAVE_RSS_EXP
			if (i == 0 && user_param->use_rss) {
			#ifdef HAVE_VERBS_EXP
				init_flag = IBV_EXP_QP_GROUP_RSS;
			#else
				init_flag = IBV_QP_GROUP_RSS;
			#endif
			}
			else
			#endif
				init_flag = 0;
			if(user_param->connection_type == DC) {
				if ( !((!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
								|| ((user_param->duplex || user_param->tst == LAT) && (i >= num_of_qps)))) {
				#ifdef HAVE_DC
					if (ctx_modify_dc_qp_to_init(ctx->qp[i],user_param)) {
						fprintf(stderr," Unable to create DC QP.\n");
						return FAILURE;
					}
				#endif
				}
			} else {
				if (ctx_modify_qp_to_init(ctx->qp[i],user_param,init_flag)) {
					fprintf(stderr, "Failed to modify QP to INIT\n");
					return FAILURE;
				}
			}
		}
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
struct ibv_qp* ctx_qp_create(struct pingpong_context *ctx,
							 struct perftest_parameters *user_param) {

	struct ibv_qp_init_attr attr;
	struct ibv_qp* qp = NULL;

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.send_cq = ctx->send_cq;
	attr.recv_cq = (user_param->verb == SEND) ? ctx->recv_cq : ctx->send_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_inline_data = user_param->inline_size;

	if (user_param->use_srq && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) {
		attr.srq = ctx->srq;
		attr.cap.max_recv_wr  = 0;
		attr.cap.max_recv_sge = 0;
	} else {
		attr.srq = NULL;
		attr.cap.max_recv_wr  = user_param->rx_depth;
		attr.cap.max_recv_sge = MAX_RECV_SGE;
	}

	switch (user_param->connection_type) {

		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
#ifdef HAVEE_XRCD
		case XRC : attr.qp_type = IBV_QPT_XRC; break;
#endif
#ifdef HAVE_RAW_ETH
		case RawEth : attr.qp_type = IBV_QPT_RAW_PACKET; break;
#endif
		default:  fprintf(stderr, "Unknown connection type \n");
			return NULL;
	}

	if (user_param->work_rdma_cm) {

		if (rdma_create_qp(ctx->cm_id,ctx->pd,&attr)) {
			fprintf(stderr, " Couldn't create rdma QP - %s\n",strerror(errno));
			return NULL;
		}
		qp = ctx->cm_id->qp;

	} else {
		qp = ibv_create_qp(ctx->pd,&attr);
	}
	return qp;
}

#ifdef HAVE_DC

 /******************************************************************************
 *
 ******************************************************************************/
int ctx_modify_dc_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param)  {

	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

#ifdef HAVE_VERBS_EXP
	struct ibv_exp_qp_attr attr;
#else
	struct ibv_qp_attr attr;
#endif

	int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

	static int portindex=0;  // for dual-port support

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = user_param->pkey_index;

	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	if (user_param->dualport==ON) {
		if (portindex % num_of_qps < num_of_qps_per_port) {
	        attr.port_num = user_param->ib_port;
		} else {
			attr.port_num = user_param->ib_port2;
		}
		portindex++;

	} else {

		attr.port_num = user_param->ib_port;
	}
	switch (user_param->verb) {
		case ATOMIC: attr.qp_access_flags = IBV_ACCESS_REMOTE_ATOMIC; break;
		case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
		case WRITE : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
		case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
	}
#ifdef HAVE_VERBS_EXP
	flags |= IBV_QP_ACCESS_FLAGS | IBV_EXP_QP_DC_KEY;
#else
	flags |= IBV_QP_ACCESS_FLAGS | IBV_QP_DC_KEY;
#endif
	attr.dct_key = user_param->dct_key;
#ifdef HAVE_VERBS_EXP
	attr.comp_mask = IBV_EXP_QP_ATTR_DCT_KEY;
#else
	attr.comp_mask = IBV_QP_ATTR_DCT_KEY;
#endif

#ifdef HAVE_VERBS_EXP
	if (ibv_exp_modify_qp(qp,&attr,flags)) {
#else
	if (ibv_modify_qp_ex(qp,&attr,flags)) {
#endif
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return 1;
	}
	return 0;
}
#endif

 /******************************************************************************
 *
 ******************************************************************************/
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param, int init_flag)  {

	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

	struct ibv_qp_attr attr;
	int flags = init_flag | IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

	static int portindex=0;  // for dual-port support

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = user_param->pkey_index;;

	if ( user_param->use_xrc && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	if (user_param->dualport==ON) {
		if (portindex % num_of_qps < num_of_qps_per_port) {
	        attr.port_num = user_param->ib_port;
		} else {
			attr.port_num = user_param->ib_port2;
		}
		portindex++;

	} else {

		attr.port_num = user_param->ib_port;
	}

	if (user_param->connection_type == RawEth) {
		flags = init_flag | IBV_QP_STATE | IBV_QP_PORT;

	} else if (user_param->connection_type == UD) {
		attr.qkey = DEFF_QKEY;
		flags |= IBV_QP_QKEY;

	} else {
		switch (user_param->verb) {
			case ATOMIC: attr.qp_access_flags = IBV_ACCESS_REMOTE_ATOMIC; break;
			case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
			case WRITE : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
			case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
		}
		flags |= IBV_QP_ACCESS_FLAGS;
	}

	if (ibv_modify_qp(qp,&attr,flags)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return 1;
	}
	return 0;
}

#ifdef HAVE_DC
/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_dc_qp_to_rtr(struct ibv_qp *qp,
								struct ibv_exp_qp_attr *attr,
								struct perftest_parameters *user_param,
								struct pingpong_dest *dest,
								struct pingpong_dest *my_dest,
								int qpindex) {

	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

	int flags = IBV_QP_STATE;
	attr->qp_state = IBV_QPS_RTR;
	attr->ah_attr.src_path_bits = 0;

	//in DC with bidirectional,
	//there are send qps and recv qps. the actual number of send/recv qps
	//is num_of_qps / 2.
	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	// first half of qps are for ib_port and second half are for ib_port2
	// in DC with bidirectional, the first half of qps are DC_INI qps and
	// the second half are DC_TGT qps. the first half of the send/recv qps
	// are for ib_port1 and the second half are for ib_port2
	if (user_param->dualport == ON && (qpindex % num_of_qps >= num_of_qps_per_port))
		attr->ah_attr.port_num = user_param->ib_port2;
	else
		attr->ah_attr.port_num = user_param->ib_port;

	attr->ah_attr.dlid = dest->lid;
	if (user_param->gid_index == DEF_GID_INDEX) {

		attr->ah_attr.is_global = 0;
		attr->ah_attr.sl = user_param->sl;

	} else {
		attr->ah_attr.is_global  = 1;
		attr->ah_attr.grh.dgid = dest->gid;
		attr->ah_attr.grh.sgid_index = user_param->gid_index;
		attr->ah_attr.grh.hop_limit = 1;
		attr->ah_attr.sl = 0;
	}

	flags |= IBV_QP_PATH_MTU | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	attr->max_dest_rd_atomic = my_dest->out_reads;
	attr->min_rnr_timer = 0;
	attr->dct_key = user_param->dct_key;
	attr->path_mtu = user_param->curr_mtu;

	return ibv_exp_modify_qp(qp,attr,flags);
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rtr(struct ibv_qp *qp,
								struct ibv_qp_attr *attr,
								struct perftest_parameters *user_param,
								struct pingpong_dest *dest,
								struct pingpong_dest *my_dest,
								int qpindex) {

	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

	int flags = IBV_QP_STATE;
	attr->qp_state = IBV_QPS_RTR;
	attr->ah_attr.src_path_bits = 0;

	//in xrc with bidirectional,
	//there are send qps and recv qps. the actual number of send/recv qps
	//is num_of_qps / 2.
	if ( user_param->use_xrc && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	// first half of qps are for ib_port and second half are for ib_port2
	// in xrc with bidirectional, the first half of qps are xrc_send qps and
	// the second half are xrc_recv qps. the first half of the send/recv qps
	// are for ib_port1 and the second half are for ib_port2
	if (user_param->dualport == ON && (qpindex % num_of_qps >= num_of_qps_per_port))
		attr->ah_attr.port_num = user_param->ib_port2;
	else
		attr->ah_attr.port_num = user_param->ib_port;

	if (user_param->connection_type != RawEth) {

		attr->ah_attr.dlid = dest->lid;
		if (user_param->gid_index == DEF_GID_INDEX) {

			attr->ah_attr.is_global = 0;
			attr->ah_attr.sl = user_param->sl;

		} else {

			attr->ah_attr.is_global  = 1;
			attr->ah_attr.grh.dgid = dest->gid;
			attr->ah_attr.grh.sgid_index = user_param->gid_index;
			attr->ah_attr.grh.hop_limit = 1;
			attr->ah_attr.sl = 0;
		}

		if (user_param->connection_type != UD) {

			attr->path_mtu = user_param->curr_mtu;
			attr->dest_qp_num = dest->qpn;
			attr->rq_psn = dest->psn;

			flags |= (IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);

			if (user_param->connection_type == RC || user_param->connection_type == XRC) {

				attr->max_dest_rd_atomic = my_dest->out_reads;
				attr->min_rnr_timer = 12;
				flags |= (IBV_QP_MIN_RNR_TIMER | IBV_QP_MAX_DEST_RD_ATOMIC);
			}
		}
	}
	else if (user_param->raw_qos){
		attr->ah_attr.sl = user_param->sl;
		flags |= IBV_QP_AV;
	}
	return ibv_modify_qp(qp,attr,flags);
}

#ifdef HAVE_DC
/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_dc_qp_to_rts(struct ibv_qp *qp,
							#ifdef HAVE_VERBS_EXP
								struct ibv_exp_qp_attr *attr,
							#else
								struct ibv_qp_attr_ex *attr,
							#endif
								struct perftest_parameters *user_param,
								struct pingpong_dest *dest,
								struct pingpong_dest *my_dest)
{

	int flags = IBV_QP_STATE;
	attr->qp_state = IBV_QPS_RTS;

	attr->timeout   = user_param->qp_timeout;
	attr->retry_cnt = 7;
	attr->rnr_retry = 7;
	attr->max_rd_atomic  = dest->out_reads;

	flags |= (IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
#ifdef HAVE_VERBS_EXP
	return ibv_exp_modify_qp(qp,attr,flags);
#else
	return ibv_modify_qp_ex(qp,attr,flags);
#endif
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rts(struct ibv_qp *qp,
								struct ibv_qp_attr *attr,
								struct perftest_parameters *user_param,
								struct pingpong_dest *dest,
								struct pingpong_dest *my_dest)
{

	int flags = IBV_QP_STATE;
	attr->qp_state = IBV_QPS_RTS;

	if (user_param->connection_type != RawEth) {

		flags |= IBV_QP_SQ_PSN;
		attr->sq_psn = my_dest->psn;

		if (user_param->connection_type == RC || user_param->connection_type == XRC) {

			attr->timeout   = user_param->qp_timeout;
			attr->retry_cnt = 7;
			attr->rnr_retry = 7;
			attr->max_rd_atomic  = dest->out_reads;

			flags |= (IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
		}
	}
	return ibv_modify_qp(qp,attr,flags);
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_connect(struct pingpong_context *ctx,
				struct pingpong_dest *dest,
				struct perftest_parameters *user_param,
				struct pingpong_dest *my_dest) {

	int i;
#ifdef HAVE_DC
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_qp_attr attr_ex;
	#else
	struct ibv_qp_attr_ex attr_ex;
	#endif
#endif
	struct ibv_qp_attr attr;
	int xrc_offset = 0;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		xrc_offset = user_param->num_of_qps / 2;
	}
	for (i=0; i < user_param->num_of_qps; i++) {

		if (user_param->connection_type == DC) {
			if ( ((!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
										|| ((user_param->duplex || user_param->tst == LAT) && (i >= user_param->num_of_qps/2)))) {
				continue;
			}
		}
	#ifdef HAVE_DC
		memset(&attr_ex, 0, sizeof attr_ex);
	#endif
		memset(&attr, 0, sizeof attr);

		if ( (i >= xrc_offset) && (user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT))
			xrc_offset = -1*xrc_offset;

		if(user_param->connection_type == DC) {
		#ifdef HAVE_DC
			if(ctx_modify_dc_qp_to_rtr(ctx->qp[i],&attr_ex,user_param,&dest[xrc_offset + i],&my_dest[i],i)) {
				fprintf(stderr, "Failed to modify QP %d to RTR\n",ctx->qp[i]->qp_num);
				return FAILURE;
			}
		#endif
		} else {
			if(ctx_modify_qp_to_rtr(ctx->qp[i],&attr,user_param,&dest[xrc_offset + i],&my_dest[i],i)) {
				fprintf(stderr, "Failed to modify QP %d to RTR\n",ctx->qp[i]->qp_num);
				return FAILURE;
			}
		}

		if (user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex) {
			if(user_param->connection_type == DC) {
			#ifdef HAVE_DC
				if(ctx_modify_dc_qp_to_rts(ctx->qp[i],&attr_ex,user_param,&dest[xrc_offset + i],&my_dest[i])) {
					fprintf(stderr, "Failed to modify QP to RTS\n");
					return FAILURE;
				}
			#endif
			} else {
				if(ctx_modify_qp_to_rts(ctx->qp[i],&attr,user_param,&dest[xrc_offset + i],&my_dest[i])) {
					fprintf(stderr, "Failed to modify QP to RTS\n");
					return FAILURE;
				}
			}
		}

			if ((user_param->connection_type == UD || user_param->connection_type == DC) &&
				(user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) {

				#ifdef HAVE_DC
				if(user_param->connection_type == DC)
					ctx->ah[i] = ibv_create_ah(ctx->pd,&(attr_ex.ah_attr));
				else
				#endif
					ctx->ah[i] = ibv_create_ah(ctx->pd,&(attr.ah_attr));


				if (!ctx->ah[i]) {
					fprintf(stderr, "Failed to create AH for UD\n");
					return FAILURE;
				}
		}

		if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT))
			xrc_offset = user_param->num_of_qps / 2;

	}
	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
void ctx_set_send_wqes(struct pingpong_context *ctx,
					   struct perftest_parameters *user_param,
					   struct pingpong_dest *rem_dest) {

	int i,j;
	int num_of_qps = user_param->num_of_qps;
	int xrc_offset = 0;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		xrc_offset = num_of_qps;
	}

	for (i = 0; i < num_of_qps ; i++) {
	#ifdef HAVE_VERBS_EXP
		memset(&ctx->wr[i],0,sizeof(struct ibv_exp_send_wr));
	#else
		memset(&ctx->wr[i],0,sizeof(struct ibv_send_wr));
	#endif
		ctx->sge_list[i*user_param->post_list].addr = (uintptr_t)ctx->buf + (i*BUFF_SIZE(ctx->size));
		if (user_param->mac_fwd)
			ctx->sge_list[i*user_param->post_list].addr = (uintptr_t)ctx->buf + (num_of_qps + i)*BUFF_SIZE(ctx->size);

		if (user_param->verb == WRITE || user_param->verb == READ)
			ctx->wr[i*user_param->post_list].wr.rdma.remote_addr   = rem_dest[xrc_offset + i].vaddr;

		else if (user_param->verb == ATOMIC)
			ctx->wr[i*user_param->post_list].wr.atomic.remote_addr = rem_dest[xrc_offset + i].vaddr;

		if (user_param->tst == BW ) {

			ctx->scnt[i] = 0;
			ctx->ccnt[i] = 0;
			ctx->my_addr[i] = (uintptr_t)ctx->buf + (i*BUFF_SIZE(ctx->size));
			if (user_param->verb != SEND)
				ctx->rem_addr[i] = rem_dest[xrc_offset + i].vaddr;
		}

		for (j = 0; j < user_param->post_list; j++) {

			ctx->sge_list[i*user_param->post_list + j].length =  (user_param->connection_type == RawEth) ? (user_param->size - HW_CRC_ADDITION) : user_param->size;
			ctx->sge_list[i*user_param->post_list + j].lkey = ctx->mr->lkey;

			if (j > 0) {

				ctx->sge_list[i*user_param->post_list +j].addr = ctx->sge_list[i*user_param->post_list + (j-1)].addr;

				if ((user_param->tst == BW ) && user_param->size <= (cycle_buffer / 2))
					increase_loc_addr(&ctx->sge_list[i*user_param->post_list +j],user_param->size,j-1,ctx->my_addr[i],0);
			}

			ctx->wr[i*user_param->post_list + j].sg_list = &ctx->sge_list[i*user_param->post_list + j];
			ctx->wr[i*user_param->post_list + j].num_sge = MAX_SEND_SGE;
			ctx->wr[i*user_param->post_list + j].wr_id   = i;

			if (j == (user_param->post_list - 1)) {
				ctx->wr[i*user_param->post_list + j].send_flags = IBV_SEND_SIGNALED;
				ctx->wr[i*user_param->post_list + j].next = NULL;
			}

			else {
				ctx->wr[i*user_param->post_list + j].next = &ctx->wr[i*user_param->post_list+j+1];
				ctx->wr[i*user_param->post_list + j].send_flags = 0;
			}

			if (user_param->verb == ATOMIC) {
			#ifdef HAVE_VERBS_EXP
				ctx->wr[i*user_param->post_list + j].exp_opcode = opcode_atomic_array[user_param->atomicType];
			#else
				ctx->wr[i*user_param->post_list + j].opcode = opcode_atomic_array[user_param->atomicType];
			#endif
			}
			else {
			#ifdef HAVE_VERBS_EXP
				ctx->wr[i*user_param->post_list + j].exp_opcode = opcode_verbs_array[user_param->verb];
			#else
				ctx->wr[i*user_param->post_list + j].opcode = opcode_verbs_array[user_param->verb];
			#endif
			}
			if (user_param->verb == WRITE || user_param->verb == READ) {

				ctx->wr[i*user_param->post_list + j].wr.rdma.rkey = rem_dest[xrc_offset + i].rkey;

				if (j > 0) {

					ctx->wr[i*user_param->post_list + j].wr.rdma.remote_addr = ctx->wr[i*user_param->post_list + (j-1)].wr.rdma.remote_addr;

					if ((user_param->tst == BW) && user_param->size <= (cycle_buffer / 2))
						increase_rem_addr(&ctx->wr[i*user_param->post_list + j],user_param->size,j-1,ctx->rem_addr[i],WRITE);
				}

			} else if (user_param->verb == ATOMIC) {

				ctx->wr[i*user_param->post_list + j].wr.atomic.rkey = rem_dest[xrc_offset + i].rkey;

				if (j > 0) {

					ctx->wr[i*user_param->post_list + j].wr.atomic.remote_addr = ctx->wr[i*user_param->post_list + j-1].wr.atomic.remote_addr;
					if ((user_param->tst == BW))
						increase_rem_addr(&ctx->wr[i*user_param->post_list + j],user_param->size,j-1,ctx->rem_addr[i],ATOMIC);
				}

				if (user_param->atomicType == FETCH_AND_ADD)
					ctx->wr[i*user_param->post_list + j].wr.atomic.compare_add = ATOMIC_ADD_VALUE;

				else
					ctx->wr[i*user_param->post_list + j].wr.atomic.swap = ATOMIC_SWAP_VALUE;


			} else if (user_param->verb == SEND) {

				if (user_param->connection_type == UD) {

					ctx->wr[i*user_param->post_list + j].wr.ud.ah = ctx->ah[i];
					if (user_param->work_rdma_cm) {

						ctx->wr[i*user_param->post_list + j].wr.ud.remote_qkey = user_param->rem_ud_qkey;
						ctx->wr[i*user_param->post_list + j].wr.ud.remote_qpn  = user_param->rem_ud_qpn;

					} else {
						ctx->wr[i*user_param->post_list + j].wr.ud.remote_qkey = DEF_QKEY;
						ctx->wr[i*user_param->post_list + j].wr.ud.remote_qpn  = rem_dest[xrc_offset + i].qpn;
					}

				#ifdef HAVE_DC
				} else if (user_param->connection_type == DC) {
					ctx->wr[i*user_param->post_list + j].dc.ah = ctx->ah[i];
					ctx->wr[i*user_param->post_list + j].dc.dct_access_key = user_param->dct_key;
					ctx->wr[i*user_param->post_list + j].dc.dct_number = rem_dest[xrc_offset + i].qpn;
				#endif
				}
			}

			#ifdef HAVE_DC
				if (user_param->connection_type == DC) {
					ctx->wr[i*user_param->post_list + j].dc.ah = ctx->ah[i];
					ctx->wr[i*user_param->post_list + j].dc.dct_access_key = user_param->dct_key;
					ctx->wr[i*user_param->post_list + j].dc.dct_number = rem_dest[xrc_offset + i].qpn;
				}
			#endif


			if ((user_param->verb == SEND || user_param->verb == WRITE) && user_param->size <= user_param->inline_size)
				ctx->wr[i*user_param->post_list + j].send_flags |= IBV_SEND_INLINE;

		#ifdef HAVE_XRCD
			if (user_param->use_xrc)
				ctx->wr[i*user_param->post_list + j].qp_type.xrc.remote_srqn = rem_dest[xrc_offset + i].srqn;
		#endif
		}
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_set_recv_wqes(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int	i,j,k;
	int num_of_qps = user_param->num_of_qps;
	struct ibv_recv_wr  *bad_wr_recv;
	i = 0;
	int size_per_qp = user_param->rx_depth;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		i = user_param->num_of_qps / 2;
		num_of_qps /= 2;
	}

	if (user_param->use_srq)
			size_per_qp /= user_param->num_of_qps;

	if (user_param->use_rss) {
		i = 1;
		num_of_qps = 1;
	}
	for (k = 0; i < user_param->num_of_qps; i++,k++) {

		ctx->recv_sge_list[i].addr  = (uintptr_t)ctx->buf + (num_of_qps + k)*BUFF_SIZE(ctx->size);

		if (user_param->connection_type == UD)
			ctx->recv_sge_list[i].addr += (CACHE_LINE_SIZE - UD_ADDITION);

		ctx->recv_sge_list[i].length = SIZE(user_param->connection_type,user_param->size,1);
		ctx->recv_sge_list[i].lkey   = ctx->mr->lkey;

		ctx->rwr[i].sg_list = &ctx->recv_sge_list[i];
		ctx->rwr[i].wr_id   = i;
		ctx->rwr[i].next    = NULL;
		ctx->rwr[i].num_sge	= MAX_RECV_SGE;

		if (user_param->tst == BW)
			ctx->rx_buffer_addr[i] = ctx->recv_sge_list[i].addr;

		for (j = 0; j < size_per_qp ; ++j) {

			if (user_param->use_srq) {

				if (ibv_post_srq_recv(ctx->srq,&ctx->rwr[i], &bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv SRQ = %d: counter=%d\n",i,j);
					return 1;
				}

			} else {

				if (ibv_post_recv(ctx->qp[i],&ctx->rwr[i],&bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
					return 1;
				}
			}

			if ((user_param->tst == BW) && user_param->size <= (cycle_buffer / 2)) {

				increase_loc_addr(&ctx->recv_sge_list[i],
								  user_param->size,
								  j,
								  ctx->rx_buffer_addr[i],
								  user_param->connection_type);
			}
		}
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int perform_warm_up(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int ne,index,warmindex,warmupsession;
#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr *bad_wr = NULL;
#else
	struct ibv_send_wr *bad_wr = NULL;
#endif
	struct ibv_wc wc;
	struct ibv_wc *wc_for_cleaning = NULL;
	int num_of_qps = user_param->num_of_qps;

	if(user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	warmupsession = (user_param->post_list == 1) ? user_param->tx_depth : user_param->post_list;
	ALLOCATE(wc_for_cleaning,struct ibv_wc,user_param->tx_depth);

	// Clean up the pipe
	ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth,wc_for_cleaning);

	for (index=0 ; index < num_of_qps ; index++) {

		for (warmindex = 0 ;warmindex < warmupsession ;warmindex += user_param->post_list) {

		#ifdef HAVE_VERBS_EXP
            if (ibv_exp_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
		#else
			if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
		#endif
                fprintf(stderr,"Couldn't post send during warm up: qp %d scnt=%d \n",index,warmindex);
                return 1;
            }
		}

		do {

			ne = ibv_poll_cq(ctx->send_cq,1,&wc);
			if (ne > 0) {

				if (wc.status != IBV_WC_SUCCESS)
					return 1;

				warmindex -= user_param->post_list;

			} else if (ne < 0)
				return 1;

		} while (warmindex);

	}
	free(wc_for_cleaning);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int                totscnt = 0;
	int 	       	   totccnt = 0;
	int                i = 0;
	int                index,ne,tot_iters;
#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr *bad_wr = NULL;
#else
	struct ibv_send_wr *bad_wr = NULL;
#endif
	struct ibv_wc 	   *wc = NULL;
	int num_of_qps = user_param->num_of_qps;

	/* Rate Limiter*/
	int rate_limit_pps = 0;
	double gap_time = 0;	//in usec
	cycles_t gap_cycles = 0;	//in cycles
	cycles_t gap_deadline = 0;
	unsigned int number_of_bursts = 0;
	int burst_iter = 0;
	int is_sending_burst = 0;
	int cpu_mhz = 0;
	/**/

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);

	if (user_param->test_type == DURATION) {
		duration_param=user_param;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);
		user_param->iters = 0;
	}

	// Will be 0, in case of Duration (look at force_dependencies or in the exp above).
	if (user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	tot_iters = user_param->iters*num_of_qps;

	if (user_param->test_type == DURATION && user_param->state != START_STATE) {
		fprintf(stderr, "Failed: margin is not long enough (taking samples before warmup ends)\n");
		fprintf(stderr, "Please increase margin or decrease tx_depth\n");
		return 1;
	}

	if (user_param->test_type == ITERATIONS && user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	// If using rate limiter, calculate gap time between bursts
	if (user_param->is_rate_limiting == 1) {
		// Calculate rate limit in pps
		switch (user_param->rate_units) {
			case MEGA_BYTE_PS:
				rate_limit_pps = ((double)(user_param->rate_limit) / user_param->size) * 1048576;		//1024^2
				break;
			case GIGA_BIT_PS:
				rate_limit_pps = ((double)(user_param->rate_limit) / (user_param->size * 8)) * 1000000000;	//1000^3
				break;
			case PACKET_PS:
				rate_limit_pps = user_param->rate_limit;
				break;
			default:
				fprintf(stderr, " Failed: Unknown rate limit units\n");
				return 1;
		}
		cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
		if (cpu_mhz <= 0) {
			fprintf(stderr, "Failed: couldn't acquire cpu frequency for rate limiter.\n");
		}
		number_of_bursts = rate_limit_pps / user_param->burst_size;
		gap_time = 1000000 * (1.0 / number_of_bursts);
		gap_cycles = cpu_mhz * gap_time;
	}

	// main loop for posting
	while (totscnt < tot_iters  || totccnt < tot_iters || (user_param->test_type == DURATION && user_param->state != END_STATE) ) {

		// main loop to run over all the qps and post each time n messages
		for (index =0 ; index < num_of_qps ; index++) {

			if (user_param->is_rate_limiting == 1 && is_sending_burst == 0) {
				if (gap_deadline > get_cycles()) {
					//Go right to cq polling until gap time is over.
					continue;
				}
				gap_deadline = get_cycles() + gap_cycles;
				is_sending_burst = 1;
				burst_iter = 0;
			}

			while ((ctx->scnt[index] < user_param->iters || user_param->test_type == DURATION) && (ctx->scnt[index] - ctx->ccnt[index]) < (user_param->tx_depth) && 
				!(user_param->is_rate_limiting && is_sending_burst == 0)) {

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1))
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && user_param->state == END_STATE)
					break;
			#ifdef HAVE_VERBS_EXP
				if (ibv_exp_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
			#else
				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
			#endif
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}

				if (user_param->post_list == 1 && user_param->size <= (cycle_buffer / 2)) {

						increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],ctx->my_addr[index],0);

						if (user_param->verb != SEND)
							increase_rem_addr(&ctx->wr[index],user_param->size,ctx->scnt[index],ctx->rem_addr[index],user_param->verb);
				}

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 &&
				   (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || (user_param->test_type == ITERATIONS && ctx->scnt[index] == user_param->iters - 1)))
					ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;

				// Check if a full burst was sent.
				if (user_param->is_rate_limiting == 1) {
					burst_iter += user_param->post_list;
					if (burst_iter >= user_param->burst_size) {
						is_sending_burst = 0;
					}
				}
			}
		}

		if (totccnt < tot_iters || (user_param->test_type == DURATION &&  totccnt < totscnt)) {
			if (user_param->use_event) {
				if (ctx_notify_events(ctx->channel)) {
					fprintf(stderr, "Couldn't request CQ notification\n");
					return 1;
				}
			}

			ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc);

			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					if (wc[i].status != IBV_WC_SUCCESS)
					{
						NOTIFY_COMP_ERROR_SEND(wc[i],totscnt,totccnt);
					}
					ctx->ccnt[(int)wc[i].wr_id] += user_param->cq_mod;
					totccnt += user_param->cq_mod;

					if (user_param->noPeak == OFF) {

						if (totccnt >=  tot_iters - 1)
							user_param->tcompleted[user_param->iters*num_of_qps - 1] = get_cycles();
						else
							user_param->tcompleted[totccnt-1] = get_cycles();
					}

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters += user_param->cq_mod;
				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n",ne);
				return 1;
			}
		}
	}

	if (user_param->noPeak == ON && user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

	free(wc);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static inline void set_on_first_rx_packet(struct perftest_parameters *user_param)
{
	if (user_param->test_type == DURATION) {

		duration_param=user_param;
		user_param->iters=0;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);

	} else if (user_param->tst == BW){
		user_param->tposted[0] = get_cycles();
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw_server(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int 				rcnt = 0;
	int 				ne,i,tot_iters;
	long                 *rcnt_for_qp = NULL;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  *bad_wr_recv = NULL;
	int firstRx = 1;
	int size_per_qp = (user_param->use_srq) ? user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);

	ALLOCATE(rcnt_for_qp,long,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(long)*user_param->num_of_qps);


	if (user_param->use_rss)
		tot_iters = user_param->iters*(user_param->num_of_qps-1);
	else
		tot_iters = user_param->iters*user_param->num_of_qps;

	while (rcnt < tot_iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr ," Failed to notify events to CQ");
				return 1;
			}
		}

		do {
			if (user_param->connection_type == DC)
				ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc);
			else
				ne = ibv_poll_cq(ctx->recv_cq,CTX_POLL_BATCH,wc);
			if (ne > 0) {
				if (firstRx) {
					set_on_first_rx_packet(user_param);
					firstRx = 0;
				}

				for (i = 0; i < ne; i++) {

					if (wc[i].status != IBV_WC_SUCCESS) {

						NOTIFY_COMP_ERROR_RECV(wc[i],(int)rcnt_for_qp[0]);
					}

					rcnt_for_qp[wc[i].wr_id]++;
					rcnt++;

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters++;

					if (user_param->test_type==DURATION || rcnt_for_qp[wc[i].wr_id] + size_per_qp <= user_param->iters) {

						if (user_param->use_srq) {
							if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%d\n",(int)wc[i].wr_id,rcnt);
								return 1;
							}

						} else {

							if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%ld\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
								return 15;
							}
						}

						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (cycle_buffer / 2)) {
							increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
											  user_param->size,
											  rcnt_for_qp[wc[i].wr_id] + size_per_qp,
											  ctx->rx_buffer_addr[wc[i].wr_id],
											  user_param->connection_type);
						}
					}
				}
			}

		} while (ne > 0);

		if (ne < 0) {
			fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
			return 1;
		}
	}

	if (user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

	/*
	if (user_param->use_rss) {
		for (i = 1; i < user_param->num_of_qps; i++)
			fprintf(stderr,"child %d count = %ld\n",i,rcnt_for_qp[i]);
	}*/

	free(wc);
	free(rcnt_for_qp);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw_infinitely(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	int i,j = 0;
	int index = 0,ne;
#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr *bad_wr = NULL;
#else
	struct ibv_send_wr *bad_wr = NULL;
#endif
	struct ibv_wc *wc = NULL;
	int num_of_qps = user_param->num_of_qps;

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);

	duration_param=user_param;
	signal(SIGALRM,catch_alarm_infintely);
	alarm(user_param->duration);
	user_param->iters = 0;

	// Will be 0, in case of Duration (look at force_dependencies or in the exp above)
	if (user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	for (i=0; i < num_of_qps; i++)
		for (j=0 ; j < user_param->post_list; j++)
			ctx->wr[i*user_param->post_list +j].send_flags |= IBV_SEND_SIGNALED;

	user_param->tposted[0] = get_cycles();

	// main loop for posting
	while (1) {

		// main loop to run over all the qps and post each time n messages
		for (index =0 ; index < num_of_qps ; index++) {

			while (ctx->scnt[index] < user_param->tx_depth) {

			#ifdef HAVE_VERBS_EXP
				if (ibv_exp_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
			#else
				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
			#endif
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}

				ctx->scnt[index] += user_param->post_list;
			}
		}


		ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc);

		if (ne > 0) {

			for (i = 0; i < ne; i++) {
				if (wc[i].status != IBV_WC_SUCCESS)
					NOTIFY_COMP_ERROR_SEND(wc[i],ctx->scnt[(int)wc[i].wr_id],ctx->scnt[(int)wc[i].wr_id]);

				ctx->scnt[(int)wc[i].wr_id]--;
				user_param->iters++;
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n",ne);
			return 1;
		}
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw_infinitely_server(struct pingpong_context *ctx, struct perftest_parameters *user_param) {

	int 				i,ne;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr 	*bad_wr_recv = NULL;

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);

	while (1) {

		ne = ibv_poll_cq(ctx->recv_cq,CTX_POLL_BATCH,wc);

		if (ne > 0) {

			for (i = 0; i < ne; i++) {

				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr,"A completion with Error in run_infinitely_bw_server function");
					return 1;
				}

				if (user_param->use_srq) {

					if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv SRQ. QP = %d:\n",(int)wc[i].wr_id);
						return 1;
					}

				} else {

					if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv Qp=%d\n",(int)wc[i].wr_id);
						return 15;
					}
				}
			}

		} else if (ne < 0) {
			fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
			return 1;
		}
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bi(struct pingpong_context *ctx,
				struct perftest_parameters *user_param)  {

	uint64_t totscnt    = 0;
	uint64_t totccnt    = 0;
	uint64_t totrcnt    = 0;
	int i,index      = 0;
	int ne = 0;
	int	*rcnt_for_qp = NULL;
	int tot_iters = 0;
	int iters = 0;
	struct ibv_wc *wc = NULL;
	struct ibv_wc *wc_tx = NULL;
	struct ibv_recv_wr *bad_wr_recv = NULL;
#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr *bad_wr      = NULL;
#else
	struct ibv_send_wr *bad_wr      = NULL;
#endif
	int num_of_qps = user_param->num_of_qps;
	// This is to ensure SERVER will not start to send packets before CLIENT start the test.
	int before_first_rx = ON;
	int size_per_qp = (user_param->use_srq) ? user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;

	ALLOCATE(wc_tx,struct ibv_wc,CTX_POLL_BATCH);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);

	/* This is a very important point. Since this function do RX and TX
	in the same time, we need to give some priority to RX to avoid
	deadlock in UC/UD test scenarios (Recv WQEs depleted due to fast TX) */
	ALLOCATE(wc,struct ibv_wc,user_param->rx_depth);

	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	if (user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	if (user_param->machine == CLIENT) {

		before_first_rx = OFF;
		if (user_param->test_type == DURATION) {
			duration_param=user_param;
			user_param->iters=0;
			duration_param->state = START_STATE;
			signal(SIGALRM, catch_alarm);
			alarm(user_param->margin);
		}
	}

	if(user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	tot_iters = user_param->iters*num_of_qps;
	iters=user_param->iters;

	while ((user_param->test_type == DURATION && user_param->state != END_STATE) || totccnt < tot_iters || totrcnt < tot_iters) {

		for (index=0; index < num_of_qps; index++) {

			while (before_first_rx == OFF && (ctx->scnt[index] < iters || user_param->test_type == DURATION) && ((ctx->scnt[index] - ctx->ccnt[index]) < user_param->tx_depth)) {

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1))
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && duration_param->state == END_STATE)
					break;

			#ifdef HAVE_VERBS_EXP
				if (ibv_exp_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
			#else
				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
			#endif
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}

				if (user_param->post_list == 1 && user_param->size <= (cycle_buffer / 2))
					increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],ctx->my_addr[index],0);

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 && (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || (user_param->test_type == ITERATIONS && ctx->scnt[index] == iters-1)))
					ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
			}
		}

		if (user_param->use_event) {

			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr,"Failed to notify events to CQ");
				return 1;
			}
		}

		ne = ibv_poll_cq(ctx->recv_cq,user_param->rx_depth,wc);
		if (ne > 0) {

			if (user_param->machine == SERVER && before_first_rx == ON) {
				before_first_rx = OFF;
				if (user_param->test_type == DURATION) {
					duration_param=user_param;
					user_param->iters=0;
					duration_param->state = START_STATE;
					signal(SIGALRM, catch_alarm);
					alarm(user_param->margin);
				}
			}

			for (i = 0; i < ne; i++) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_RECV(wc[i],(int)totrcnt);
				}

				rcnt_for_qp[wc[i].wr_id]++;
				totrcnt++;

				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters++;

				if (user_param->test_type==DURATION || rcnt_for_qp[wc[i].wr_id] + size_per_qp <= user_param->iters) {

					if (user_param->use_srq) {
						if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%d\n",(int)wc[i].wr_id,(int)totrcnt);
							return 1;
						}

					} else {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}
					}

					if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (cycle_buffer / 2)) {
						increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
										  user_param->size,
										  rcnt_for_qp[wc[i].wr_id] + size_per_qp -1,
										  ctx->rx_buffer_addr[wc[i].wr_id],user_param->connection_type);
					}
				}
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return FAILURE;
		}

		ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc_tx);

		if (ne > 0) {
			for (i = 0; i < ne; i++) {
				if (wc_tx[i].status != IBV_WC_SUCCESS)
					 NOTIFY_COMP_ERROR_SEND(wc_tx[i],(int)totscnt,(int)totccnt);
				totccnt += user_param->cq_mod;
				ctx->ccnt[(int)wc_tx[i].wr_id] += user_param->cq_mod;

				if (user_param->noPeak == OFF) {

					if ((user_param->test_type == ITERATIONS && (totccnt >= tot_iters - 1)))
						user_param->tcompleted[tot_iters - 1] = get_cycles();
					else
						user_param->tcompleted[totccnt-1] = get_cycles();
				}

				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters += user_param->cq_mod;
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return FAILURE;
		}
	}

	if (user_param->noPeak == ON && user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

	free(rcnt_for_qp);
	free(wc);
	free(wc_tx);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_lat_write(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	int                     scnt = 0;
	int                     ccnt = 0;
	int                     rcnt = 0;
	int                     ne;
	int 					poll_buf_offset = 0;
	volatile char           *poll_buf = NULL;
	volatile char           *post_buf = NULL;
#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr      *bad_wr = NULL;
#else
	struct ibv_send_wr      *bad_wr = NULL;
#endif
	struct ibv_wc           wc;

	ctx->wr[0].sg_list->length = user_param->size;
	ctx->wr[0].send_flags      = IBV_SEND_SIGNALED;

	if (user_param->size <= user_param->inline_size)
		ctx->wr[0].send_flags |= IBV_SEND_INLINE;

	if((user_param->use_xrc || user_param->connection_type == DC))
		poll_buf_offset = 1;
	post_buf = (char*)ctx->buf + user_param->size - 1;
	poll_buf = (char*)ctx->buf + (user_param->num_of_qps + poll_buf_offset)*BUFF_SIZE(ctx->size) + user_param->size - 1;

	// Duration support in latency tests.
	if (user_param->test_type == DURATION) {
		duration_param=user_param;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);
		user_param->iters = 0;
	}

	/* Done with setup. Start the test. */
	while (scnt < user_param->iters || ccnt < user_param->iters || rcnt < user_param->iters
			|| ((user_param->test_type == DURATION && user_param->state != END_STATE))) {

		if ((rcnt < user_param->iters || user_param->test_type == DURATION) && !(scnt < 1 && user_param->machine == SERVER)) {
			rcnt++;
			while (*poll_buf != (char)rcnt && user_param->state != END_STATE);
		}

		if (scnt < user_param->iters || user_param->test_type == DURATION) {

			if (user_param->test_type == ITERATIONS)
				user_param->tposted[scnt] = get_cycles();

			*post_buf = (char)++scnt;
		#ifdef HAVE_VERBS_EXP
			if (ibv_exp_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr)) {
		#else
			if (ibv_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr)) {
		#endif
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return FAILURE;
			}
		}

		if (user_param->test_type == DURATION && user_param->state == END_STATE)
			break;

		if (ccnt < user_param->iters || user_param->test_type == DURATION) {

			do { ne = ibv_poll_cq(ctx->send_cq, 1, &wc); } while (ne == 0);

			if(ne > 0) {

				if (wc.status != IBV_WC_SUCCESS)
					NOTIFY_COMP_ERROR_SEND(wc,scnt,ccnt);

				ccnt++;
				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters++;

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return FAILURE;
			}
		}
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_lat(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	int scnt = 0;
	int ne;
#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr *bad_wr = NULL;
#else
	struct ibv_send_wr *bad_wr = NULL;
#endif
	struct ibv_wc wc;

	ctx->wr[0].sg_list->length = user_param->size;
	ctx->wr[0].send_flags = IBV_SEND_SIGNALED;

	// Duration support in latency tests.
	if (user_param->test_type == DURATION) {
		duration_param=user_param;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);
		user_param->iters = 0;
	}

	while (scnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

		if (user_param->test_type == ITERATIONS)
			user_param->tposted[scnt++] = get_cycles();

	#ifdef HAVE_VERBS_EXP
		if (ibv_exp_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr)) {
	#else
		if (ibv_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr)) {
	#endif
			fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
			return 11;
		}

		if (user_param->test_type == DURATION && user_param->state == END_STATE)
			break;

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return 1;
			}
		}

		do {
			ne = ibv_poll_cq(ctx->send_cq, 1, &wc);

			if(ne > 0) {
				if (wc.status != IBV_WC_SUCCESS)
					NOTIFY_COMP_ERROR_SEND(wc,scnt,scnt);

				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters++;

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return FAILURE;
			}

		} while (!user_param->use_event && ne == 0);
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_lat_send(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	int				scnt = 0; //sent packets counter
	int				rcnt = 0; //received packets counter
	int				poll = 0;
	int				ne;
	struct 			ibv_wc	wc;
	struct 			ibv_recv_wr	*bad_wr_recv;
#ifdef HAVE_VERBS_EXP
	struct 			ibv_exp_send_wr	*bad_wr;
#else
	struct          ibv_send_wr     *bad_wr;
#endif
	int  			firstRx = 1;
	int size_per_qp = (user_param->use_srq) ? user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;


	if (user_param->connection_type != RawEth) {
		ctx->wr[0].sg_list->length = user_param->size;
		ctx->wr[0].send_flags = 0;
	}

	//ask Idos about this.
	if (user_param->size <= user_param->inline_size)
		ctx->wr[0].send_flags |= IBV_SEND_INLINE;

	while (scnt < user_param->iters || rcnt < user_param->iters ||
			( (user_param->test_type == DURATION && user_param->state != END_STATE))) {

		/* 
		 * Get the received packet. make sure that the client won't enter here until he sends
		 * his first packet (scnt < 1)
		 * server will enter here first and wait for a packet to arrive (from the client)
		 */
		if ((rcnt < user_param->iters || user_param->test_type == DURATION) && !(scnt < 1 && user_param->machine == CLIENT)) {

			if (user_param->use_event) {
				if (ctx_notify_events(ctx->channel)) {
					fprintf(stderr , " Failed to notify events to CQ");
					return 1;
				}
			}

			do {
				ne = ibv_poll_cq(ctx->recv_cq,1,&wc);

				if (user_param->test_type == DURATION && user_param->state == END_STATE)
					break;

				if (ne > 0) {

					if (firstRx) {
						set_on_first_rx_packet(user_param);
						firstRx = 0;
					}

					if (wc.status != IBV_WC_SUCCESS)
						NOTIFY_COMP_ERROR_RECV(wc,rcnt);

					rcnt++;

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters++;

					//if we're in duration mode or there is enough space in the rx_depth, post that you received a packet
					if (user_param->test_type==DURATION || (rcnt + size_per_qp  <= user_param->iters)) {

						if (user_param->use_srq) {

							if (ibv_post_srq_recv(ctx->srq,&ctx->rwr[wc.wr_id],&bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%d\n",(int)wc.wr_id,rcnt);
								return 1;
							}

						} else {

							if (ibv_post_recv(ctx->qp[wc.wr_id],&ctx->rwr[wc.wr_id],&bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv: rcnt=%d\n",rcnt);
								return 15;
							}
						}
					}
				}
			} while (!user_param->use_event && ne == 0);
		}

		if (scnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

			if (user_param->test_type == ITERATIONS)
				user_param->tposted[scnt] = get_cycles();

			scnt++;

			if (scnt % user_param->cq_mod == 0 || (user_param->test_type == ITERATIONS && scnt == user_param->iters)) {
				poll = 1;
				ctx->wr[0].send_flags |= IBV_SEND_SIGNALED;
			}

			//if we're in duration mode and the time is over, exit from this function
			if (user_param->test_type == DURATION && user_param->state == END_STATE)
				break;

			//send the packet that's in index 0 on the buffer
		#ifdef HAVE_VERBS_EXP	
			if (ibv_exp_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr)) {
		#else
			if (ibv_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr)) {
		#endif
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return FAILURE;
			}


			if (poll == 1) {

				struct ibv_wc s_wc;
				int s_ne;

				if (user_param->use_event) {
					if (ctx_notify_events(ctx->channel)) {
						fprintf(stderr , " Failed to notify events to CQ");
						return FAILURE;
					}
				}

				//wait until you get a cq for the last packet
				do {
					s_ne = ibv_poll_cq(ctx->send_cq, 1, &s_wc);
				} while (!user_param->use_event && s_ne == 0);



				if (s_ne < 0) {
					fprintf(stderr, "poll on Send CQ failed %d\n", s_ne);
					return FAILURE;
				}

				if (s_wc.status != IBV_WC_SUCCESS)
					NOTIFY_COMP_ERROR_SEND(s_wc,scnt,scnt)

				poll = 0;
				ctx->wr[0].send_flags &= ~IBV_SEND_SIGNALED;
			}
		}
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
uint16_t ctx_get_local_lid(struct ibv_context *context,int port) {

	struct ibv_port_attr attr;

	if (ibv_query_port(context,port,&attr))
		return 0;

	return attr.lid;
}

/******************************************************************************
 *
 ******************************************************************************/
void catch_alarm(int sig) {
	switch (duration_param->state) {
		case START_STATE:
			duration_param->state = SAMPLE_STATE;
			duration_param->tposted[0] = get_cycles();
			alarm(duration_param->duration - 2*(duration_param->margin));
			break;
		case SAMPLE_STATE:
			duration_param->state = STOP_SAMPLE_STATE;
			duration_param->tcompleted[0] = get_cycles();
			alarm(duration_param->margin);
			break;
		case STOP_SAMPLE_STATE:
			duration_param->state = END_STATE;
			break;
		default:
			fprintf(stderr,"unknown state\n");
	}
}

/******************************************************************************
 *
 ******************************************************************************/
void catch_alarm_infintely(int sig)
{
	duration_param->tcompleted[0] = get_cycles();
	print_report_bw(duration_param,NULL);
	duration_param->iters = 0;
	alarm(duration_param->duration);
	duration_param->tposted[0] = get_cycles();
}

/******************************************************************************
 * End
 ******************************************************************************/

