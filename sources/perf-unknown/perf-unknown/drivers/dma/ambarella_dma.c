/*
 * drivers/dma/ambarella_dma.c  --  Ambarella DMA engine driver
 *
 * History:
 *	2012/05/10 - Ken He <jianhe@ambarella.com> created file
 *
 * Coryright (c) 2008-2012, Ambarella, Inc.
 * http://www.ambarella.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <plat/dma.h>
#include <plat/rct.h>
#include "dmaengine.h"
#include "ambarella_dma.h"

/* The set of bus widths supported by the DMA controller */
#define AMBARELLA_DMA_BUSWIDTHS					\
		BIT(DMA_SLAVE_BUSWIDTH_UNDEFINED)		| \
		BIT(DMA_SLAVE_BUSWIDTH_1_BYTE)			| \
		BIT(DMA_SLAVE_BUSWIDTH_2_BYTES)			| \
		BIT(DMA_SLAVE_BUSWIDTH_4_BYTES)			| \
		BIT(DMA_SLAVE_BUSWIDTH_8_BYTES)

static int ambdma_proc_show(struct seq_file *m, void *v)
{
	struct ambdma_device *amb_dma = m->private;
	struct ambdma_chan *amb_chan;
	const char *sw_status;
	const char *hw_status;
	int i;

	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];

		switch(amb_chan->status) {
		case AMBDMA_STATUS_IDLE:
			sw_status = "idle";
			break;
		case AMBDMA_STATUS_BUSY:
			sw_status = "busy";
			break;
		case AMBDMA_STATUS_STOPPING:
			sw_status = "stopping";
			break;
		default:
			sw_status = "unknown";
			break;
		}

		if (ambdma_chan_is_enabled(amb_chan))
			hw_status = "running";
		else
			hw_status = "stopped";

		seq_printf(m, "channel %d:   %s, %d, %s, %s\n",
			amb_chan->chan.chan_id, dma_chan_name(&amb_chan->chan),
			amb_chan->chan.client_count, sw_status, hw_status);
	}

	seq_printf(m, "\nInput channel ID to stop specific dma channel:\n");
	seq_printf(m, "    example: echo 3 > dma\n\n");

	return 0;
}

static ssize_t ambdma_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	struct ambdma_device *amb_dma = PDE_DATA(file_inode(file));
	int id, ret;

	ret = kstrtouint_from_user(buffer, count, 0, &id);
	if (ret)
		return ret;

	if (id >= amb_dma->nr_channels) {
		printk("Invalid channel id\n");
		return -EINVAL;
	}

	dmaengine_terminate_all(&amb_dma->amb_chan[id].chan);

	return count;
}

static int ambdma_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambdma_proc_show, PDE_DATA(inode));
}

static const struct file_operations proc_ambdma_fops = {
	.open = ambdma_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = ambdma_proc_write,
};

static struct ambdma_desc *ambdma_alloc_desc(struct dma_chan *chan, gfp_t gfp_flags)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	struct ambdma_desc *amb_desc = NULL;
	dma_addr_t phys;

	amb_desc = kzalloc(sizeof(struct ambdma_desc), gfp_flags);
	if (!amb_desc)
		return NULL;

	amb_desc->lli = dma_pool_alloc(amb_dma->lli_pool, gfp_flags, &phys);
	if (!amb_desc->lli) {
		kfree(amb_desc);
		return NULL;
	}

	INIT_LIST_HEAD(&amb_desc->tx_list);
	dma_async_tx_descriptor_init(&amb_desc->txd, chan);
	/* txd.flags will be overwritten in prep functions */
	amb_desc->txd.flags = DMA_CTRL_ACK;
	amb_desc->txd.tx_submit = ambdma_tx_submit;
	amb_desc->txd.phys = phys;
	amb_desc->is_cyclic = 0;

	return amb_desc;
}

static void ambdma_free_desc(struct dma_chan *chan, struct ambdma_desc *amb_desc)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_device *amb_dma = amb_chan->amb_dma;

	dma_pool_free(amb_dma->lli_pool, amb_desc->lli, amb_desc->txd.phys);
	kfree(amb_desc);
}

static struct ambdma_desc *ambdma_get_desc(struct ambdma_chan *amb_chan)
{
	struct ambdma_desc *desc, *_desc;
	struct ambdma_desc *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);
	list_for_each_entry_safe(desc, _desc, &amb_chan->free_list, desc_node) {
		if (async_tx_test_ack(&desc->txd)) {
			list_del_init(&desc->desc_node);
			ret = desc;
			break;
		}
	}
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	/* no more descriptor available in initial pool: create one more */
	if (!ret) {
		ret = ambdma_alloc_desc(&amb_chan->chan, GFP_ATOMIC);
		if (ret) {
			spin_lock_irqsave(&amb_chan->lock, flags);
			amb_chan->descs_allocated++;
			spin_unlock_irqrestore(&amb_chan->lock, flags);
		} else {
			pr_err("%s: no more descriptors available\n", __func__);
		}
	}

	return ret;
}


static void ambdma_put_desc(struct ambdma_chan *amb_chan,
		struct ambdma_desc *amb_desc)
{
	unsigned long flags;

	if (amb_desc) {
		spin_lock_irqsave(&amb_chan->lock, flags);
		list_splice_init(&amb_desc->tx_list, &amb_chan->free_list);
		list_add_tail(&amb_desc->desc_node, &amb_chan->free_list);
		spin_unlock_irqrestore(&amb_chan->lock, flags);
	}
}

static void ambdma_return_desc(struct ambdma_chan *amb_chan,
		struct ambdma_desc *amb_desc)
{
	/* move children to free_list */
	list_splice_init(&amb_desc->tx_list, &amb_chan->free_list);
	/* move myself to free_list */
	list_move_tail(&amb_desc->desc_node, &amb_chan->free_list);
}

static void ambdma_chain_complete(struct ambdma_chan *amb_chan,
		struct ambdma_desc *amb_desc)
{
	struct dma_async_tx_descriptor	*txd = &amb_desc->txd;

	dma_cookie_complete(txd);

	ambdma_return_desc(amb_chan, amb_desc);

	spin_unlock(&amb_chan->lock);
	if (txd->callback && (txd->flags & DMA_PREP_INTERRUPT))
		txd->callback(txd->callback_param);
	spin_lock(&amb_chan->lock);

	dma_run_dependencies(txd);
}


static void ambdma_complete_all(struct ambdma_chan *amb_chan)
{
	struct ambdma_desc *amb_desc, *_desc;
	LIST_HEAD(list);

	list_splice_init(&amb_chan->active_list, &list);

	/* submit queued descriptors ASAP, i.e. before we go through
	 * the completed ones. */
	if (!list_empty(&amb_chan->queue)) {
		list_splice_init(&amb_chan->queue, &amb_chan->active_list);
		ambdma_dostart(amb_chan, ambdma_first_active(amb_chan));
	}

	list_for_each_entry_safe(amb_desc, _desc, &list, desc_node)
		ambdma_chain_complete(amb_chan, amb_desc);
}

static void ambdma_advance_work(struct ambdma_chan *amb_chan)
{
	if (list_empty(&amb_chan->active_list) || list_is_singular(&amb_chan->active_list)) {
		ambdma_complete_all(amb_chan);
	} else {
		ambdma_chain_complete(amb_chan, ambdma_first_active(amb_chan));
		/* active_list has been updated by ambdma_chain_complete(),
		 * so ambdma_first_active() will get another amb_desc. */
		ambdma_dostart(amb_chan, ambdma_first_active(amb_chan));
	}
}

static void ambdma_handle_error(struct ambdma_chan *amb_chan,
		struct ambdma_desc *bad_desc)
{
	list_del_init(&bad_desc->desc_node);

	/* try to submit queued descriptors to restart dma */
	list_splice_init(&amb_chan->queue, amb_chan->active_list.prev);
	if (!list_empty(&amb_chan->active_list))
		ambdma_dostart(amb_chan, ambdma_first_active(amb_chan));

	pr_crit("%s: DMA error on channel %d: 0x%08x\n",
		__func__, amb_chan->chan.chan_id, bad_desc->lli->rpt);

	/* pretend the descriptor completed successfully */
	ambdma_chain_complete(amb_chan, bad_desc);
}

static void ambdma_tasklet(unsigned long data)
{
	struct ambdma_chan *amb_chan = (struct ambdma_chan *)data;
	struct ambdma_desc *amb_desc = NULL;
	enum ambdma_status old_status;
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);

	old_status = amb_chan->status;
	if (!ambdma_chan_is_enabled(amb_chan)) {
		amb_chan->status = AMBDMA_STATUS_IDLE;
		if (!list_empty(&amb_chan->stopping_list))
			ambdma_return_desc(amb_chan, ambdma_first_stopping(amb_chan));
	}

	/* someone might have called terminate all */
	if (list_empty(&amb_chan->active_list))
		goto tasklet_out;

	/* note: if the DMA channel is stopped manually rather than naturally end,
	 * then ambdma_first_active() will return the next descriptor that need
	 * to be started, but not the descriptor that invoke this tasklet (IRQ) */
	amb_desc = ambdma_first_active(amb_chan);

	if (!amb_desc->is_cyclic && amb_chan->status != AMBDMA_STATUS_IDLE) {
		pr_err("%s: channel(%d) invalid status\n",
				__func__, amb_chan->chan.chan_id);
		goto tasklet_out;
	}

	if (old_status == AMBDMA_STATUS_BUSY) {
		/* the IRQ is triggered by DMA stopping naturally or by errors.*/
		if (ambdma_desc_is_error(amb_desc)) {
			ambdma_handle_error(amb_chan, amb_desc);
		} else if (amb_desc->is_cyclic) {
			spin_unlock(&amb_chan->lock);
			if (amb_desc->txd.callback)
				amb_desc->txd.callback(amb_desc->txd.callback_param);
			spin_lock(&amb_chan->lock);
		} else {
			ambdma_advance_work(amb_chan);
		}
	} else if (old_status == AMBDMA_STATUS_STOPPING) {
		/* the DMA channel is stopped manually.  */
		ambdma_dostart(amb_chan, amb_desc);
	}

tasklet_out:
	spin_unlock_irqrestore(&amb_chan->lock, flags);
}

static irqreturn_t ambdma_dma_irq_handler(int irq, void *dev_data)
{
	struct ambdma_device *amb_dma = dev_data;
	struct ambdma_chan *amb_chan;
	u32 i, int_src;
	irqreturn_t ret = IRQ_NONE;
#if defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
	int chan_sel, cur_chan;
#endif

	int_src = readl(amb_dma->regbase + DMA_INT_OFFSET);

	if (int_src == 0)
		return IRQ_HANDLED;

#if defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
	if (amb_dma->reg_scr)
		regmap_read(amb_dma->reg_scr, AHBSP_DMA_CHANNEL_SEL_OFFSET, &chan_sel);
	else
		chan_sel = 0;
	for (i = amb_dma->nr_start; i < amb_dma->nr_channels; i++) {
		/*
		 * Only handle DMA that would be used by Linux,
		 * e.g. UART used by BT.
		 */
		cur_chan = (chan_sel >> (i * 4)) & 0xf;
		switch (cur_chan) {
		case UART1_TX_DMA_REQ_IDX:
		case UART1_RX_DMA_REQ_IDX:
		case UART2_TX_DMA_REQ_IDX:
		case UART2_RX_DMA_REQ_IDX:
			break;
		default:
			continue;
		}

		amb_chan = &amb_dma->amb_chan[i];
		spin_lock(&amb_chan->lock);
		if (int_src & (1 << i)) {
			writel(0, dma_chan_sta_reg(amb_chan));
			tasklet_schedule(&amb_chan->tasklet);
			ret = IRQ_HANDLED;
		}
		spin_unlock(&amb_chan->lock);
	}
#else
	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];
		spin_lock(&amb_chan->lock);
		if (int_src & (1 << i)) {
			writel(0, dma_chan_sta_reg(amb_chan));
			tasklet_schedule(&amb_chan->tasklet);
			ret = IRQ_HANDLED;
		}
		spin_unlock(&amb_chan->lock);
	}
#endif

	return ret;
}

static int ambdma_stop_channel(struct ambdma_chan *amb_chan)
{
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	struct ambdma_desc *first, *amb_desc;

	if (amb_chan->status == AMBDMA_STATUS_BUSY) {
		if (amb_chan->force_stop == 0 || amb_dma->support_prs) {
			/* if force_stop == 0, the DMA channel is still running
			 * at this moment. And if the chip doesn't support early
			 * end, normally there are still two IRQs will be triggered
			 * untill DMA channel stops. */
			first = ambdma_first_active(amb_chan);
			first->lli->attr |= DMA_DESC_EOC;
			list_for_each_entry(amb_desc, &first->tx_list, desc_node) {
				amb_desc->lli->attr |= DMA_DESC_EOC;
			}
			amb_chan->status = AMBDMA_STATUS_STOPPING;
			/* active_list is still being used by DMA controller,
			 * so move it to stopping_list to avoid being
			 * initialized by next transfer */
			list_move_tail(&first->desc_node, &amb_chan->stopping_list);

			if (amb_dma->support_prs) {
				u32 val = readl(amb_dma->regbase + DMA_EARLY_END_OFFSET);
				val |= 0x1 << amb_chan->chan.chan_id;
				writel(val, amb_dma->regbase + DMA_EARLY_END_OFFSET);
			}
		} else {
			/* Disable DMA: this sequence is not mentioned at PRM.*/
			writel(DMA_CHANX_STA_OD, dma_chan_sta_reg(amb_chan));
			writel(amb_dma->dummy_lli_phys, dma_chan_da_reg(amb_chan));
			writel(DMA_CHANX_CTR_WM | DMA_CHANX_CTR_NI,
						dma_chan_ctr_reg(amb_chan));
			udelay(1);
			/* avoid to trigger dummy IRQ.*/
			writel(0x0, dma_chan_sta_reg(amb_chan));
			if (ambdma_chan_is_enabled(amb_chan)) {
				pr_err("%s: stop dma channel(%d) failed\n",
					__func__, amb_chan->chan.chan_id);
				return -EIO;
			}
			amb_chan->status = AMBDMA_STATUS_IDLE;
		}
	}

	return 0;
}

static int ambdma_pause_channel(struct ambdma_chan *amb_chan)
{
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	u32 val;

	if (!amb_dma->support_prs)
		return -ENXIO;

	val = readl(amb_dma->regbase + DMA_PAUSE_SET_OFFSET);
	val |= 0x1 << amb_chan->chan.chan_id;
	writel(val, amb_dma->regbase + DMA_PAUSE_SET_OFFSET);

	return 0;
}

static int ambdma_resume_channel(struct ambdma_chan *amb_chan)
{
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	u32 val;

	if (!amb_dma->support_prs)
		return -ENXIO;

	val = readl(amb_dma->regbase + DMA_PAUSE_CLR_OFFSET);
	val |= 1 << amb_chan->chan.chan_id;
	writel(val, amb_dma->regbase + DMA_PAUSE_CLR_OFFSET);

	return 0;
}

static void ambdma_dostart(struct ambdma_chan *amb_chan, struct ambdma_desc *first)
{
	/* if DMA channel is not idle right now, the DMA descriptor
	 * will be started at ambdma_tasklet(). */
	if (amb_chan->status > AMBDMA_STATUS_IDLE)
		return;

	if (ambdma_chan_is_enabled(amb_chan)) {
		pr_err("%s: channel(%d) should be idle\n",
				__func__, amb_chan->chan.chan_id);
		return;
	}

	writel(0x0, dma_chan_sta_reg(amb_chan));
	writel(first->txd.phys, dma_chan_da_reg(amb_chan));
	writel(DMA_CHANX_CTR_EN | DMA_CHANX_CTR_D, dma_chan_ctr_reg(amb_chan));
	amb_chan->status = AMBDMA_STATUS_BUSY;
}

static dma_cookie_t ambdma_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct ambdma_desc *amb_desc;
	struct ambdma_chan *amb_chan;
	dma_cookie_t cookie;
	unsigned long flags;

	amb_desc = to_ambdma_desc(tx);
	amb_chan = to_ambdma_chan(tx->chan);

	spin_lock_irqsave(&amb_chan->lock, flags);

	cookie = dma_cookie_assign(tx);

	if (list_empty(&amb_chan->active_list)) {
		list_add_tail(&amb_desc->desc_node, &amb_chan->active_list);
		ambdma_dostart(amb_chan, amb_desc);
	} else {
		list_add_tail(&amb_desc->desc_node, &amb_chan->queue);
	}

	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return cookie;
}

/* DMA Engine API begin */
static int ambdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan;
	struct ambdma_desc *amb_desc;
	LIST_HEAD(tmp_list);
	unsigned long flags;
	int i;

	amb_chan = to_ambdma_chan(chan);

	if (amb_chan->status == AMBDMA_STATUS_BUSY) {
		pr_err("%s: channel(%d) not idle!\n", __func__, amb_chan->chan.chan_id);
		return -EIO;
	}

	/* Alloc descriptors for this channel */
	for (i = 0; i < NR_DESCS_PER_CHANNEL; i++) {
		amb_desc = ambdma_alloc_desc(chan, GFP_KERNEL);
		if (amb_desc == NULL) {
			struct ambdma_desc *d, *_d;
			list_for_each_entry_safe(d, _d, &tmp_list, desc_node) {
				ambdma_free_desc(chan, d);
			}
			pr_err("%s: failed to allocate descriptor\n", __func__);
			return -ENOMEM;
		}

		list_add_tail(&amb_desc->desc_node, &tmp_list);
	}

	spin_lock_irqsave(&amb_chan->lock, flags);
	amb_chan->descs_allocated = i;
	list_splice_init(&tmp_list, &amb_chan->free_list);
	dma_cookie_init(chan);
	if (!amb_chan->chan.private)
		amb_chan->force_stop = 1;
	else
		amb_chan->force_stop = *(u32 *)amb_chan->chan.private;
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return amb_chan->descs_allocated;
}

static void ambdma_free_chan_resources(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan;
	struct ambdma_desc *amb_desc, *_desc;
	LIST_HEAD(list);
	unsigned long flags;

	amb_chan = to_ambdma_chan(chan);

	spin_lock_irqsave(&amb_chan->lock, flags);
	BUG_ON(!list_empty(&amb_chan->active_list));
	BUG_ON(!list_empty(&amb_chan->queue));
	BUG_ON(amb_chan->status == AMBDMA_STATUS_BUSY);

	list_splice_init(&amb_chan->free_list, &list);
	amb_chan->descs_allocated = 0;
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	list_for_each_entry_safe(amb_desc, _desc, &list, desc_node)
		ambdma_free_desc(chan, amb_desc);
}

/* If this function is called when the dma channel is transferring data,
 * you may get inaccuracy result. */
static u32 ambdma_get_bytes_left(struct dma_chan *chan, dma_cookie_t cookie)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *first = NULL, *amb_desc;
	unsigned long flags;
	u32 count = 0;

	spin_lock_irqsave(&amb_chan->lock, flags);

	/* according to the cookie, find amb_desc in active_list. */
	if (!list_empty(&amb_chan->active_list)) {
		amb_desc = ambdma_first_active(amb_chan);
		if (amb_desc->txd.cookie == cookie)
			first = amb_desc;
	}

	/* if it's in active list, we should get the count for non-completed
	 * desc from the dma channel status register, and get the count for
	 * completed desc from the "rpt" field in desc. */
	if (first) {
		count = readl(dma_chan_sta_reg(amb_chan));
		count &= AMBARELLA_DMA_MAX_LENGTH;

		count += ambdma_desc_transfer_count(first);
		if (!list_empty(&first->tx_list)) {
			list_for_each_entry(amb_desc, &first->tx_list, desc_node) {
				count += ambdma_desc_transfer_count(amb_desc);
			}
		}
	} else if (!list_empty(&amb_chan->queue)) {
		/* if it's in queue list, all of the desc have not been started,
		 * so the transferred count is always 0.  */
		list_for_each_entry(amb_desc, &amb_chan->queue, desc_node) {
			if (amb_desc->txd.cookie == cookie) {
				first = amb_desc;
				count = 0;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&amb_chan->lock, flags);

	BUG_ON(!first);

	return first->len - count;
}

static enum dma_status ambdma_tx_status(struct dma_chan *chan,
		dma_cookie_t cookie, struct dma_tx_state *txstate)
{
	enum dma_status ret;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret != DMA_COMPLETE)
		dma_set_residue(txstate, ambdma_get_bytes_left(chan, cookie));

	return ret;
}

static void ambdma_issue_pending(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);

	/* if dma channel is not idle, will active queue list in tasklet. */
	if (amb_chan->status == AMBDMA_STATUS_IDLE) {
		if (!list_empty(&amb_chan->active_list))
			pr_err("%s: active_list should be empty here\n", __func__);
		else
			ambdma_advance_work(amb_chan);
	}

	spin_unlock_irqrestore(&amb_chan->lock, flags);
}


static int ambdma_pause(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&amb_chan->lock, flags);
	ret = ambdma_pause_channel(amb_chan);
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return ret;
}

static int ambdma_resume(struct dma_chan *chan)
{

	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&amb_chan->lock, flags);
	ret = ambdma_resume_channel(amb_chan);
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return ret;
}
static int ambdma_config(struct dma_chan *chan,
			 struct dma_slave_config *dmaengine_cfg)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct dma_slave_config *config = dmaengine_cfg;
	enum dma_slave_buswidth width = 0;
	int maxburst;

	/* We only support mem to dev or dev to mem transfers */
	switch (config->direction) {
	case DMA_MEM_TO_DEV:
		width = config->dst_addr_width;
		maxburst = config->dst_maxburst;
		amb_chan->rt_addr = config->dst_addr;
		amb_chan->rt_attr = DMA_DESC_RM | DMA_DESC_NI |
			DMA_DESC_IE | DMA_DESC_ST;
		break;
	case DMA_DEV_TO_MEM:
		width = config->src_addr_width;
		maxburst = config->src_maxburst;
		amb_chan->rt_addr = config->src_addr;
		amb_chan->rt_attr = DMA_DESC_WM | DMA_DESC_NI |
			DMA_DESC_IE | DMA_DESC_ST;
		break;
	default:
		return -ENXIO;
	}

	/* bus width for descriptor mode control_info [ts fileds] */
	switch (width) {
	case DMA_SLAVE_BUSWIDTH_8_BYTES:
		amb_chan->rt_attr |= DMA_DESC_TS_8B;
		break;
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		amb_chan->rt_attr |= DMA_DESC_TS_4B;
		break;
	case DMA_SLAVE_BUSWIDTH_2_BYTES:
		amb_chan->rt_attr |= DMA_DESC_TS_2B;
		break;
	case DMA_SLAVE_BUSWIDTH_1_BYTE:
		amb_chan->rt_attr |= DMA_DESC_TS_1B;
		break;
	default:
		break;
	}

	/* burst for descriptor mode control_info [blk fileds] */
	switch (maxburst) {
	case 1024:
		amb_chan->rt_attr |= DMA_DESC_BLK_1024B;
		break;
	case 512:
		amb_chan->rt_attr |= DMA_DESC_BLK_512B;
		break;
	case 256:
		amb_chan->rt_attr |= DMA_DESC_BLK_256B;
		break;
	case 128:
		amb_chan->rt_attr |= DMA_DESC_BLK_128B;
		break;
	case 64:
		amb_chan->rt_attr |= DMA_DESC_BLK_64B;
		break;
	case 32:
		amb_chan->rt_attr |= DMA_DESC_BLK_32B;
		break;
	case 16:
		amb_chan->rt_attr |= DMA_DESC_BLK_16B;
		break;
	case 8:
		amb_chan->rt_attr |= DMA_DESC_BLK_8B;
		break;
	default:
		break;
	}

	return 0;


}

static int ambdma_terminate_all(struct dma_chan *chan)
{

	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc, *_desc;
	LIST_HEAD(list);
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);
	ambdma_stop_channel(amb_chan);

	/* active_list entries will end up before queued entries */
	list_splice_init(&amb_chan->queue, &list);
	list_splice_init(&amb_chan->active_list, &list);

	/* Flush all pending and queued descriptors */
	list_for_each_entry_safe(amb_desc, _desc, &list, desc_node) {
		/* move children to free_list */
		list_splice_init(&amb_desc->tx_list, &amb_chan->free_list);
		/* move myself to free_list */
		list_move_tail(&amb_desc->desc_node, &amb_chan->free_list);
	}
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return 0;

}

static struct dma_async_tx_descriptor *ambdma_prep_dma_cyclic(
		struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
		size_t period_len, enum dma_transfer_direction direction,
		unsigned long flags)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc, *first = NULL, *prev = NULL;
	int left_len = buf_len;

	if (buf_len == 0 || period_len == 0) {
		pr_err("%s: buf/period length is zero!\n", __func__);
		return NULL;
	}

	do {
		amb_desc = ambdma_get_desc(amb_chan);
		if (!amb_desc)
			goto dma_cyclic_err;

		amb_desc->is_cyclic = 1;

		if (period_len > left_len)
			period_len = left_len;

		if (direction == DMA_MEM_TO_DEV) {
			amb_desc->lli->src = buf_addr;
			amb_desc->lli->dst = amb_chan->rt_addr;
		} else if (direction == DMA_DEV_TO_MEM) {
			amb_desc->lli->src = amb_chan->rt_addr;
			amb_desc->lli->dst = buf_addr;
		} else {
			goto dma_cyclic_err;
		}
		/* trigger interrupt after each dma transaction ends. */
		amb_desc->lli->attr = amb_chan->rt_attr | DMA_DESC_ID;
		amb_desc->lli->xfr_count = period_len;
		/* rpt_addr points to amb_desc->lli->rpt */
		amb_desc->lli->rpt_addr =
			amb_desc->txd.phys + sizeof(struct ambdma_lli) - 4;
		/* here we initialize rpt to 0 */
		amb_desc->lli->rpt = 0;

		if (first == NULL)
			first = amb_desc;
		else {
			prev->lli->next_desc = amb_desc->txd.phys;
			list_add_tail(&amb_desc->desc_node, &first->tx_list);
		}

		prev = amb_desc;

		left_len -= period_len;
		buf_addr += period_len;

		/* our dma controller can't transfer data larger than 4M Bytes,
		 * but it seems that no use case will transfer so large data,
		 * so we just trigger a BUG here for reminder. */
		BUG_ON(amb_desc->lli->xfr_count > AMBARELLA_DMA_MAX_LENGTH);
	} while (left_len > 0);

	/* lets make a cyclic list */
	amb_desc->lli->next_desc = first->txd.phys;

	/* First descriptor of the chain embedds additional information */
	first->txd.cookie = -EBUSY;
	first->len = buf_len;

	return &first->txd;

dma_cyclic_err:
	dev_err(&chan->dev->device, "prep_dma_cyclic error: %p\n", amb_desc);
	ambdma_put_desc(amb_chan, first);
	return NULL;
}


static struct dma_async_tx_descriptor *ambdma_prep_slave_sg(
		struct dma_chan *chan, struct scatterlist *sgl,
		unsigned int sg_len, enum dma_transfer_direction direction,
		unsigned long flags, void *context)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc, *first = NULL, *prev = NULL;
	struct scatterlist *sgent;
	size_t total_len = 0;
	unsigned i = 0;

	if (sg_len == 0) {
		pr_err("%s: sg length is zero!\n", __func__);
		return NULL;
	}

	for_each_sg(sgl, sgent, sg_len, i) {
		amb_desc = ambdma_get_desc(amb_chan);
		if (!amb_desc)
			goto slave_sg_err;

		amb_desc->is_cyclic = 0;

		if (direction == DMA_MEM_TO_DEV) {
			amb_desc->lli->src = sg_dma_address(sgent);
			amb_desc->lli->dst = amb_chan->rt_addr;
		} else if (direction == DMA_DEV_TO_MEM) {
			amb_desc->lli->src = amb_chan->rt_addr;
			amb_desc->lli->dst = sg_dma_address(sgent);
		} else {
			goto slave_sg_err;
		}
		amb_desc->lli->attr = amb_chan->rt_attr;
		amb_desc->lli->xfr_count = sg_dma_len(sgent);
		/* rpt_addr points to amb_desc->lli->rpt */
		amb_desc->lli->rpt_addr =
			amb_desc->txd.phys + sizeof(struct ambdma_lli) - 4;
		/* here we initialize rpt to 0 */
		amb_desc->lli->rpt = 0;

		if (first == NULL)
			first = amb_desc;
		else {
			prev->lli->next_desc = amb_desc->txd.phys;
			list_add_tail(&amb_desc->desc_node, &first->tx_list);
		}

		prev = amb_desc;
		total_len += amb_desc->lli->xfr_count;

		/* our dma controller can't transfer data larger than 4M Bytes,
		 * but it seems that no use case will transfer so large data,
		 * so we just trigger a BUG here for reminder. */
		BUG_ON(amb_desc->lli->xfr_count > AMBARELLA_DMA_MAX_LENGTH);
	}

	/* set EOC flag to specify the last descriptor */
	amb_desc->lli->attr |= DMA_DESC_EOC;

	/* First descriptor of the chain embedds additional information */
	first->txd.cookie = -EBUSY;
	first->txd.flags = flags; /* client is in control of this ack */
	first->len = total_len;

	return &first->txd;

slave_sg_err:
	dev_err(&chan->dev->device, "prep_slave_sg error: %p\n", amb_desc);
	ambdma_put_desc(amb_chan, first);
	return NULL;
}

static struct dma_async_tx_descriptor *ambdma_prep_dma_memcpy(
		struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
		size_t len, unsigned long flags)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc = NULL, *first = NULL, *prev = NULL;
	size_t left_len = len, xfer_count;

	if (unlikely(!len)) {
		pr_info("%s: length is zero!\n", __func__);
		return NULL;
	}

	do{
		amb_desc = ambdma_get_desc(amb_chan);
		if (!amb_desc)
			goto dma_memcpy_err;

		amb_desc->is_cyclic = 0;

		amb_desc->lli->src = src;
		amb_desc->lli->dst = dst;
		amb_desc->lli->attr = DMA_DESC_RM | DMA_DESC_WM | DMA_DESC_IE |
				DMA_DESC_ST | DMA_DESC_BLK_32B | DMA_DESC_TS_4B;
		xfer_count = min(left_len, (size_t)AMBARELLA_DMA_MAX_LENGTH);
		amb_desc->lli->xfr_count = xfer_count;
		/* rpt_addr points to amb_desc->lli->rpt */
		amb_desc->lli->rpt_addr =
			amb_desc->txd.phys + sizeof(struct ambdma_lli) - 4;
		/* here we initialize rpt to 0 */
		amb_desc->lli->rpt = 0;

		if (first == NULL)
			first = amb_desc;
		else {
			prev->lli->next_desc = amb_desc->txd.phys;
			list_add_tail(&amb_desc->desc_node, &first->tx_list);
		}

		prev = amb_desc;

		src += xfer_count;
		dst += xfer_count;
		left_len -= xfer_count;

	} while (left_len > 0);

	/* set EOC flag to specify the last descriptor */
	amb_desc->lli->attr |= DMA_DESC_EOC;

	/* First descriptor of the chain embedds additional information */
	first->txd.cookie = -EBUSY;
	first->txd.flags = flags; /* client is in control of this ack */
	first->len = len;

	return &first->txd;

dma_memcpy_err:
	dev_err(&chan->dev->device, "prep_dma_memcpy error: %p\n", amb_desc);
	ambdma_put_desc(amb_chan, first);

	return NULL;
}

static struct dma_chan *ambdma_of_xlate(struct of_phandle_args *dma_spec,
			       struct of_dma *ofdma)
{
	struct ambdma_device *amb_dma = ofdma->of_dma_data;
	unsigned int request = dma_spec->args[0];
	struct dma_chan *chan;

	if (request >= amb_dma->nr_requests)
		return NULL;

	if (amb_dma->reg_scr == NULL)
		return of_dma_xlate_by_chan_id(dma_spec, ofdma);

	chan = dma_get_any_slave_channel(&amb_dma->dma_dev);
	if (!chan) {
		dev_err(amb_dma->dma_dev.dev, "can't get a dma channel\n");
		return NULL;
	}

	regmap_update_bits(amb_dma->reg_scr, AHBSP_DMA_CHANNEL_SEL_OFFSET,
		0xf << (chan->chan_id * 4), request << (chan->chan_id * 4));

	return chan;
}

static int ambarella_dma_probe(struct platform_device *pdev)
{
	struct ambdma_device *amb_dma;
	struct ambdma_chan *amb_chan;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	int val, i, ret = 0;

	/* alloc the amba dma engine struct */
	amb_dma = devm_kzalloc(&pdev->dev, sizeof(*amb_dma), GFP_KERNEL);
	if (amb_dma == NULL) {
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "No mem resource for dma!\n");
		ret = -ENXIO;
		goto ambdma_dma_probe_exit;
	}

	amb_dma->regbase =
		devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!amb_dma->regbase) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit;
	}

	amb_dma->reg_scr = syscon_regmap_lookup_by_phandle(np, "amb,scr-regmap");
	if (IS_ERR(amb_dma->reg_scr))
		amb_dma->reg_scr = NULL;

	amb_dma->dma_irq = platform_get_irq(pdev, 0);
	if (amb_dma->dma_irq < 0) {
		ret = -EINVAL;
		goto ambdma_dma_probe_exit;
	}

	amb_dma->support_prs = !!of_find_property(np, "amb,support-prs", NULL);

	ret = of_property_read_u32(np, "dma-channels", &amb_dma->nr_channels);
	if (ret) {
		dev_err(&pdev->dev, "failed to get dma-channels\n");
		goto ambdma_dma_probe_exit;
	}

	ret = of_property_read_u32(np, "dma-requests", &amb_dma->nr_requests);
	if (ret) {
		dev_err(&pdev->dev, "failed to get dma-requests\n");
		goto ambdma_dma_probe_exit;
	}

#if defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
	ret = of_property_read_u32(np, "amb,dma-startchannel", &amb_dma->nr_start);
	if (ret) {
		amb_dma->nr_start = 0;
	}
#endif /* CONFIG_ARCH_AMBARELLA_AMBALINK */

	/* create a pool of consistent memory blocks for hardware descriptors */
	amb_dma->lli_pool = dma_pool_create("ambdma_lli_pool",
			&pdev->dev, sizeof(struct ambdma_lli), 16, 0);
	if (!amb_dma->lli_pool) {
		dev_err(&pdev->dev, "No memory for descriptors dma pool\n");
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit;
	}

	/* alloc dummy_lli and dummy_data for terminate usage. */
	amb_dma->dummy_lli = dma_pool_alloc(amb_dma->lli_pool,
			GFP_KERNEL, &amb_dma->dummy_lli_phys);
	if (amb_dma->dummy_lli == NULL) {
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit1;
	}

	amb_dma->dummy_data = dma_pool_alloc(amb_dma->lli_pool,
			GFP_KERNEL, &amb_dma->dummy_data_phys);
	if (amb_dma->dummy_data == NULL) {
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit2;
	}

	amb_dma->dummy_lli->attr = DMA_DESC_EOC | DMA_DESC_WM |
					DMA_DESC_NI | DMA_DESC_IE |
					DMA_DESC_ST | DMA_DESC_ID;
	amb_dma->dummy_lli->next_desc = amb_dma->dummy_lli_phys;
	amb_dma->dummy_lli->xfr_count = 0;
	amb_dma->dummy_lli->src = 0;
	amb_dma->dummy_lli->dst = amb_dma->dummy_data_phys;
	/* rpt_addr points to ambdma_lli->rpt field */
	amb_dma->dummy_lli->rpt_addr =
		amb_dma->dummy_lli_phys + sizeof(struct ambdma_lli) - 4;

	/* Init dma_device struct */
	dma_cap_zero(amb_dma->dma_dev.cap_mask);
	dma_cap_set(DMA_SLAVE, amb_dma->dma_dev.cap_mask);
	dma_cap_set(DMA_CYCLIC, amb_dma->dma_dev.cap_mask);
	dma_cap_set(DMA_MEMCPY, amb_dma->dma_dev.cap_mask);

#if defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
	/* cf. https://lkml.org/lkml/2014/1/20/5 */
	dma_cap_set(DMA_PRIVATE, amb_dma->dma_dev.cap_mask);
#endif

	INIT_LIST_HEAD(&amb_dma->dma_dev.channels);
	amb_dma->dma_dev.dev = &pdev->dev;
	amb_dma->dma_dev.device_alloc_chan_resources = ambdma_alloc_chan_resources;
	amb_dma->dma_dev.device_free_chan_resources = ambdma_free_chan_resources;
	amb_dma->dma_dev.device_tx_status = ambdma_tx_status;
	amb_dma->dma_dev.device_issue_pending = ambdma_issue_pending;
	amb_dma->dma_dev.device_prep_dma_cyclic = ambdma_prep_dma_cyclic;
	amb_dma->dma_dev.device_prep_slave_sg = ambdma_prep_slave_sg;
	amb_dma->dma_dev.device_prep_dma_memcpy = ambdma_prep_dma_memcpy;
	amb_dma->dma_dev.device_pause = ambdma_pause;
	amb_dma->dma_dev.device_resume = ambdma_resume;
	amb_dma->dma_dev.device_config = ambdma_config;
	amb_dma->dma_dev.device_terminate_all = ambdma_terminate_all;
	/* DMA capabilities */
	amb_dma->dma_dev.src_addr_widths = AMBARELLA_DMA_BUSWIDTHS;
	amb_dma->dma_dev.dst_addr_widths = AMBARELLA_DMA_BUSWIDTHS;
	amb_dma->dma_dev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	amb_dma->dma_dev.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;

	/* init dma_chan struct */
	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];

		spin_lock_init(&amb_chan->lock);
		amb_chan->amb_dma = amb_dma;
		amb_chan->status = AMBDMA_STATUS_IDLE;
		amb_chan->chan.device = &amb_dma->dma_dev;
		INIT_LIST_HEAD(&amb_chan->active_list);
		INIT_LIST_HEAD(&amb_chan->queue);
		INIT_LIST_HEAD(&amb_chan->free_list);
		INIT_LIST_HEAD(&amb_chan->stopping_list);

		tasklet_init(&amb_chan->tasklet, ambdma_tasklet,
				(unsigned long)amb_chan);
		dma_cookie_init(&amb_chan->chan);

#if defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
		if (i < amb_dma->nr_start) {
			amb_chan->status = AMBDMA_STATUS_BUSY;
			amb_chan->force_stop = 1;
		}
#endif /* CONFIG_ARCH_AMBARELLA_AMBALINK */

		list_add_tail(&amb_chan->chan.device_node,
				&amb_dma->dma_dev.channels);
	}

	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];

		/*
		 * I2S_RX_DMA_CHAN and I2S_TX_DMA_CHAN may be used
		 * for fastboot in Amboot
		 */
		if ((i == I2S_RX_DMA_CHAN || i == I2S_TX_DMA_CHAN)
			&& ambdma_chan_is_enabled(amb_chan)) {
			amb_chan->status = AMBDMA_STATUS_BUSY;
			amb_chan->force_stop = 1;
			continue;
		}

		writel(0, dma_chan_sta_reg(amb_chan));
		val = DMA_CHANX_CTR_WM | DMA_CHANX_CTR_RM | DMA_CHANX_CTR_NI;
		writel(val, dma_chan_ctr_reg(amb_chan));
	}

	ret = request_irq(amb_dma->dma_irq, ambdma_dma_irq_handler,
			IRQF_SHARED | IRQF_TRIGGER_HIGH,
			dev_name(&pdev->dev), amb_dma);
	if (ret)
		goto ambdma_dma_probe_exit3;

	ret = dma_async_device_register(&amb_dma->dma_dev);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register slave DMA device: %d\n", ret);
		goto ambdma_dma_probe_exit4;
	}

	ret = of_dma_controller_register(np, ambdma_of_xlate, amb_dma);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register controller\n");
		goto ambdma_dma_probe_exit5;
	}
#if defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
	/* of_dma_controller_register will set all as unused. */
	for (i= 0; i < amb_dma->nr_start; i++) {
		/* Skip channels used by RTOS */
		amb_chan = &amb_dma->amb_chan[i];
		amb_chan->chan.client_count++;
	}
#endif /* CONFIG_ARCH_AMBARELLA_AMBALINK */

	proc_create_data("dma", S_IRUGO|S_IWUSR,
		get_ambarella_proc_dir(), &proc_ambdma_fops, amb_dma);

	platform_set_drvdata(pdev, amb_dma);

	dev_info(&pdev->dev, "Ambarella DMA Engine \n");

	return 0;

ambdma_dma_probe_exit5:
	dma_async_device_unregister(&amb_dma->dma_dev);

ambdma_dma_probe_exit4:
	free_irq(amb_dma->dma_irq, amb_dma);

ambdma_dma_probe_exit3:
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_data,
			amb_dma->dummy_data_phys);

ambdma_dma_probe_exit2:
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_lli,
			amb_dma->dummy_lli_phys);

ambdma_dma_probe_exit1:
	dma_pool_destroy(amb_dma->lli_pool);

ambdma_dma_probe_exit:
	return ret;
}

static int ambarella_dma_remove(struct platform_device *pdev)
{
	struct ambdma_device *amb_dma = platform_get_drvdata(pdev);
	struct ambdma_chan *amb_chan;
	int i;

	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];
		ambdma_stop_channel(amb_chan);

		tasklet_disable(&amb_chan->tasklet);
		tasklet_kill(&amb_chan->tasklet);
	}

	if (pdev->dev.of_node)
		of_dma_controller_free(pdev->dev.of_node);

	dma_async_device_unregister(&amb_dma->dma_dev);

	free_irq(amb_dma->dma_irq, amb_dma);
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_lli,
			amb_dma->dummy_lli_phys);
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_data,
			amb_dma->dummy_data_phys);
	dma_pool_destroy(amb_dma->lli_pool);

	kfree(amb_dma);

	return 0;
}

static const struct of_device_id ambarella_dma_dt_ids[] = {
	{.compatible = "ambarella,dma", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_dma_dt_ids);

static struct platform_driver ambarella_dma_driver = {
	.probe		= ambarella_dma_probe,
	.remove		= ambarella_dma_remove,

	.driver		= {
		.name	= "ambarella-dma",
		.of_match_table = ambarella_dma_dt_ids,
	},
};

static int __init ambarella_dma_init(void)
{
	return platform_driver_register(&ambarella_dma_driver);
}

static void __exit ambarella_dma_exit(void)
{
	platform_driver_unregister(&ambarella_dma_driver);
}

subsys_initcall(ambarella_dma_init);
module_exit(ambarella_dma_exit);

MODULE_DESCRIPTION("Ambarella DMA Engine System Driver");
MODULE_AUTHOR("Jian He <jianhe@ambarella.com>");
MODULE_LICENSE("GPL");

