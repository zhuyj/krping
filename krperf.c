/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/signal.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "krperf_getopt.h"
#include "krperf.h"
#include "krperf_srq.h"
#include "krperf_proc.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": file: %s +%d caller: %ps " fmt, __FILE__, __LINE__, __builtin_return_address(0)
#define PFX "krperf: "

DEFINE_MUTEX(krperf_mutex);

/*
 * List of running krperf threads.
 */
LIST_HEAD(krperf_cbs);

static int debug = 0;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
#define DEBUG_LOG if (debug) printk

MODULE_AUTHOR("Yanjun.Zhu");
MODULE_DESCRIPTION("RDMA perf/ping server");
MODULE_LICENSE("Dual BSD/GPL");

static int krperf_cma_event_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	int ret;
	struct krperf_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			pr_err("rdma_resolve_route error %d(%pe)\n", ret, ERR_PTR(ret));
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		if (!cb->server) {
			cb->state = CONNECTED;
		}
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		pr_err("cma event %d, error %d\n", event->event,
		       event->status);
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		pr_err("DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		pr_err("cma detected device removal!!!!\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	default:
		pr_err("oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

static int server_recv(struct krperf_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		pr_err("Received bogus data, size %d\n", wc->byte_len);
		return -EINVAL;
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);
	cb->remote_addr = ntohll(cb->recv_buf.buf);
	cb->remote_len  = ntohl(cb->recv_buf.size);
	DEBUG_LOG("Received rkey %x addr %llx len %d from peer\n",
		  cb->remote_rkey, (unsigned long long)cb->remote_addr, 
		  cb->remote_len);

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;

	return 0;
}

static int client_recv(struct krperf_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		pr_err("Received bogus data, size %d\n", wc->byte_len);
		return -EINVAL;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

static void krperf_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krperf_cb *cb = ctx;
	struct ib_wc wc;
	const struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR) {
		pr_err("cq completion in ERROR state\n");
		return;
	}
	if (cb->frtest) {
		pr_err("cq completion event in frtest!\n");
		return;
	}
	if (!cb->wlat && !cb->rlat && !cb->bw)
		schedule_work(&cb->ib_req_notify_cq_work);
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				pr_err("cq completion failed with "
				       "wr_id %Lx status: %s opcode %d vender_err %x\n",
					wc.wr_id, ib_wc_status_msg(wc.status), wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("send completion\n");
			cb->stats.send_bytes += cb->send_sgl.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			cb->stats.write_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.write_msgs++;
			cb->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n");
			cb->stats.read_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.read_msgs++;
			cb->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			DEBUG_LOG("recv completion\n");
			cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
			if (cb->wlat || cb->rlat || cb->bw)
				ret = server_recv(cb, &wc);
			else
				ret = cb->server ? server_recv(cb, &wc) :
						   client_recv(cb, &wc);
			if (ret) {
				pr_err("recv wc error: %d(%pe)\n", ret, ERR_PTR(ret));
				goto error;
			}

			ret = krperf_ib_srq_rq_post_recv(cb, &bad_wr);
			if (ret) {
				pr_err("post recv error: %d(%pe)\n", ret, ERR_PTR(ret));
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			pr_err("%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		pr_err("poll error %d(%pe)\n", ret, ERR_PTR(ret));
		goto error;
	}
	return;
error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}

static void krperf_ib_req_notify_cq_handler(struct work_struct *work)
{
	struct krperf_cb *cb_work = container_of(work,
						 struct krperf_cb,
						 ib_req_notify_cq_work);

	ib_req_notify_cq(cb_work->cq, IB_CQ_NEXT_COMP);
}

static int krperf_accept(struct krperf_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		pr_err("rdma_accept error: %d(%pe)\n", ret, ERR_PTR(ret));
		return ret;
	}

	if (!cb->wlat && !cb->rlat && !cb->bw) {
		wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
		if (cb->state == ERROR) {
			pr_err("wait for CONNECTED state %d\n", cb->state);
			return -1;
		}
	}
	return 0;
}

static void krperf_setup_wr(struct krperf_cb *cb)
{
	cb->recv_sgl.addr = cb->recv_dma_addr;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey = cb->pd->local_dma_lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->pd->local_dma_lkey;

	cb->sq_wr.opcode = IB_WR_SEND;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	if (cb->server || cb->wlat || cb->rlat || cb->bw) {
		cb->rdma_sgl.addr = cb->rdma_dma_addr;
		cb->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED;
		cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;
		cb->rdma_sq_wr.wr.num_sge = 1;
	}

	/* 
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled.  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
	cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;
	cb->reg_mr_wr.mr = cb->reg_mr;

	cb->invalidate_wr.next = &cb->reg_mr_wr.wr;
	cb->invalidate_wr.opcode = IB_WR_LOCAL_INV;
}

static int krperf_setup_buffers(struct krperf_cb *cb)
{
	int ret;

	DEBUG_LOG(PFX "krperf_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = ib_dma_map_single(cb->pd->device,
				   &cb->recv_buf, 
				   sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
	cb->send_dma_addr = ib_dma_map_single(cb->pd->device,
					   &cb->send_buf, sizeof(cb->send_buf),
					   DMA_BIDIRECTIONAL);
	dma_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	cb->rdma_buf = kzalloc(cb->size, GFP_KERNEL);
	if (cb->rdma_buf)
		cb->rdma_dma_addr = ib_dma_map_single(cb->pd->device, cb->rdma_buf, cb->size, DMA_BIDIRECTIONAL);
	if (!cb->rdma_buf || ib_dma_mapping_error(cb->pd->device, cb->rdma_dma_addr)) {
		DEBUG_LOG(PFX "rdma_buf allocation failed\n");
		kfree(cb->rdma_buf);
		ret = -ENOMEM;
		goto bail;
	}
	dma_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);
	cb->page_list_len = (((cb->size - 1) & PAGE_MASK) + PAGE_SIZE)
				>> PAGE_SHIFT;
	cb->reg_mr = ib_alloc_mr(cb->pd,  IB_MR_TYPE_MEM_REG,
				 cb->page_list_len);
	if (IS_ERR(cb->reg_mr)) {
		ret = PTR_ERR(cb->reg_mr);
		DEBUG_LOG(PFX "recv_buf reg_mr failed %d\n", ret);
		goto bail;
	}
	DEBUG_LOG(PFX "reg rkey 0x%x page_list_len %u\n",
		cb->reg_mr->rkey, cb->page_list_len);

	if (!cb->server || cb->wlat || cb->rlat || cb->bw) {
		cb->start_buf = kzalloc(cb->size, GFP_KERNEL);
		if (cb->start_buf)
			cb->start_dma_addr = ib_dma_map_single(cb->pd->device, cb->start_buf, cb->size, DMA_BIDIRECTIONAL);
		if (!cb->start_buf || ib_dma_mapping_error(cb->pd->device, cb->start_dma_addr)) {
			DEBUG_LOG(PFX "start_buf malloc failed\n");
			kfree(cb->start_buf);
			ret = -ENOMEM;
			goto bail;
		}
		dma_unmap_addr_set(cb, start_mapping, cb->start_dma_addr);
	}

	krperf_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	return 0;
bail:
	if (cb->reg_mr && !IS_ERR(cb->reg_mr))
		ib_dereg_mr(cb->reg_mr);
	if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
		ib_dereg_mr(cb->rdma_mr);
	if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_buf) {
		ib_dma_unmap_single(cb->pd->device, cb->rdma_dma_addr, cb->size,
				    DMA_BIDIRECTIONAL);
		kfree(cb->rdma_buf);
	}
	if (cb->start_buf) {
		ib_dma_unmap_single(cb->pd->device, cb->start_dma_addr, cb->size,
				    DMA_BIDIRECTIONAL);
		kfree(cb->start_buf);
	}
	return ret;
}

static void krperf_free_buffers(struct krperf_cb *cb)
{
	DEBUG_LOG("krperf_free_buffers called on cb %p\n", cb);
	
	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_mr)
		ib_dereg_mr(cb->rdma_mr);
	if (cb->start_mr)
		ib_dereg_mr(cb->start_mr);
	if (cb->reg_mr)
		ib_dereg_mr(cb->reg_mr);

	ib_dma_unmap_single(cb->pd->device,
			 dma_unmap_addr(cb, recv_mapping),
			 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	ib_dma_unmap_single(cb->pd->device,
			 dma_unmap_addr(cb, send_mapping),
			 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);

	ib_dma_unmap_single(cb->pd->device, dma_unmap_addr(cb, rdma_dma_addr),
			    cb->size, DMA_BIDIRECTIONAL);
	kfree(cb->rdma_buf);

	if (cb->start_buf) {
		ib_dma_unmap_single(cb->pd->device, dma_unmap_addr(cb, start_dma_addr),
				    cb->size, DMA_BIDIRECTIONAL);
		kfree(cb->start_buf);
	}
}

static int krperf_create_qp(struct krperf_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 2;
	
	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	if (krperf_srq_valid(cb)) {
		init_attr.srq = cb->srq;
		init_attr.cap.max_recv_wr = 0;
	}

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void krperf_free_qp(struct krperf_cb *cb)
{
	ib_destroy_qp(cb->qp);
	krperf_free_srq(cb);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

static int krperf_setup_qp(struct krperf_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device, 0);
	if (IS_ERR(cb->pd)) {
		pr_err("ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	attr.cqe = cb->txdepth * 2;
	attr.comp_vector = 0;
	cb->cq = ib_create_cq(cm_id->device, krperf_cq_event_handler, NULL,
			      cb, &attr);
	if (IS_ERR(cb->cq)) {
		pr_err("ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);

	if (!cb->wlat && !cb->rlat && !cb->bw && !cb->frtest) {
		ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		if (ret) {
			pr_err("ib_create_cq failed\n");
			goto err2;
		}
	}

	ret = krperf_alloc_srq(cb);
	if (ret) {
		pr_err("srq alloc failed: %d(%pe)\n", ret, ERR_PTR(ret));
		goto err2;
	}

	ret = krperf_create_qp(cb);
	if (ret) {
		pr_err("krperf_create_qp failed: %d(%pe)\n", ret, ERR_PTR(ret));
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;
err3:
	krperf_free_srq(cb);
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

/*
 * return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * other modes: just return the mr rkey.
 */
static u32 krperf_rdma_rkey(struct krperf_cb *cb, u64 buf, int post_inv)
{
	u32 rkey;
	const struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};
	sg_init_marker(&sg, 1);

	cb->invalidate_wr.ex.invalidate_rkey = cb->reg_mr->rkey;

	/*
	 * Update the reg key.
	 */
	ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
	cb->reg_mr_wr.key = cb->reg_mr->rkey;

	/*
	 * Update the reg WR with new buf info.
	 */
	if (buf == (u64)cb->start_dma_addr)
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ;
	else
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = cb->size;

	ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
	BUG_ON(ret <= 0 || ret > cb->page_list_len);

	DEBUG_LOG(PFX "post_inv = %d, reg_mr new rkey 0x%x pgsz %u len %lu"
		" iova_start %llx\n",
		post_inv,
		cb->reg_mr_wr.key,
		cb->reg_mr->page_size,
		(unsigned long)cb->reg_mr->length,
		(unsigned long long)cb->reg_mr->iova);

	if (post_inv)
		ret = ib_post_send(cb->qp, &cb->invalidate_wr, &bad_wr);
	else
		ret = ib_post_send(cb->qp, &cb->reg_mr_wr.wr, &bad_wr);
	if (ret) {
		pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
		cb->state = ERROR;
	}
	rkey = cb->reg_mr->rkey;
	return rkey;
}

static void krperf_format_send(struct krperf_cb *cb, u64 buf)
{
	struct krperf_rdma_info *info = &cb->send_buf;
	u32 rkey;

	/*
	 * Client side will do reg or mw bind before
	 * advertising the rdma buffer.  Server side
	 * sends have no data.
	 */
	if (!cb->server || cb->wlat || cb->rlat || cb->bw) {
		rkey = krperf_rdma_rkey(cb, buf, !cb->server_invalidate);
		info->buf = htonll(buf);
		info->rkey = htonl(rkey);
		info->size = htonl(cb->size);
		DEBUG_LOG("RDMA addr %llx rkey %x len %d\n",
			  (unsigned long long)buf, rkey, cb->size);
	}
}

static void krperf_test_server(struct krperf_cb *cb)
{
	struct ib_send_wr inv;
	const struct ib_send_wr *bad_wr;
	int ret;

	while (1) {
		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
		if (cb->state != RDMA_READ_ADV) {
			pr_err("wait for RDMA_READ_ADV state %d\n",
				cb->state);
			break;
		}

		DEBUG_LOG("server received sink adv\n");

		cb->rdma_sq_wr.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.wr.sg_list->length = cb->remote_len;
		cb->rdma_sgl.lkey = krperf_rdma_rkey(cb, cb->rdma_dma_addr, !cb->read_inv);
		cb->rdma_sq_wr.wr.next = NULL;

		/* Issue RDMA Read. */
		if (cb->read_inv)
			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
		else {

			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
			/* 
			 * Immediately follow the read with a 
			 * fenced LOCAL_INV.
			 */
			cb->rdma_sq_wr.wr.next = &inv;
			memset(&inv, 0, sizeof inv);
			inv.opcode = IB_WR_LOCAL_INV;
			inv.ex.invalidate_rkey = cb->reg_mr->rkey;
			inv.send_flags = IB_SEND_FENCE;
		}

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret) {
			pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
			break;
		}
		cb->rdma_sq_wr.wr.next = NULL;

		DEBUG_LOG("server posted rdma read req \n");

		/* Wait for read completion */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_READ_COMPLETE);
		if (cb->state != RDMA_READ_COMPLETE) {
			pr_err("wait for RDMA_READ_COMPLETE state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server received read complete\n");

		/* Display data in recv buf */
		if (cb->verbose)
			printk(KERN_INFO PFX
				"server ping data (64B max): |%.64s|\n",
				cb->rdma_buf);

		/* Tell client to continue */
		if (cb->server && cb->server_invalidate) {
			cb->sq_wr.ex.invalidate_rkey = cb->remote_rkey;
			cb->sq_wr.opcode = IB_WR_SEND_WITH_INV;
			DEBUG_LOG("send-w-inv rkey 0x%x\n", cb->remote_rkey);
		} 
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
			break;
		}
		DEBUG_LOG("server posted go ahead\n");

		/* Wait for client's RDMA STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			pr_err("wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server received sink adv\n");

		/* RDMA Write echo data */
		cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
		cb->rdma_sq_wr.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.wr.sg_list->length = strlen(cb->rdma_buf) + 1;
		if (cb->local_dma_lkey)
			cb->rdma_sgl.lkey = cb->pd->local_dma_lkey;
		else 
			cb->rdma_sgl.lkey = krperf_rdma_rkey(cb, cb->rdma_dma_addr, 0);
			
		DEBUG_LOG("rdma write from lkey %x laddr %llx len %d\n",
			  cb->rdma_sq_wr.wr.sg_list->lkey,
			  (unsigned long long)cb->rdma_sq_wr.wr.sg_list->addr,
			  cb->rdma_sq_wr.wr.sg_list->length);

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret) {
			pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
			break;
		}

		/* Wait for completion */
		ret = wait_event_interruptible(cb->sem, cb->state >= 
							 RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			pr_err("wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server rdma write complete \n");

		cb->state = CONNECTED;

		/* Tell client to begin again */
		if (cb->server && cb->server_invalidate) {
			cb->sq_wr.ex.invalidate_rkey = cb->remote_rkey;
			cb->sq_wr.opcode = IB_WR_SEND_WITH_INV;
			DEBUG_LOG("send-w-inv rkey 0x%x\n", cb->remote_rkey);
		} 
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
			break;
		}
		DEBUG_LOG("server posted go ahead\n");
	}
}

static void rlat_test(struct krperf_cb *cb)
{
	int scnt;
	int iters = cb->count;
	ktime_t start, stop;
	int ret;
	struct ib_wc wc;
	const struct ib_send_wr *bad_wr;
	int ne;

	scnt = 0;
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	start = ktime_get();
	if (!cb->poll) {
		cb->state = RDMA_READ_ADV;
		schedule_work(&cb->ib_req_notify_cq_work);
	}
	while (scnt < iters) {

		cb->state = RDMA_READ_ADV;
		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret) {
			pr_err("Couldn't post send: ret=%d scnt %d\n",
				ret, scnt);
			return;
		}

		do {
			if (!cb->poll) {
				wait_event_interruptible(cb->sem, 
					cb->state != RDMA_READ_ADV);
				if (cb->state == RDMA_READ_COMPLETE) {
					ne = 1;
					schedule_work(&cb->ib_req_notify_cq_work);
				} else {
					ne = -1;
				}
			} else
				ne = ib_poll_cq(cb->cq, 1, &wc);
			if (cb->state == ERROR) {
				pr_err("state == ERROR...bailing scnt %d\n",
					scnt);
				return;
			}
		} while (ne == 0);

		if (ne < 0) {
			pr_err("poll CQ failed %d(%pe)\n", ne, ERR_PTR(ne));
			return;
		}
		if (cb->poll && wc.status != IB_WC_SUCCESS) {
			pr_err("Completion wth error at %s:\n",
				cb->server ? "server" : "client");
			pr_err("Failed status %d: wr_id %d\n",
				wc.status, (int) wc.wr_id);
			return;
		}
		++scnt;
	}
	stop = ktime_get();

	pr_err("delta nsec %llu iter %d size %d\n",
		ktime_sub(stop, start),
		scnt, cb->size);
}

static void wlat_test(struct krperf_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters=cb->count;
	volatile char *poll_buf = (char *) cb->start_buf;
	char *buf = (char *)cb->rdma_buf;
	ktime_t start, stop;
	cycles_t *post_cycles_start = NULL;
	cycles_t *post_cycles_stop = NULL;
	cycles_t *poll_cycles_start = NULL;
	cycles_t *poll_cycles_stop = NULL;
	cycles_t *last_poll_cycles_start = NULL;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), 
		GFP_KERNEL);
	if (!last_poll_cycles_start) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	start = ktime_get();
	while (scnt < iters || ccnt < iters || rcnt < iters) {

		/* Wait till buffer changes. */
		if (rcnt < iters && !(scnt < 1 && !cb->server)) {
			++rcnt;
			while (*poll_buf != (char)rcnt) {
				if (cb->state == ERROR) {
					pr_err("state = ERROR, bailing\n");
					goto done;
				}
			}
		}

		if (scnt < iters) {
			const struct ib_send_wr *bad_wr;

			*buf = (char)scnt+1;
			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr)) {
				pr_err("Couldn't post send: scnt=%d\n",
					scnt);
				goto done;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			scnt++;
		}

		if (ccnt < iters) {
			struct ib_wc wc;
			int ne;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do {
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] = 
						get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			++ccnt;

			if (ne < 0) {
				pr_err("poll CQ failed %d(%pe)\n", ne, ERR_PTR(ne));
				goto done;
			}
			if (wc.status != IB_WC_SUCCESS) {
				pr_err("Completion wth error at %s:\n",
					cb->server ? "server" : "client");
				pr_err("Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				pr_err("scnt=%d, rcnt=%d, ccnt=%d\n",
					scnt, rcnt, ccnt);
				goto done;
			}
		}
	}
	stop = ktime_get();

	for (i=0; i < cycle_iters; i++) {
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i]-last_poll_cycles_start[i];
	}
	pr_err("delta nsec %llu iter %d size %d cycle_iters %d"
		" sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		ktime_sub(stop, start),
		scnt, cb->size, cycle_iters,
		(unsigned long long)sum_post, (unsigned long long)sum_poll, 
		(unsigned long long)sum_last_poll);
done:
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void bw_test(struct krperf_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters=cb->count;
	ktime_t start, stop;
	cycles_t *post_cycles_start = NULL;
	cycles_t *post_cycles_stop = NULL;
	cycles_t *poll_cycles_start = NULL;
	cycles_t *poll_cycles_stop = NULL;
	cycles_t *last_poll_cycles_start = NULL;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), 
		GFP_KERNEL);
	if (!last_poll_cycles_start) {
		pr_err("%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	start = ktime_get();
	while (scnt < iters || ccnt < iters) {

		while (scnt < iters && scnt - ccnt < cb->txdepth) {
			const struct ib_send_wr *bad_wr;

			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr)) {
				pr_err("Couldn't post send: scnt=%d\n",
					scnt);
				goto done;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			++scnt;
		}

		if (ccnt < iters) {
			int ne;
			struct ib_wc wc;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do {
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] = 
						get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			ccnt += 1;

			if (ne < 0) {
				pr_err("poll CQ failed %d(%pe)\n", ne, ERR_PTR(ne));
				goto done;
			}
			if (wc.status != IB_WC_SUCCESS) {
				pr_err("Completion wth error at %s:\n",
					cb->server ? "server" : "client");
				pr_err("Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				goto done;
			}
		}
	}
	stop = ktime_get();

	for (i=0; i < cycle_iters; i++) {
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i]-last_poll_cycles_start[i];
	}
	pr_err("delta nsec %llu iter %d size %d cycle_iters %d"
		" sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		ktime_sub(stop, start), scnt, cb->size, cycle_iters, 
		(unsigned long long)sum_post, (unsigned long long)sum_poll, 
		(unsigned long long)sum_last_poll);
done:
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void krperf_rlat_test_server(struct krperf_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krperf_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krperf_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		pr_err("poll error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}
	if (wc.status) {
		pr_err("send completiong error %d\n", wc.status);
		return;
	}

	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static void krperf_wlat_test_server(struct krperf_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krperf_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krperf_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		pr_err("poll error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}
	if (wc.status) {
		pr_err("send completiong error %d\n", wc.status);
		return;
	}

	wlat_test(cb);
	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static void krperf_bw_test_server(struct krperf_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krperf_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krperf_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		pr_err("poll error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}
	if (wc.status) {
		pr_err("send completiong error %d\n", wc.status);
		return;
	}

	if (cb->duplex)
		bw_test(cb);
	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static int reg_supported(struct ib_device *dev)
{
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS;

	if ((dev->attrs.device_cap_flags & needed_flags) != needed_flags) {
		pr_err("Fastreg not supported - device_cap_flags 0x%llx\n",
			(unsigned long long)dev->attrs.device_cap_flags);
		return 0;
	}
	DEBUG_LOG("Fastreg supported - device_cap_flags 0x%llx\n",
		(unsigned long long)dev->attrs.device_cap_flags);
	return 1;
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct krperf_cb *cb)
{
	memset(sin, 0, sizeof(*sin));

	if (cb->addr_type == AF_INET) {
		struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	} else if (cb->addr_type == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
		sin6->sin6_family = AF_INET6;
		memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
		sin6->sin6_port = cb->port;
		if (cb->ip6_ndev_name[0] != 0) {
			struct net_device *ndev;

			ndev = __dev_get_by_name(&init_net, cb->ip6_ndev_name);
			if (ndev != NULL) {
				sin6->sin6_scope_id = ndev->ifindex;
				dev_put(ndev);
			}
		}
	}
}

static int krperf_bind_server(struct krperf_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		pr_err("rdma_bind_addr error %d(%pe)\n", ret, ERR_PTR(ret));
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		pr_err("rdma_listen failed: %d(%pe)\n", ret, ERR_PTR(ret));
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST);
	if (cb->state != CONNECT_REQUEST) {
		pr_err("wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	if (!reg_supported(cb->child_cm_id->device))
		return -EINVAL;

	return 0;
}

static void krperf_run_server(struct krperf_cb *cb)
{
	const struct ib_recv_wr *bad_wr;
	int ret;

	ret = krperf_bind_server(cb);
	if (ret)
		return;

	ret = krperf_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		pr_err("setup_qp failed: %d(%pe)\n", ret, ERR_PTR(ret));
		goto err0;
	}

	ret = krperf_setup_buffers(cb);
	if (ret) {
		pr_err("krperf_setup_buffers failed: %d(%pe)\n", ret, ERR_PTR(ret));
		goto err1;
	}

	ret = krperf_ib_srq_rq_post_recv(cb, &bad_wr);
	if (ret) {
		pr_err("krperf_ib_srq_rq_post_recv failed: %d(%pe)\n", ret, ERR_PTR(ret));
		goto err2;
	}

	ret = krperf_accept(cb);
	if (ret) {
		pr_err("connect error %d(%pe)\n", ret, ERR_PTR(ret));
		goto err2;
	}

	if (cb->wlat)
		krperf_wlat_test_server(cb);
	else if (cb->rlat)
		krperf_rlat_test_server(cb);
	else if (cb->bw)
		krperf_bw_test_server(cb);
	else
		krperf_test_server(cb);
	rdma_disconnect(cb->child_cm_id);
err2:
	krperf_free_buffers(cb);
err1:
	krperf_free_qp(cb);
err0:
	rdma_destroy_id(cb->child_cm_id);
}

static void krperf_test_client(struct krperf_cb *cb)
{
	int ping, start, cc, i, ret;
	const struct ib_send_wr *bad_wr;
	unsigned char c;

	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		cc = sprintf(cb->start_buf, "rdma-ping-%d: ", ping);
		for (i = cc, c = start; i < cb->size; i++) {
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[cb->size - 1] = 0;

		krperf_format_send(cb, cb->start_dma_addr);
		if (cb->state == ERROR) {
			pr_err("krperf_format_send failed\n");
			break;
		}
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
			break;
		}

		/* Wait for server to ACK */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			pr_err("wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;
		}

		krperf_format_send(cb, cb->rdma_dma_addr);
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
			break;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			pr_err("wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}

		if (cb->validate)
			if (memcmp(cb->start_buf, cb->rdma_buf, cb->size)) {
				pr_err("data mismatch!\n");
				break;
			}

		if (cb->verbose)
			printk(KERN_INFO PFX "ping data (64B max): |%.64s|\n",
				cb->rdma_buf);
#ifdef SLOW_KRPERF
		wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
#endif
	}
}

static void krperf_rlat_test_client(struct krperf_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krperf_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR) {
		pr_err("krperf_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		pr_err("poll error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}
	if (wc.status) {
		pr_err("send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krperf_cq_event_handler(cb->cq, cb);
	}

	rlat_test(cb);
}

static void krperf_wlat_test_client(struct krperf_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krperf_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR) {
		pr_err("krperf_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		pr_err("poll error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}
	if (wc.status) {
		pr_err("send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krperf_cq_event_handler(cb->cq, cb);
	}

	wlat_test(cb);
}

static void krperf_bw_test_client(struct krperf_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krperf_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR) {
		pr_err("krperf_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		pr_err("post send error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		pr_err("poll error %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}
	if (wc.status) {
		pr_err("send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krperf_cq_event_handler(cb->cq, cb);
	}

	bw_test(cb);
}

/*
 * Manual qp flush test
 */
static void flush_qp(struct krperf_cb *cb)
{
	struct ib_send_wr wr = { 0 };
	const struct ib_send_wr *bad;
	struct ib_recv_wr recv_wr = { 0 };
	const struct ib_recv_wr *recv_bad;
	struct ib_wc wc;
	int ret;
	int flushed = 0;
	int ccnt = 0;

	rdma_disconnect(cb->cm_id);
	DEBUG_LOG("disconnected!\n");

	wr.opcode = IB_WR_SEND;
	wr.wr_id = 0xdeadbeefcafebabe;
	ret = ib_post_send(cb->qp, &wr, &bad);
	if (ret) {
		pr_err("%s post_send failed ret %d(%pe)\n", __func__, ret, ERR_PTR(ret));
		return;
	}

	recv_wr.wr_id = 0xcafebabedeadbeef;
	ret = krperf_ib_srq_rq_post_recv(cb, &recv_bad);
	if (ret) {
		pr_err("%s post_recv failed ret %d(%pe)\n", __func__, ret, ERR_PTR(ret));
		return;
	}

	/* poll until the flush WRs complete */
	do {
		ret = ib_poll_cq(cb->cq, 1, &wc);
		if (ret < 0) {
			pr_err("ib_poll_cq failed %d(%pe)\n", ret, ERR_PTR(ret));
			return;
		}
		if (ret == 0)
			continue;
		ccnt++;
		if (wc.wr_id == 0xdeadbeefcafebabe ||
		    wc.wr_id == 0xcafebabedeadbeef)
			flushed++;
	} while (flushed != 2);
	DEBUG_LOG("qp_flushed! ccnt %u\n", ccnt);
}

static unsigned long krperf_get_seconds(void)
{
	time64_t sec;

	sec = ktime_get_seconds();
	return (unsigned long)sec;
}

static void krperf_fr_test(struct krperf_cb *cb)
{
	struct ib_send_wr inv;
	const struct ib_send_wr *bad;
	struct ib_reg_wr fr;
	struct ib_wc wc;
	u8 key = 0;
	struct ib_mr *mr;
	int ret;
	int size = cb->size;
	int plen = (((size - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
	unsigned long start;
	int count = 0;
	int scnt = 0;
	struct scatterlist sg = {0};

	mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, plen);
	if (IS_ERR(mr)) {
		pr_err("ib_alloc_mr failed %ld(%pe)\n", PTR_ERR(mr), mr);
		return;
	}

	sg_dma_address(&sg) = (dma_addr_t)0xcafebabe0000ULL;
	sg_dma_len(&sg) = size;
	ret = ib_map_mr_sg(mr, &sg, 1, NULL, PAGE_SIZE);
	if (ret <= 0) {
		pr_err("ib_map_mr_sge err %d(%pe)\n", ret, ERR_PTR(ret));
		goto err2;
	}

	memset(&fr, 0, sizeof fr);
	fr.wr.opcode = IB_WR_REG_MR;
	fr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
	fr.mr = mr;
	fr.wr.next = &inv;

	memset(&inv, 0, sizeof inv);
	inv.opcode = IB_WR_LOCAL_INV;
	inv.send_flags = IB_SEND_SIGNALED;
	
	DEBUG_LOG("fr_test: stag index 0x%x plen %u size %u depth %u\n", mr->rkey >> 8, plen, cb->size, cb->txdepth);
	start = krperf_get_seconds();
	while (!cb->count || count <= cb->count) {
		if (signal_pending(current)) {
			pr_err("signal!\n");
			break;
		}
		if ((krperf_get_seconds() - start) >= 9) {
			DEBUG_LOG("fr_test: pausing 1 second! count %u latest size %u plen %u\n", count, size, plen);
			wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
			if (cb->state == ERROR)
				break;
			start = krperf_get_seconds();
		}	
		while (scnt < (cb->txdepth>>1)) {
			ib_update_fast_reg_key(mr, ++key);
			fr.key = mr->rkey;
			inv.ex.invalidate_rkey = mr->rkey;

			size = get_random_u32() % cb->size;
			if (size == 0)
				size = cb->size;
			sg_dma_len(&sg) = size;
			ret = ib_map_mr_sg(mr, &sg, 1, NULL, PAGE_SIZE);
			if (ret <= 0) {
				pr_err("ib_map_mr_sge err %d(%pe)\n", ret, ERR_PTR(ret));
				goto err2;
			}
			ret = ib_post_send(cb->qp, &fr.wr, &bad);
			if (ret) {
				pr_err("ib_post_send failed %d(%pe)\n", ret, ERR_PTR(ret));
				goto err2;	
			}
			scnt++;
		}

		ret = ib_poll_cq(cb->cq, 1, &wc);
		if (ret < 0) {
			pr_err("ib_poll_cq failed %d(%pe)\n", ret, ERR_PTR(ret));
			goto err2;	
		}
		if (ret == 1) {
			if (wc.status) {
				pr_err("completion error %u\n", wc.status);
				goto err2;
			}
			count++;
			scnt--;
		}
	}
err2:
	flush_qp(cb);
	DEBUG_LOG("fr_test: done!\n");
	ib_dereg_mr(mr);
}

static int krperf_connect_client(struct krperf_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		pr_err("rdma_connect error %d(%pe)\n", ret, ERR_PTR(ret));
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		pr_err("wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

static int krperf_bind_client(struct krperf_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		pr_err("rdma_resolve_addr error %d(%pe)\n", ret, ERR_PTR(ret));
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED) {
		pr_err("addr/route resolution did not resolve: state %d\n",
		       cb->state);
		return -EINTR;
	}

	if (!reg_supported(cb->cm_id->device))
		return -EINVAL;

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static void krperf_run_client(struct krperf_cb *cb)
{
	const struct ib_recv_wr *bad_wr;
	int ret;

	/* set type of service, if any */
	if (cb->tos != 0)
		rdma_set_service_type(cb->cm_id, cb->tos);

	ret = krperf_bind_client(cb);
	if (ret)
		return;

	ret = krperf_setup_qp(cb, cb->cm_id);
	if (ret) {
		pr_err("setup_qp failed: %d(%pe)\n", ret, ERR_PTR(ret));
		return;
	}

	ret = krperf_setup_buffers(cb);
	if (ret) {
		pr_err("krperf_setup_buffers failed: %d(%pe)\n", ret, ERR_PTR(ret));
		goto err1;
	}

	ret = krperf_ib_srq_rq_post_recv(cb, &bad_wr);
	if (ret) {
		pr_err("krperf_ib_srq_rq_post_recv failed: %d(%pe)\n", ret, ERR_PTR(ret));
		goto err2;
	}

	ret = krperf_connect_client(cb);
	if (ret) {
		pr_err("connect error %d(%pe)\n", ret, ERR_PTR(ret));
		goto err2;
	}

	if (cb->wlat)
		krperf_wlat_test_client(cb);
	else if (cb->rlat)
		krperf_rlat_test_client(cb);
	else if (cb->bw)
		krperf_bw_test_client(cb);
	else if (cb->frtest)
		krperf_fr_test(cb);
	else
		krperf_test_client(cb);
	rdma_disconnect(cb->cm_id);
err2:
	krperf_free_buffers(cb);
err1:
	krperf_free_qp(cb);
}

static int krperf_parse(char *cmd, struct krperf_cb *cb)
{
	unsigned long optint;
	char *optarg;
	int ret = 0;
	char *scope;
	int op;

	while ((op = krperf_getopt("krperf", &cmd, krperf_opts, NULL, &optarg,
			      &optint)) != 0) {
		switch (op) {
		case 'a':
			cb->addr_str = optarg;
			in4_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET;
			DEBUG_LOG("ipaddr (%s)\n", optarg);
			break;
		case 'A':
			cb->addr_str = optarg;
			scope = strstr(optarg, "%");
			if (scope != NULL) {
				*scope++ = 0;
				strncpy(cb->ip6_ndev_name, scope,
					sizeof(cb->ip6_ndev_name));
				/* force zero-termination */
				cb->ip6_ndev_name[
				        sizeof(cb->ip6_ndev_name) - 1] = 0;
			}
			in6_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET6;
			DEBUG_LOG("ipv6addr (%s)\n", optarg);
			break;
		case 'p':
			cb->port = htons(optint);
			DEBUG_LOG("port %d\n", (int)optint);
			break;
		case 'P':
			cb->poll = 1;
			DEBUG_LOG("server\n");
			break;
		case 's':
			cb->server = 1;
			DEBUG_LOG("server\n");
			break;
		case 'c':
			cb->server = 0;
			DEBUG_LOG("client\n");
			break;
		case 'S':
			cb->size = optint;
			if ((cb->size < 1) || (cb->size > RPING_BUFSIZE)) {
				pr_err("Invalid size %d (valid range is 1 to %d)\n",
				       cb->size, RPING_BUFSIZE);
				ret = EINVAL;
			} else {
				DEBUG_LOG("size %d\n", (int)optint);
			}
			break;
		case 'C':
			cb->count = optint;
			if (cb->count < 0) {
				pr_err("Invalid count %d\n", cb->count);
				ret = EINVAL;
			} else {
				DEBUG_LOG("count %d\n", (int) cb->count);
			}
			break;
		case 'v':
			cb->verbose++;
			DEBUG_LOG("verbose\n");
			break;
		case 'V':
			cb->validate++;
			DEBUG_LOG("validate data\n");
			break;
		case 'l':
			cb->wlat++;
			break;
		case 'L':
			cb->rlat++;
			break;
		case 'B':
			cb->bw++;
			break;
		case 'd':
			cb->duplex++;
			break;
		case 'I':
			cb->server_invalidate = 1;
			break;
		case 't':
			cb->tos = optint;
			DEBUG_LOG("type of service, tos=%d\n", (int) cb->tos);
			break;
		case 'T':
			cb->txdepth = optint;
			DEBUG_LOG("txdepth %d\n", (int) cb->txdepth);
			break;
		case 'Z':
			cb->local_dma_lkey = 1;
			DEBUG_LOG("using local dma lkey\n");
			break;
		case 'R':
			cb->read_inv = 1;
			DEBUG_LOG("using read-with-inv\n");
			break;
		case 'f':
			cb->frtest = 1;
			DEBUG_LOG("fast-reg test!\n");
			break;
		case 'q':
			cb->use_srq = true;
			cb->srq = NULL;
			break;
		default:
			pr_err("unknown opt %s\n", optarg);
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

int krperf_doit(char *cmd)
{
	struct krperf_cb *cb;
	//int op;
	int ret = 0;
//	char *optarg;
//	char *scope;
//	unsigned long optint;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	mutex_lock(&krperf_mutex);
	list_add_tail(&cb->list, &krperf_cbs);
	mutex_unlock(&krperf_mutex);

	cb->server = -1;
	cb->state = IDLE;
	cb->size = 64;
	cb->txdepth = RPING_SQ_DEPTH;
	init_waitqueue_head(&cb->sem);
#if 0
	while ((op = krperf_getopt("krperf", &cmd, krperf_opts, NULL, &optarg,
			      &optint)) != 0) {
		switch (op) {
		case 'a':
			cb->addr_str = optarg;
			in4_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET;
			DEBUG_LOG("ipaddr (%s)\n", optarg);
			break;
		case 'A':
			cb->addr_str = optarg;
			scope = strstr(optarg, "%");
			if (scope != NULL) {
				*scope++ = 0;
				strncpy(cb->ip6_ndev_name, scope,
					sizeof(cb->ip6_ndev_name));
				/* force zero-termination */
				cb->ip6_ndev_name[
				        sizeof(cb->ip6_ndev_name) - 1] = 0;
			}
			in6_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET6;
			DEBUG_LOG("ipv6addr (%s)\n", optarg);
			break;
		case 'p':
			cb->port = htons(optint);
			DEBUG_LOG("port %d\n", (int)optint);
			break;
		case 'P':
			cb->poll = 1;
			DEBUG_LOG("server\n");
			break;
		case 's':
			cb->server = 1;
			DEBUG_LOG("server\n");
			break;
		case 'c':
			cb->server = 0;
			DEBUG_LOG("client\n");
			break;
		case 'S':
			cb->size = optint;
			if ((cb->size < 1) ||
			    (cb->size > RPING_BUFSIZE)) {
				pr_err("Invalid size %d (valid range is 1 to %d)\n",
				       cb->size, RPING_BUFSIZE);
				ret = EINVAL;
			} else
				DEBUG_LOG("size %d\n", (int)optint);
			break;
		case 'C':
			cb->count = optint;
			if (cb->count < 0) {
				pr_err("Invalid count %d\n", cb->count);
				ret = EINVAL;
			} else
				DEBUG_LOG("count %d\n", (int) cb->count);
			break;
		case 'v':
			cb->verbose++;
			DEBUG_LOG("verbose\n");
			break;
		case 'V':
			cb->validate++;
			DEBUG_LOG("validate data\n");
			break;
		case 'l':
			cb->wlat++;
			break;
		case 'L':
			cb->rlat++;
			break;
		case 'B':
			cb->bw++;
			break;
		case 'd':
			cb->duplex++;
			break;
		case 'I':
			cb->server_invalidate = 1;
			break;
		case 't':
			cb->tos = optint;
			DEBUG_LOG("type of service, tos=%d\n", (int) cb->tos);
			break;
		case 'T':
			cb->txdepth = optint;
			DEBUG_LOG("txdepth %d\n", (int) cb->txdepth);
			break;
		case 'Z':
			cb->local_dma_lkey = 1;
			DEBUG_LOG("using local dma lkey\n");
			break;
		case 'R':
			cb->read_inv = 1;
			DEBUG_LOG("using read-with-inv\n");
			break;
		case 'f':
			cb->frtest = 1;
			DEBUG_LOG("fast-reg test!\n");
			break;
		case 'q':
			cb->use_srq = true;
			cb->srq = NULL;
			break;
		default:
			pr_err("unknown opt %s\n", optarg);
			ret = -EINVAL;
			break;
		}
	}
#endif
	ret = krperf_parse(cmd, cb);
	if (ret)
		goto out;

	if (cb->server == -1) {
		pr_err("must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->server && cb->frtest) {
		pr_err("must be client to run frtest\n");
		ret = -EINVAL;
		goto out;
	}

	if ((cb->frtest + cb->bw + cb->rlat + cb->wlat) > 1) {
		pr_err("Pick only one test: fr, bw, rlat, wlat\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->wlat || cb->rlat || cb->bw) {
		pr_err("wlat, rlat, and bw tests only support mem_mode MR - which is no longer supported\n");
		ret = -EINVAL;
		goto out;
	}

	cb->cm_id = rdma_create_id(&init_net, krperf_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		pr_err("rdma_create_id error %d(%pe)\n", ret, ERR_PTR(ret));
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);

	/* Add ib_req_notify_cq workqueue handler */
	INIT_WORK(&cb->ib_req_notify_cq_work, krperf_ib_req_notify_cq_handler);

	if (cb->server)
		krperf_run_server(cb);
	else
		krperf_run_client(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
out:
	mutex_lock(&krperf_mutex);
	list_del(&cb->list);
	mutex_unlock(&krperf_mutex);
	kfree(cb);
	return ret;
}

static int __init krperf_init(void)
{
	struct proc_dir_entry *krperf_proc = NULL;

	DEBUG_LOG("krperf_init\n");
	krperf_proc = krperf_proc_create();
	if (krperf_proc == NULL) {
		pr_err("cannot create /proc/krperf\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit krperf_exit(void)
{
	DEBUG_LOG("krperf_exit\n");
	remove_proc_entry("krperf", NULL);
}

module_init(krperf_init);
module_exit(krperf_exit);
