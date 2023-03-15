/*
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
#include <linux/delay.h>
#include<linux/cpumask.h>
#endif

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/sched/rt.h>
#include <uapi/linux/sched/types.h>

#include "queue.h"
#include "block.h"
#include "core.h"
#include "crypto.h"
#include "card.h"
#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
//#define CONFIG_EMMC_SOFTWARE_CQ_BIND_CPUS
#endif

/*
 * Prepare a MMC request. This just filters out odd stuff.
 */
static int mmc_prep_request(struct request_queue *q, struct request *req)
{
	struct mmc_queue *mq = q->queuedata;

	if (mq && mmc_card_removed(mq->card))
		return BLKPREP_KILL;

	req->rq_flags |= RQF_DONTPREP;

	return BLKPREP_OK;
}

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
static void mmc_queue_softirq_done(struct request *req)
{
	blk_end_request_all(req, 0);
}

static int mmc_cmd_cmdq_full(struct mmc_queue *mq, struct request *req)
{
	struct mmc_host *host;
	int cnt, class;
	u8 cmdq_depth;

	host = mq->card->host;
	class = IS_RT_CLASS_REQ(req);

	cnt = atomic_read(&host->areq_cnt);
	cmdq_depth = host->card->ext_csd.cmdq_depth;
	if (!class &&
		cmdq_depth > EMMC_MIN_RT_CLASS_TAG_COUNT)
		cmdq_depth -= EMMC_MIN_RT_CLASS_TAG_COUNT;

	if (cnt >= cmdq_depth)
		return 1;

	return 0;
}
#endif
static int mmc_queue_thread(void *d)
{
	struct mmc_queue *mq = d;
	struct request_queue *q = mq->queue;
	struct mmc_context_info *cntx = &mq->card->host->context_info;
	struct sched_param scheduler_params = {0};

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	int cmdq_full = 0;
	unsigned int timeout;
#endif
	bool part_cmdq_en = false;

	scheduler_params.sched_priority = 1;

	sched_setscheduler(current, SCHED_FIFO, &scheduler_params);

	current->flags |= PF_MEMALLOC;

	down(&mq->thread_sem);

	do {
		struct request *req;

		spin_lock_irq(q->queue_lock);
		set_current_state(TASK_INTERRUPTIBLE);

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
		req = blk_peek_request(q);
		if (!req)
			goto fetch_done;

		part_cmdq_en = mmc_blk_part_cmdq_en(mq);
		if (part_cmdq_en && mmc_cmd_cmdq_full(mq, req)) {
			req = NULL;
			cmdq_full = 1;
			goto fetch_done;
		}
#endif

		req = blk_fetch_request(q);

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
fetch_done:
#endif
		mq->asleep = false;
		cntx->is_waiting_last_req = false;
		cntx->is_new_req = false;
		if (!req) {
			/*
			 * Dispatch queue is empty so set flags for
			 * mmc_request_fn() to wake us up.
			 */
			if (atomic_read(&mq->qcnt))
				cntx->is_waiting_last_req = true;
			else
				mq->asleep = true;
		}
		spin_unlock_irq(q->queue_lock);

		if (req || (!part_cmdq_en && atomic_read(&mq->qcnt))) {
			set_current_state(TASK_RUNNING);
			mmc_blk_issue_rq(mq, req);
			cond_resched();
		} else {
			if (kthread_should_stop()) {
				set_current_state(TASK_RUNNING);
				break;
			}
#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
			if (!cmdq_full) {
				/* no request */
				up(&mq->thread_sem);
				schedule();
				down(&mq->thread_sem);
			} else {
				/* queue full */
				cmdq_full = 0;
				/* wait when queue full */
				timeout = schedule_timeout(HZ);
				if (!timeout)
					pr_info("%s:sched_timeout,areq_cnt=%d\n",
						__func__,
					atomic_read(&mq->card->host->areq_cnt));
			}

#else
			up(&mq->thread_sem);
			schedule();
			down(&mq->thread_sem);
#endif

		}
	} while (1);
	up(&mq->thread_sem);

	return 0;
}

/*
 * Generic MMC request handler.  This is called for any queue on a
 * particular host.  When the host is not busy, we look for a request
 * on any queue on this host, and attempt to issue it.  This may
 * not be the queue we were asked to process.
 */
static void mmc_request_fn(struct request_queue *q)
{
	struct mmc_queue *mq = q->queuedata;
	struct request *req;
	struct mmc_context_info *cntx;

	if (!mq) {
		while ((req = blk_fetch_request(q)) != NULL) {
			req->rq_flags |= RQF_QUIET;
			__blk_end_request_all(req, BLK_STS_IOERR);
		}
		return;
	}

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	/* just wake up thread for cmdq */
	if (mmc_blk_part_cmdq_en(mq)) {
		wake_up_process(mq->thread);
		return;
	}
#endif
	cntx = &mq->card->host->context_info;

	if (cntx->is_waiting_last_req) {
		cntx->is_new_req = true;
		wake_up_interruptible(&cntx->wait);
	}

	if (mq->asleep)
		wake_up_process(mq->thread);
}

static struct scatterlist *mmc_alloc_sg(int sg_len, gfp_t gfp)
{
	struct scatterlist *sg;

	sg = kmalloc_array(sg_len, sizeof(*sg), gfp);
	if (sg)
		sg_init_table(sg, sg_len);

	return sg;
}

static void mmc_queue_setup_discard(struct request_queue *q,
				    struct mmc_card *card)
{
	unsigned max_discard;

	max_discard = mmc_calc_max_discard(card);
	if (!max_discard)
		return;

	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
	blk_queue_max_discard_sectors(q, max_discard);
	q->limits.discard_granularity = card->pref_erase << 9;
	/* granularity must not be greater than max. discard */
	if (card->pref_erase > max_discard)
		q->limits.discard_granularity = 0;
	if (mmc_can_secure_erase_trim(card))
		queue_flag_set_unlocked(QUEUE_FLAG_SECERASE, q);
}

/**
 * mmc_init_request() - initialize the MMC-specific per-request data
 * @q: the request queue
 * @req: the request
 * @gfp: memory allocation policy
 */
static int mmc_init_request(struct request_queue *q, struct request *req,
			    gfp_t gfp)
{
	struct mmc_queue_req *mq_rq = req_to_mmc_queue_req(req);
	struct mmc_queue *mq;
	struct mmc_card *card;
	struct mmc_host *host;

	/* add "if" to fix the error condition:
	 * STEP 1:remove sdcard call mmc_cleanup_queue,
	 * get queue_lock, then queuedata = NULL, put queue_lock;
	 * STEP 2:generic_make _request call blk_queue_bio,
	 * get queue_lock, then call get_request, mempool_alloc,
	 * alloc_request_size, mmc_init_request, queuedata is NULL
	 * in this time.
	 * STEP 3: null pointer exception.
	 */
	if (q->queuedata)
		mq = q->queuedata;
	else
		return -ENODEV;

	card = mq->card;
	host = card->host;
#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	/* cmdq use preallocate sg buffer */
	if (mmc_blk_part_cmdq_en(mq))
		return 0;
#endif
	mq_rq->sg = mmc_alloc_sg(host->max_segs, gfp);
	if (!mq_rq->sg)
		return -ENOMEM;

	return 0;
}

static void mmc_exit_request(struct request_queue *q, struct request *req)
{
	struct mmc_queue_req *mq_rq = req_to_mmc_queue_req(req);
#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	/* cmdq use preallocate sg buffer */
	if (q->queuedata &&
		mmc_blk_part_cmdq_en(q->queuedata))
		return;
#endif

	kfree(mq_rq->sg);
	mq_rq->sg = NULL;
}

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
void mmc_cmdq_enable_work(struct work_struct *work)
{
	struct mmc_host *host =
		container_of(work, struct mmc_host, cmdq_enable_work.work);
	struct mmc_card *card = host->card;
	/**
	 * make sure no owner claimed the host. if not, the owner may
	 * claim the host many times. It should be safe when claim_cnt
	 * is zero before cmdq switching.
	 */
	while (host->claim_cnt)
		usleep_range(1000, 2000);
	mmc_get_card(card);
	card->ext_csd.cmdq_support = true;
	mmc_select_cmdq(card);
	host->cmdq_enable_delay = false;
	mmc_put_card(card);
	pr_info("%s: cmdq delay enabled success\n",
					mmc_hostname(host));
}
#endif
/**
 * mmc_init_queue - initialise a queue structure.
 * @mq: mmc queue
 * @card: mmc card to attach this queue
 * @lock: queue lock
 * @subname: partition subname
 *
 * Initialise a MMC card request queue.
 */
int mmc_init_queue(struct mmc_queue *mq, struct mmc_card *card,
		   spinlock_t *lock, const char *subname, int area_type)
{
	struct mmc_host *host = card->host;
#if defined(CONFIG_EMMC_SOFTWARE_CQ_BIND_CPUS)
	cpumask_t cpumasks;
	int cpu_num;
#endif
	u64 limit = BLK_BOUNCE_HIGH;
	int ret = -ENOMEM;
#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	int i;
#endif

	if (mmc_dev(host)->dma_mask && *mmc_dev(host)->dma_mask)
		limit = (u64)dma_max_pfn(mmc_dev(host)) << PAGE_SHIFT;

	mq->card = card;
#if defined(CONFIG_EMMC_SOFTWARE_CQ_SUPPORT)
	if (card->ext_csd.cmdq_support &&
		(area_type == MMC_BLK_DATA_AREA_MAIN)) {
#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
		if (!(host->caps2 & MMC_CAP2_CQE)) {
			pr_notice("%s: init cq\n", mmc_hostname(host));
			atomic_set(&host->cq_rw, false);
			atomic_set(&host->cq_w, false);
			atomic_set(&host->cq_wait_rdy, 0);
			host->wp_error = 0;
			host->task_id_index = 0;
			atomic_set(&host->is_data_dma, 0);
			host->cur_rw_task = CQ_TASK_IDLE;
			atomic_set(&host->cq_tuning_now, 0);

			for (i = 0; i < EMMC_MAX_QUEUE_DEPTH; i++) {
				host->data_mrq_queued[i] = false;
				atomic_set(&mq->mqrq[i].index, 0);
			}
			/*enable cmdq after 30s*/
#ifdef CONFIG_EMMC_CMDQ_ENABLE_DELAY_SUPPORT
			mmc_get_card(card);
			card->ext_csd.cmdq_support = false;
			mmc_deselect_cmdq(card);
			mmc_put_card(card);
			INIT_DELAYED_WORK(&host->cmdq_enable_work,
						mmc_cmdq_enable_work);
			queue_delayed_work(system_wq,
						&host->cmdq_enable_work, 30*HZ);
			host->cmdq_enable_delay = true;
#endif
			host->cmdq_thread = kthread_run(mmc_cmd_queue_thread,
				host,
				"mmc_cq/%d", host->index);
			if (IS_ERR(host->cmdq_thread)) {
				pr_notice("%s: %d: cmdq: failed to start mmc_cq thread\n",
					mmc_hostname(host), ret);
			}
#if defined(CONFIG_EMMC_SOFTWARE_CQ_BIND_CPUS)
			/* bind this thread to cpus except cpu0/1
			 * because cpu 0/1 will handle interrupt
			 */
			cpu_num = nr_cpu_ids;
			for (i = 0; i < cpu_num; i++)
				cpumask_clear_cpu(i, &cpumasks);
			for (i = 2; i < cpu_num; i++)
				cpumask_set_cpu(i, &cpumasks);
			set_cpus_allowed_ptr(host->cmdq_thread, &cpumasks);
#endif
		}
#endif
	}
#endif
	mq->queue = blk_alloc_queue(GFP_KERNEL);
	if (!mq->queue)
		return -ENOMEM;
	mq->queue->queue_lock = lock;
	mq->queue->request_fn = mmc_request_fn;
	mq->queue->init_rq_fn = mmc_init_request;
	mq->queue->exit_rq_fn = mmc_exit_request;
	mq->queue->cmd_size = sizeof(struct mmc_queue_req);
	mq->queue->queuedata = mq;
	atomic_set(&mq->qcnt, 0);
	mq->queue->backing_dev_info->ra_pages = 128;
	ret = blk_init_allocated_queue(mq->queue);
	if (ret) {
		blk_cleanup_queue(mq->queue);
		return ret;
	}

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	if (mmc_card_mmc(card)) {
		for (i = 0; i < card->ext_csd.cmdq_depth; i++)
			atomic_set(&mq->mqrq[i].index, 0);
	}
#endif
	blk_queue_prep_rq(mq->queue, mmc_prep_request);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, mq->queue);
	queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, mq->queue);

	if (mmc_can_erase(card))
		mmc_queue_setup_discard(mq->queue, card);

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	blk_queue_softirq_done(mq->queue, mmc_queue_softirq_done);
#endif
	blk_queue_bounce_limit(mq->queue, limit);
	blk_queue_max_hw_sectors(mq->queue,
		min(host->max_blk_count, host->max_req_size / 512));
	blk_queue_max_segments(mq->queue, host->max_segs);
	blk_queue_max_segment_size(mq->queue, host->max_seg_size);

#ifdef CONFIG_EMMC_SOFTWARE_CQ_SUPPORT
	if (mmc_card_mmc(card)) {
		for (i = 0; i < card->ext_csd.cmdq_depth; i++) {
			mq->mqrq[i].sg = mmc_alloc_sg(host->max_segs,
				GFP_KERNEL);
			if (!mq->mqrq[i].sg)
				goto cleanup_queue;
		}
	}
#endif
	sema_init(&mq->thread_sem, 1);

	mq->thread = kthread_run(mmc_queue_thread, mq, "mmcqd/%d%s",
		host->index, subname ? subname : "");

	if (mmc_card_sd(card)) {
		/* decrease max # of requests to 32. The goal of this tuning is
		 * reducing the time for draining elevator when elevator_switch
		 * function is called. It is effective for slow external sdcard.
		 */
		mq->queue->nr_requests = BLKDEV_MAX_RQ / 8;
		if (mq->queue->nr_requests < 32)
			mq->queue->nr_requests = 32;

#ifdef CONFIG_LARGE_DIRTY_BUFFER
		/* apply more throttle on external sdcard */
		mq->queue->backing_dev_info->capabilities |= BDI_CAP_STRICTLIMIT;
		bdi_set_min_ratio(mq->queue->backing_dev_info, 30);
		bdi_set_max_ratio(mq->queue->backing_dev_info, 60);
#endif

		pr_info("Parameters for external-sdcard: min/max_ratio: %u/%u "
			"strictlimit: on nr_requests: %lu read_ahead_kb: %lu\n",
			mq->queue->backing_dev_info->min_ratio,
			mq->queue->backing_dev_info->max_ratio,
			mq->queue->nr_requests,
			mq->queue->backing_dev_info->ra_pages * 4);
	}

	if (IS_ERR(mq->thread)) {
		ret = PTR_ERR(mq->thread);
		goto cleanup_queue;
	}

	mmc_crypto_setup_queue(host, mq->queue);
	return 0;

cleanup_queue:
	blk_cleanup_queue(mq->queue);
#ifdef CONFIG_EMMC_CMDQ_ENABLE_DELAY_SUPPORT
	if (host->cmdq_enable_delay)
		cancel_delayed_work(&host->cmdq_enable_work);
#endif
	return ret;
}

void mmc_cleanup_queue(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	/* Make sure the queue isn't suspended, as that will deadlock */
	mmc_queue_resume(mq);

	/* Then terminate our worker thread */
	kthread_stop(mq->thread);

#ifdef CONFIG_LARGE_DIRTY_BUFFER
	/* Restore bdi min/max ratio before device removal */
	bdi_set_min_ratio(q->backing_dev_info, 0);
	bdi_set_max_ratio(q->backing_dev_info, 100);
#endif

	/* Empty the queue */
	spin_lock_irqsave(q->queue_lock, flags);
	q->queuedata = NULL;
	blk_start_queue(q);
	spin_unlock_irqrestore(q->queue_lock, flags);

	if (likely(!blk_queue_dead(q)))
		blk_cleanup_queue(q);
	mq->card = NULL;
}
EXPORT_SYMBOL(mmc_cleanup_queue);

/**
 * mmc_queue_suspend - suspend a MMC request queue
 * @mq: MMC queue to suspend
 *
 * Stop the block request queue, and wait for our thread to
 * complete any outstanding requests.  This ensures that we
 * won't suspend while a request is being processed.
 */
void mmc_queue_suspend(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	if (!mq->suspended) {
		mq->suspended |= true;

		spin_lock_irqsave(q->queue_lock, flags);
		blk_stop_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);

		down(&mq->thread_sem);
	}
}

/**
 * mmc_queue_resume - resume a previously suspended MMC request queue
 * @mq: MMC queue to resume
 */
void mmc_queue_resume(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	if (mq->suspended) {
		mq->suspended = false;

		up(&mq->thread_sem);

		spin_lock_irqsave(q->queue_lock, flags);
		blk_start_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
}

/*
 * Prepare the sg list(s) to be handed of to the host driver
 */
unsigned int mmc_queue_map_sg(struct mmc_queue *mq, struct mmc_queue_req *mqrq)
{
	struct request *req = mmc_queue_req_to_req(mqrq);

	return blk_rq_map_sg(mq->queue, req, mqrq->sg);
}
