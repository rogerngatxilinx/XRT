/*******************************************************************************
 *
 * Xilinx QDMA IP Core Linux Driver
 * Copyright(c) 2017 Xilinx, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "LICENSE".
 *
 * Lizhi Hou <lizhi.hou@xilinx.com>
 *
 ******************************************************************************/

#define pr_fmt(fmt)     KBUILD_MODNAME ":%s: " fmt, __func__

#include <linux/vmalloc.h>
#include <linux/sched.h>
#include "thread.h"
#include "qdma_descq.h"
#include "qdma_intr.h"
#include "qdma_wq.h"
#include "qdma_context.h"

#define	QDMA_ST_H2C_MASK	0x3f

int qdma_wq_destroy(struct qdma_wq *queue)
{
	int			ret = 0;

	reinit_completion(&queue->wq_comp);

	if (queue->flag & QDMA_WQ_QUEUE_STARTED) {
		ret = qdma_queue_stop(queue->dev_hdl, queue->qhdl, NULL, 0);
		if (ret < 0) {
			pr_err("Stop queue failed ret=%d", ret);
			goto failed;
		}
		queue->flag &= ~QDMA_WQ_QUEUE_STARTED;
	}

	if (queue->flag & QDMA_WQ_QUEUE_ADDED) {
		ret = qdma_queue_remove(queue->dev_hdl, queue->qhdl, NULL, 0);
		if (ret < 0) {
			pr_err("Remove queue failed ret=%d", ret);
			goto failed;
		}
		queue->flag &= ~QDMA_WQ_QUEUE_ADDED;
	}

	if (queue->flag & QDMA_WQ_INITIALIZED) {
#if 0
		mutex_lock(&queue->wq_lock);
		if (queue->wq_head == queue->wq_pending) {
			mutex_unlock(&queue->wq_lock);
		} else {
			mutex_unlock(&queue->wq_lock);
			wait_for_completion(&queue->wq_comp);
		}
#endif
		queue->flag &= ~QDMA_WQ_INITIALIZED;
	}

	if (queue->wq)
		vfree(queue->wq);

	if (queue->sg_cache)
		vfree(queue->sg_cache);

	return 0;

failed:
	return ret;
}

int qdma_wq_create(unsigned long dev_hdl, struct qdma_queue_conf *qconf,
	struct qdma_wq *queue, u32 priv_data_len)
{
	struct qdma_wqe *wqe;
	int	i, ret;

	queue->dev_hdl = dev_hdl;
	ret = qdma_queue_add(dev_hdl, qconf, &queue->qhdl, NULL, 0);
	if (ret < 0) {
		pr_err("Creating queue failed, ret=%d", ret);
		goto failed;
	}
	queue->flag |= QDMA_WQ_QUEUE_ADDED;

	ret = qdma_queue_start(dev_hdl, queue->qhdl, NULL, 0);
	if (ret < 0) {
		pr_err("Starting queue failed, ret=%d", ret);
		goto failed;
	}
	queue->flag |= QDMA_WQ_QUEUE_STARTED;

	queue->qconf = qdma_queue_get_config(dev_hdl, queue->qhdl, NULL, 0);
	if (!queue->qconf) {
		pr_err("Query queue config failed");
		ret = -EFAULT;
		goto failed;
	}
	if (queue->qconf->st && queue->qconf->c2h &&
		queue->qconf->c2h_bufsz != PAGE_SIZE) {
		pr_err("Unsupported c2h_bufsz %d\n", queue->qconf->c2h_bufsz);
		ret = -EINVAL;
		goto failed;
	}
	queue->qlen = queue->qconf->rngsz;
	if (queue->qlen != (1 << (ffs(queue->qlen) - 1))) {
		pr_err("Invalid qlen %d", queue->qlen);
		ret = -EINVAL;
		goto failed;
	}

	queue->wq_len = queue->qlen << 3 ;
	queue->wqe_sz = roundup(sizeof (*queue->wq) + priv_data_len, 8);
	queue->wq = vzalloc(queue->wqe_sz * queue->wq_len);
	if (!queue->wq) {
		pr_err("Alloc wq failed");
		ret = -ENOMEM;
		goto failed;
	}
	queue->priv_data_len = priv_data_len;

	for (i = 0; i < queue->wq_len; i++) {
		wqe = (struct qdma_wqe *)((char *)queue->wq + queue->wqe_sz * i);
		wqe->queue = queue;
		init_waitqueue_head(&wqe->req_comp);
	}

	queue->sg_cache = vzalloc(queue->qlen * sizeof (*queue->sg_cache));
	if (!queue->sg_cache) {
		pr_err("Alloc sg_cache failed");
		ret = -ENOMEM;
		goto failed;
	}
	queue->sgc_avail = queue->qlen - 1;
	queue->sgc_len = queue->qlen;
	queue->sgc_pidx = 0;

	spin_lock_init(&queue->wq_lock);
	init_completion(&queue->wq_comp);
	queue->flag |= QDMA_WQ_INITIALIZED;
	return 0;

failed:
	qdma_wq_destroy(queue);
	return ret;
}

static int qdma_wq_reset(struct qdma_wq *queue)
{
	struct xlnx_dma_dev     *xdev;
        struct qdma_descq       *descq;
	struct qdma_wqe *wqe;
	int	i, ret;

        xdev = (struct xlnx_dma_dev *)queue->dev_hdl;
        descq = qdma_device_get_descq_by_id(xdev, queue->qhdl, NULL, 0, 0);

	reinit_completion(&queue->wq_comp);

	if (queue->flag & QDMA_WQ_QUEUE_STARTED) {
		ret = qdma_queue_stop(queue->dev_hdl, queue->qhdl, NULL, 0);
		if (ret < 0) {
			pr_err("Stop queue failed ret=%d", ret);
			goto failed;
		}
		queue->flag &= ~QDMA_WQ_QUEUE_STARTED;

		memset(queue->wq, 0, queue->wqe_sz * queue->wq_len);
		for (i = 0; i < queue->wq_len; i++) {
			wqe = (struct qdma_wqe *)((char *)queue->wq +
				queue->wqe_sz * i);
			wqe->queue = queue;
			init_waitqueue_head(&wqe->req_comp);
		}

		reinit_completion(&queue->wq_comp);
		queue->wq_free = 0;
		queue->wq_pending = 0;
		queue->wq_unproc = 0;
		queue->sgc_avail = queue->qlen - 1;
		queue->sgc_pidx = 0;
		memset(queue->sg_cache, 0, queue->qlen *
			sizeof(*queue->sg_cache));

		ret = qdma_queue_start(queue->dev_hdl, queue->qhdl, NULL, 0);
		if (ret < 0) {
			pr_err("Starting queue failed, ret=%d", ret);
			goto failed;
		}
		queue->flag |= QDMA_WQ_QUEUE_STARTED;
	}
failed:

	return ret;
}

static int descq_mm_fill(struct qdma_descq *descq, struct qdma_wqe *wqe)
{
	struct qdma_mm_desc	*desc;
	struct qdma_sgt_req_cb	*cb;
	struct scatterlist	*sg, *next = NULL;
	dma_addr_t		dma_addr;
	loff_t			off;
	ssize_t			len, total = 0;
	int			i;

	if (!descq->avail) {
		if (descq->conf.irq_en)
			goto update_pidx;
		return -ENOENT;
	}

	desc = (struct qdma_mm_desc *)descq->desc + descq->pidx;
	cb = qdma_req_cb_get(&wqe->wr.req);
	desc->flag_len |= (1 << S_DESC_F_SOP);
	sg = wqe->unproc_sg;
	for(i = 0; i < wqe->unproc_sg_num; i++, sg = next) {
		off = 0;
		len = sg->length;
		if (wqe->unproc_sg_off) {
			off += wqe->unproc_sg_off;
			len -= wqe->unproc_sg_off;
		}
		len = min_t(ssize_t, len, wqe->unproc_bytes);
		if (len > QDMA_DESC_BLEN_MAX) {
			wqe->unproc_sg_off += QDMA_DESC_BLEN_MAX;
			i--;
			len = QDMA_DESC_BLEN_MAX;
			next = sg;
		} else {
			wqe->unproc_sg_off = 0;
			next = sg_next(sg);
		}

		dma_addr = sg_dma_address(sg) + off;

		desc->rsvd1 = 0UL;
		desc->rsvd0 = 0U;

		if (descq->conf.c2h) {
			desc->src_addr = wqe->unproc_ep_addr;
			desc->dst_addr = dma_addr;
		} else {
			desc->dst_addr = wqe->unproc_ep_addr;
			desc->src_addr = dma_addr;
		}
		desc->flag_len = len;
		desc->flag_len |= (1 << S_DESC_F_DV);
		pr_debug("sg %p: i %d, rest %d, dma %p, len %x next %p - "
			"desc dma %p, len %lx, ep %p \n", sg, i,
			wqe->unproc_sg_num, (void *)sg->dma_address,
			sg->length, sg_next(sg), (void *)dma_addr, len,
			(void *)wqe->unproc_ep_addr);
		
		descq->pidx++;
		descq->pidx &= descq->conf.rngsz - 1;
		descq->avail--;
		cb->desc_nr++;

		wqe->unproc_bytes -= len;
		wqe->unproc_ep_addr += len;
		total += len;
		if (wqe->unproc_bytes == 0 || descq->avail == 0) {
			wqe->wr.req.count = wqe->wr.len - wqe->unproc_bytes -
				wqe->done_bytes;
			if (!cb->pending) {
				list_add_tail(&cb->list, &descq->pend_list);
				cb->pending = true;
			}
			wqe->state = QDMA_WQE_STATE_PENDING;
			i++;
			break;
		}
		desc = (struct qdma_mm_desc *)descq->desc + descq->pidx;
	}
	desc->flag_len |= (1 << S_DESC_F_EOP);
	BUG_ON(i == wqe->unproc_sg_num && wqe->unproc_bytes != 0);

	wqe->unproc_sg = next;
	wqe->unproc_sg_num =  wqe->unproc_sg_num - i;

update_pidx:
	if (descq->conf.c2h)
		descq_c2h_pidx_update(descq, descq->pidx);
	else
		descq_h2c_pidx_update(descq, descq->pidx);

	wqe->queue->proc_nbytes += total;

	return (descq->avail == 0) ? -ENOENT : 0;
}

static int descq_st_h2c_fill(struct qdma_descq *descq, struct qdma_wqe *wqe)
{
#define	PIDX_UPDATE_MASK	0x7
	struct qdma_h2c_desc	*desc;
	struct qdma_sgt_req_cb	*cb;
	struct scatterlist	*sg, *next = NULL;
	dma_addr_t		dma_addr;
	loff_t			off;
	ssize_t			len, total = 0;
	int			i;

	if (descq->q_state != Q_STATE_ONLINE)
		return -EINVAL;

	if (!descq->avail) {
		if (descq->conf.irq_en)
			goto update_pidx;
		return -ENOENT;
	}

	cb = qdma_req_cb_get(&wqe->wr.req);
	sg = wqe->unproc_sg;
	desc = (struct qdma_h2c_desc *)descq->desc + descq->pidx;
	desc->flags = S_H2C_DESC_F_SOP;
	for(i = 0; i < wqe->unproc_sg_num; i++, sg = next) {
		off = 0;
		len = sg->length;
		if (wqe->unproc_sg_off) {
			off += wqe->unproc_sg_off;
			len -= wqe->unproc_sg_off;
		}
		len = min_t(ssize_t, len, wqe->unproc_bytes);
		if (len > PAGE_SIZE) {
			wqe->unproc_sg_off += PAGE_SIZE;
			i--;
			len = PAGE_SIZE;
			next = sg;
		} else {
			wqe->unproc_sg_off = 0;
			next = sg_next(sg);
		}
		if ((len & QDMA_ST_H2C_MASK) &&
			i + 1 < wqe->unproc_sg_num) {
			/* should never hit here */
			pr_err("Invalid alignment for st h2c sg_num:%d, "
				"len %ld\n", i, len);
			return -EINVAL;
		}

		dma_addr = sg_dma_address(sg) + off;
		desc->src_addr = dma_addr;
		desc->len = len;
		if (descq->xdev->stm_en) {
			desc->pld_len = len;

			desc->cdh_flags = (1 << S_H2C_DESC_F_ZERO_CDH);
			desc->cdh_flags |= V_H2C_DESC_NUM_GL(1);
			desc->cdh_flags |= (1 << S_H2C_DESC_F_REQ_WRB);
		}
		pr_debug("sg %p: i %d, rest %d, dma %p, len %x next %p - "
			"desc dma %p, len %lx\n", sg, descq->pidx,
			wqe->unproc_sg_num, (void *)sg->dma_address,
			sg->length, sg_next(sg), (void *)dma_addr, len);

		pr_debug("idx:%d, len:%ld, addr %p, avail %d\n",
			descq->pidx, len, (void *)dma_addr, descq->avail);

		descq->pidx++;
		descq->pidx &= descq->conf.rngsz - 1;
		descq->avail--;
		cb->desc_nr++;

		wqe->unproc_bytes -= len;
		total += len;
		if (wqe->unproc_bytes == 0 || descq->avail == 0) {
			i++;
			break;
		}
		desc = (struct qdma_h2c_desc *)descq->desc + descq->pidx;
		desc->flags = 0;
		if (!(descq->pidx & PIDX_UPDATE_MASK))
			descq_h2c_pidx_update(descq, descq->pidx);
	}
	/* BUG_ON(i == wqe->unproc_sg_num && wqe->unproc_bytes != 0); */

	pr_debug("Out of loop %d, ring size %d\n", descq->pidx,
		descq->conf.rngsz);
	pr_debug("unproc_sg_num %d, uproc_bytes %lld\n", wqe->unproc_sg_num,
		wqe->unproc_bytes);

	desc->flags |= S_H2C_DESC_F_EOP;
	if (descq->xdev->stm_en && wqe->unproc_bytes == 0 &&
		wqe->wr.req.eot)
		desc->cdh_flags |= (1 << S_H2C_DESC_F_EOT);

	wqe->unproc_sg = next;
	wqe->unproc_sg_num =  wqe->unproc_sg_num - i;
	wqe->wr.req.count = wqe->wr.len - wqe->unproc_bytes -
		wqe->done_bytes;
	if (!cb->pending) {
		list_add_tail(&cb->list, &descq->pend_list);
		cb->pending = true;
	}
	wqe->state = QDMA_WQE_STATE_PENDING;

update_pidx:
	descq_h2c_pidx_update(descq, descq->pidx);

	wqe->queue->proc_nbytes += total;

	return (descq->avail == 0) ? -ENOENT : 0;
}

static int descq_st_c2h_fill(struct qdma_descq *descq, struct qdma_wqe *wqe)
{
	struct qdma_sw_sg	*sgc, *next_sgc;
	struct qdma_wq		*queue;
	struct qdma_sgt_req_cb	*cb;
	struct scatterlist	*sg;
	loff_t			off;
	ssize_t			len, total = 0;
	u32			pidx;
	int			i;

	queue = wqe->queue;
	if (!queue->sgc_avail)
		return -ENOENT;

	cb = qdma_req_cb_get(&wqe->wr.req);

	if (cb->sg_idx) {
		wqe->wr.req.sgcnt -= cb->sg_idx;
		pidx = queue->sgc_pidx - wqe->wr.req.sgcnt;
		pidx &= queue->sgc_len - 1;
	} else
		pidx = queue->sgc_pidx;

	memset(cb, 0, sizeof(*cb));

	wqe->wr.req.sgl = queue->sg_cache + pidx;
	sgc = queue->sg_cache + queue->sgc_pidx;

	sg = wqe->unproc_sg;
	for(i = 0; i < wqe->unproc_sg_num; i++, sg = sg_next(sg)) {
		off = sg->offset;
		len = sg->length;
		if (wqe->unproc_sg_off) {
			off += wqe->unproc_sg_off;
			len -= wqe->unproc_sg_off;
			wqe->unproc_sg_off = 0;
		}
		len = min_t(ssize_t, len, wqe->unproc_bytes);

		sgc->pg = sg_page(sg);
		sgc->offset = off;
		sgc->len = len;
		sgc->dma_addr = 0;

		pr_debug("sg %p: i %d, rest %d, dma %p, len %x next %p - "
			"desc idx %d, len %lx\n", sg, i,
			wqe->unproc_sg_num, (void *)sg->dma_address,
			sg->length, sg_next(sg), queue->sgc_pidx, len);

		wqe->wr.req.sgcnt++;
		queue->sgc_pidx++;
		queue->sgc_pidx &= queue->sgc_len - 1;
		queue->sgc_avail--;
		next_sgc = queue->sg_cache + queue->sgc_pidx;

		sgc->next = next_sgc;

		wqe->unproc_bytes -= len;
		total += len;
		if (wqe->unproc_bytes == 0 || queue->sgc_avail == 0) {
			sgc->next = NULL;
			wqe->wr.req.count = wqe->wr.len - wqe->unproc_bytes -
				wqe->done_bytes;
			i++;
			sg = sg_next(sg);
			wqe->state = QDMA_WQE_STATE_PENDING;
			break;
		}
		sgc = next_sgc;
	}
	//desc->flags |= S_H2C_DESC_F_EOP;
	BUG_ON(i == wqe->unproc_sg_num && wqe->unproc_bytes != 0);

	wqe->unproc_sg = sg;
	wqe->unproc_sg_num =  wqe->unproc_sg_num - i;

	cb->left = wqe->wr.req.count;
	cb->offset = 0;

	if (!cb->pending) {
		list_add_tail(&cb->list, &descq->pend_list);
		cb->pending = true;
	}

	//wqe->wr.req.fp_done = NULL;
	pr_debug("C2H request total %ld, sg_cnt %d, sgc idx %d,eot %d\n",
		total, i, queue->sgc_pidx, wqe->wr.req.eot);

	queue->proc_nbytes += total;

	if (descq->conf.irq_en)
		schedule_work(&descq->work);

	return (queue->sgc_avail == 0) ? -ENOENT : 0;
}

static inline bool wqe_done(struct qdma_wqe *wqe)
{
	struct qdma_sgt_req_cb  *cb;

	cb = qdma_req_cb_get(&wqe->wr.req);

	return (cb->done || cb->canceled) ? true : false;
}

static void descq_proc_req(struct qdma_wq *queue)
{
	struct qdma_wqe		*wqe;
	struct xlnx_dma_dev	*xdev;
	struct qdma_descq	*descq;
	struct qdma_flq		*flq;
	u32			pidx;
	int			ret;

	xdev = (struct xlnx_dma_dev *)queue->dev_hdl;
	descq = qdma_device_get_descq_by_id(xdev, queue->qhdl, NULL, 0, 0);

	wqe = wq_next_unproc(queue);
	pr_debug("QUEUE%d%s wqe %p,%lld bytes,unproc %d,free %d,pending %d\n",
		queue->qconf->qidx, queue->qconf->c2h ? "R" : "W", wqe,
		wqe?wqe->unproc_bytes:0, queue->wq_unproc, queue->wq_free,
		queue->wq_pending);

	if (!wqe && descq->conf.irq_en &&
		((queue->wq_pending != queue->wq_unproc) ||
		!wqe_done(_wqe(queue, queue->wq_pending)))) {
		if (descq->conf.c2h) {
			if (descq->conf.st) {
				flq = &descq->flq;
				pidx = ring_idx_decr(flq->pidx_pend, 1,
					flq->size);
				descq_c2h_pidx_update(descq, pidx);
			} else
				descq_c2h_pidx_update(descq, descq->pidx);
		} else
			descq_h2c_pidx_update(descq, descq->pidx);
	}

	while (wqe) {
		if (wqe->state == QDMA_WQE_STATE_CANCELED ||
			wqe->state == QDMA_WQE_STATE_CANCELED_HW)
			goto next;
		lock_descq(descq);
		if(descq->conf.st) {
			if (descq->conf.c2h) {
				ret = descq_st_c2h_fill(descq, wqe);
			} else {
				ret = descq_st_h2c_fill(descq, wqe);
			}
		} else {
			ret = descq_mm_fill(descq, wqe);
		}
		unlock_descq(descq);

		if (descq->wbthp)
			qdma_kthread_wakeup(descq->wbthp);

		if (ret)
			break;
next:
		wqe = wq_next_unproc(queue);
	}
}

static int qdma_wqe_cancel(struct qdma_request *req)
{
	struct qdma_wqe                 *wqe;
        struct qdma_wq                  *queue;
        struct qdma_complete_event      compl_evt;

        wqe = container_of(req, struct qdma_wqe, wr.req);
        queue = wqe->queue;

	pr_debug("Cancel req %p, queue %p\n", req, queue);
	if (wqe->wr.kiocb) {
		compl_evt.done_bytes = 0;
		compl_evt.error = QDMA_EVT_CANCELED;
		compl_evt.kiocb = wqe->wr.kiocb;
		compl_evt.req_priv = wqe->priv_data;
		wqe->wr.complete(&compl_evt);
	}

	return 0;
}

static int qdma_wqe_complete(struct qdma_request *req, unsigned int bytes_done,
	int err)
{
	struct qdma_wqe			*wqe;
	struct qdma_wq			*queue;
	struct qdma_complete_event	compl_evt;
	struct qdma_sgt_req_cb		*cb;

	wqe = container_of(req, struct qdma_wqe, wr.req);
	queue = wqe->queue;
	cb = qdma_req_cb_get(req);

	pr_debug("WB:  %s %x bytes, wqe %p\n",
		wqe->wr.req.write? "write": "read", bytes_done, wqe);
	spin_lock_bh(&queue->wq_lock);
	wqe->done_bytes += bytes_done;
	queue->sgc_avail += req->sgcnt;
	queue->wb_nbytes += bytes_done;
	if ((err != 0 || (req->eot  && cb->c2h_eot) ||
		wqe->wr.len == wqe->done_bytes) &&
		wqe->state != QDMA_WQE_STATE_CANCELED &&
		wqe->state != QDMA_WQE_STATE_CANCELED_HW) {
		queue->compl_nbytes += wqe->done_bytes;
		queue->compl_num++;
		wqe->state = QDMA_WQE_STATE_DONE;
		cb->pending = false;
		list_del(&cb->list);
		if (!wqe->wr.kiocb) {
			wake_up(&wqe->req_comp);
		} else {
			compl_evt.done_bytes = wqe->done_bytes;
			compl_evt.error = err ? QDMA_EVT_ERROR :
				QDMA_EVT_SUCCESS;
			compl_evt.kiocb = wqe->wr.kiocb;
			compl_evt.req_priv = wqe->priv_data;
			wqe->wr.complete(&compl_evt);
		}
		wqe = wq_next_pending(queue);
	} else if (wqe->state == QDMA_WQE_STATE_CANCELED_HW) {
		if (queue->qconf->st && queue->qconf->c2h) {
			cb->pending = false;
			wqe = wq_next_pending(queue);
		} else if (wqe->unproc_bytes + wqe->done_bytes ==
			wqe->wr.len) {
			pr_debug("Cancel notified\n");
			cb->pending = false;
			list_del(&cb->list);
			if (!wqe->wr.kiocb)
				wake_up(&wqe->req_comp);
			wqe = wq_next_pending(queue);
		}
	}

	descq_proc_req(queue);
	spin_unlock_bh(&queue->wq_lock);

	return 0;
}

int qdma_cancel_req(struct qdma_wq *queue, struct kiocb *kiocb)
{
	struct qdma_wqe			*wqe;
	struct xlnx_dma_dev		*xdev;
	struct qdma_descq		*descq;
	struct qdma_sgt_req_cb		*cb;

        xdev = (struct xlnx_dma_dev *)queue->dev_hdl;
        descq = qdma_device_get_descq_by_id(xdev, queue->qhdl, NULL, 0, 0);

	wqe = kiocb->private;

	cb = qdma_req_cb_get(&wqe->wr.req);
	descq_cancel_req(descq, &wqe->wr.req);
	if (wqe->state == QDMA_WQE_STATE_PENDING) {
		wqe->state = QDMA_WQE_STATE_CANCELED_HW;
	} else {
		wqe->state = QDMA_WQE_STATE_CANCELED;
	}
	schedule_work(&descq->work);

	return 0;
}

ssize_t qdma_wq_post(struct qdma_wq *queue, struct qdma_wr *wr)
{
	struct qdma_wqe		*wqe;
	struct qdma_sgt_req_cb	*cb;
	struct scatterlist	*sg;
	loff_t			off;
	int			sg_num;
	int			i;
	ssize_t			ret = 0;

	sg_num = wr->sgt->nents;
	off = wr->offset;
	for_each_sg(wr->sgt->sgl, sg, sg_num, i) {
		if (off < sg->length) {
			break;
		}
		off -= sg->length;
	}
	if (queue->qconf->st && wr->write &&
		((sg->length - off) & QDMA_ST_H2C_MASK) && i + 1 < sg_num) {
		pr_err("Invalid alignment.h2c buffer has to be 64B aligned"
			"offset: %lld\n", off);
		return -EINVAL;
	}
	BUG_ON(i == sg_num && off > sg->length);
	sg_num -= i;

	spin_lock_bh(&queue->wq_lock);
	wqe = wq_next_free(queue);
	if (!wqe) {
		ret = -EAGAIN;
		goto again;
	}
	wqe->state = QDMA_WQE_STATE_SUBMITTED;

	memcpy(&wqe->wr, wr, sizeof (*wr));
	wqe->done_bytes = 0;
	wqe->unproc_bytes = wr->len;
	wqe->unproc_sg_num = sg_num;
	wqe->unproc_ep_addr = wr->req.ep_addr;
	wqe->unproc_sg = sg;
	wqe->unproc_sg_off = off;
	wqe->wr.req.fp_done = qdma_wqe_complete;
	wqe->wr.req.fp_cancel = qdma_wqe_cancel;
	wqe->wr.req.write = wr->write;
	wqe->wr.req.eot = wr->eot;
	if (wqe->wr.kiocb)
		wqe->wr.kiocb->private = wqe;

	if (wr->priv_data) {
		memcpy(wqe->priv_data, wr->priv_data, queue->priv_data_len);
	}

	cb = qdma_req_cb_get(&wqe->wr.req);
	memset(cb, 0, QDMA_REQ_OPAQUE_SIZE);
	qdma_waitq_init(&cb->wq);;

	queue->req_nbytes += wr->len;
	queue->req_num++;
again:
	descq_proc_req(queue);
	if (!ret) {
		if (!wqe->wr.kiocb) {
			spin_unlock_bh(&queue->wq_lock);
			ret = wait_event_killable(wqe->req_comp,
				(wqe->state == QDMA_WQE_STATE_DONE));
			spin_lock_bh(&queue->wq_lock);
			if (ret < 0) {
				pr_debug("Reset Queue\n");
				spin_unlock_bh(&queue->wq_lock);
				qdma_wq_reset(queue);
				spin_lock_bh(&queue->wq_lock);
#if 0
				if (wqe->state == QDMA_WQE_STATE_PENDING) {
					wqe->state = QDMA_WQE_STATE_CANCELED_HW;
					spin_unlock_bh(&queue->wq_lock);
					if (!queue->qconf->st ||
						!queue->qconf->c2h)
						wait_event_timeout(
							wqe->req_comp,
							(wqe->state ==
							QDMA_WQE_STATE_DONE),
							msecs_to_jiffies(
							QDMA_CANCEL_TIMEOUT *
							1000));
					spin_lock_bh(&queue->wq_lock);
				} else {
					wqe->state = QDMA_WQE_STATE_CANCELED;
				}
#endif
			}
			ret = wqe->done_bytes;
		} else {
			ret = wr->len;
		}
	}
		
	spin_unlock_bh(&queue->wq_lock);

	return ret;
}

void qdma_wq_getstat(struct qdma_wq *queue, struct qdma_wq_stat *stat)
{
	struct xlnx_dma_dev     *xdev;
	struct qdma_descq       *descq;

        xdev = (struct xlnx_dma_dev *)queue->dev_hdl;
        descq = qdma_device_get_descq_by_id(xdev, queue->qhdl, NULL, 0, 0);


	stat->total_req_bytes = queue->req_nbytes;
	stat->total_req_num = queue->req_num;
	stat->total_complete_bytes = queue->compl_nbytes;
	stat->total_complete_num = queue->compl_num;
	stat->hw_submit_bytes = queue->proc_nbytes;
	stat->hw_complete_bytes = queue->wb_nbytes;

	stat->total_slots = queue->wq_len;
	stat->free_slots = (queue->wq_pending - queue->wq_free-1) &
		(queue->wq_len - 1);
	stat->pending_slots = (queue->wq_unproc - queue->wq_pending) &
		(queue->wq_len - 1);
	stat->unproc_slots = (queue->wq_free - queue->wq_unproc) &
		(queue->wq_len - 1);

	stat->descq_rngsz = descq->conf.rngsz;
	stat->descq_pidx = descq->pidx;
	stat->descq_cidx = descq->cidx;
	stat->descq_avail = descq->avail;
	if (descq->desc_wb) {
		stat->desc_wb_cidx = ((struct qdma_desc_wb *)descq->desc_wb)->cidx;
		stat->desc_wb_pidx = ((struct qdma_desc_wb *)descq->desc_wb)->pidx;
	}

	stat->descq_rngsz_wrb = descq->conf.rngsz_wrb;
	stat->descq_cidx_wrb = descq->cidx_wrb;
	stat->descq_pidx_wrb = descq->pidx_wrb;
	stat->descq_cidx_wrb_pend = descq->cidx_wrb_pend;
	if (descq->desc_wrb_wb) {
		stat->c2h_wrb_cidx = ((struct qdma_c2h_wrb_wb *)descq->desc_wrb_wb)->cidx;
		stat->c2h_wrb_pidx = ((struct qdma_c2h_wrb_wb *)descq->desc_wrb_wb)->pidx;
	}

	stat->flq_cidx = descq->flq.cidx;
	stat->flq_pidx = descq->flq.pidx;
	stat->flq_pidx_pend = descq->flq.pidx_pend;
}

int qdma_wq_update_pidx(struct qdma_wq *queue, u32 pidx)
{
	struct xlnx_dma_dev	*xdev;
	struct qdma_descq	*descq;

	xdev = (struct xlnx_dma_dev *)queue->dev_hdl;
	descq = qdma_device_get_descq_by_id(xdev, queue->qhdl, NULL, 0, 0);
	if (descq->conf.c2h)
		descq_c2h_pidx_update(descq, pidx);
	else
		descq_h2c_pidx_update(descq, pidx);

	return 0;
}

void qdma_arm_err_intr(unsigned long dev_hdl)
{
	qdma_err_intr_setup((struct xlnx_dma_dev *)dev_hdl, 1);
}
