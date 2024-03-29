/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/pm_runtime.h>

#include "mdss_mdp.h"
#include "mdss_panel.h"
#include "mdss_debug.h"

#define VSYNC_EXPIRE_TICK 6

#define MAX_SESSIONS 2

/* wait for at most 2 vsync for lowest refresh rate (24hz) */
#define KOFF_TIMEOUT msecs_to_jiffies(84)

#define STOP_TIMEOUT msecs_to_jiffies(16 * (VSYNC_EXPIRE_TICK + 2))
#define POWER_COLLAPSE_TIME msecs_to_jiffies(100)

struct mdss_mdp_cmd_ctx {
	struct mdss_mdp_ctl *ctl;
	u32 pp_num;
	u8 ref_cnt;
	struct completion pp_comp;
	struct completion stop_comp;
	struct list_head vsync_handlers;
	int panel_power_state;
	atomic_t koff_cnt;
	int clk_enabled;
	int vsync_enabled;
	int rdptr_enabled;
	struct mutex clk_mtx;
	spinlock_t clk_lock;
	spinlock_t koff_lock;
	struct work_struct clk_work;
	struct work_struct pp_done_work;
	atomic_t pp_done_cnt;
	struct mdss_panel_recovery recovery;
	struct mdss_mdp_cmd_ctx *sync_ctx;	/* for left + right, partial update */
};

struct mdss_mdp_cmd_ctx mdss_mdp_cmd_ctx_list[MAX_SESSIONS];

static int mdss_mdp_cmd_do_notifier(struct mdss_mdp_cmd_ctx *ctx);

static bool __mdss_mdp_cmd_panel_power_on(struct mdss_mdp_cmd_ctx *ctx)
{
	return (ctx->panel_power_state == MDSS_PANEL_POWER_ON);
}

static inline u32 mdss_mdp_cmd_line_count(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_mixer *mixer;
	u32 cnt = 0xffff;	/* init it to an invalid value */
	u32 init;
	u32 height;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (!mixer) {
		mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_RIGHT);
		if (!mixer) {
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
			goto exit;
		}
	}

	init = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_VSYNC_INIT_VAL) & 0xffff;

	height = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_HEIGHT) & 0xffff;

	if (height < init) {
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
		goto exit;
	}

	cnt = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_INT_COUNT_VAL) & 0xffff;

	if (cnt < init)		/* wrap around happened at height */
		cnt += (height - init);
	else
		cnt -= init;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	pr_debug("cnt=%d init=%d height=%d\n", cnt, init, height);
exit:
	return cnt;
}


static int mdss_mdp_cmd_tearcheck_cfg(struct mdss_mdp_ctl *ctl,
				      struct mdss_mdp_mixer *mixer)
{
	struct mdss_mdp_pp_tear_check *te;
	struct mdss_panel_info *pinfo;
	u32 vsync_clk_speed_hz, total_lines, vclks_line, cfg;

	if (IS_ERR_OR_NULL(ctl->panel_data)) {
		pr_err("no panel data\n");
		return -ENODEV;
	}

	pinfo = &ctl->panel_data->panel_info;
	te = &ctl->panel_data->panel_info.te;

	mdss_mdp_vsync_clk_enable(1);

	vsync_clk_speed_hz =
		mdss_mdp_get_clk_rate(MDSS_CLK_MDP_VSYNC);

	total_lines = mdss_panel_get_vtotal(pinfo);

	total_lines *= pinfo->mipi.frame_rate;

	vclks_line = (total_lines) ? vsync_clk_speed_hz / total_lines : 0;

	cfg = BIT(19);
	if (pinfo->mipi.hw_vsync_mode)
		cfg |= BIT(20);

	if (te->refx100)
		vclks_line = vclks_line * pinfo->mipi.frame_rate *
			100 / te->refx100;
	else {
		pr_warn("refx100 cannot be zero! Use 6000 as default\n");
		vclks_line = vclks_line * pinfo->mipi.frame_rate *
			100 / 6000;
	}

	cfg |= vclks_line;

	pr_debug("%s: yres=%d vclks=%x height=%d init=%d rd=%d start=%d ",
		__func__, pinfo->yres, vclks_line, te->sync_cfg_height,
		 te->vsync_init_val, te->rd_ptr_irq, te->start_pos);
	pr_debug("thrd_start =%d thrd_cont=%d\n",
		te->sync_threshold_start, te->sync_threshold_continue);

	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_VSYNC, cfg);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_HEIGHT,
				te->sync_cfg_height);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_VSYNC_INIT_VAL,
				te->vsync_init_val);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_RD_PTR_IRQ,
				te->rd_ptr_irq);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_START_POS,
				te->start_pos);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_THRESH,
				((te->sync_threshold_continue << 16) |
				 te->sync_threshold_start));
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_TEAR_CHECK_EN,
				te->tear_check_en);
	return 0;
}

static int mdss_mdp_cmd_tearcheck_setup(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_mixer *mixer;
	int rc = 0;
	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (mixer) {
		rc = mdss_mdp_cmd_tearcheck_cfg(ctl, mixer);
		if (rc)
			goto err;
	}
	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_RIGHT);
	if (mixer)
		rc = mdss_mdp_cmd_tearcheck_cfg(ctl, mixer);
 err:
	return rc;
}

static inline void mdss_mdp_cmd_clk_on(struct mdss_mdp_cmd_ctx *ctx)
{
	unsigned long flags;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	int irq_en;

	if (!__mdss_mdp_cmd_panel_power_on(ctx))
		return;

	mutex_lock(&ctx->clk_mtx);
	MDSS_XLOG(ctx->pp_num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
						ctx->rdptr_enabled);
	if (!ctx->clk_enabled) {
		mdss_bus_bandwidth_ctrl(true);
		ctx->clk_enabled = 1;

		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
		mdss_mdp_ctl_intf_event
			(ctx->ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)1);
		mdss_mdp_hist_intr_setup(&mdata->hist_intr, MDSS_IRQ_RESUME);
	}
	spin_lock_irqsave(&ctx->clk_lock, flags);
	irq_en =  !ctx->rdptr_enabled;
	ctx->rdptr_enabled = VSYNC_EXPIRE_TICK;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if (irq_en)
		mdss_mdp_irq_enable(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);

	mutex_unlock(&ctx->clk_mtx);
}

static inline void mdss_mdp_cmd_clk_off(struct mdss_mdp_cmd_ctx *ctx)
{
	unsigned long flags;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	int set_clk_off = 0;

	mutex_lock(&ctx->clk_mtx);
	MDSS_XLOG(ctx->pp_num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
						ctx->rdptr_enabled);
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!ctx->rdptr_enabled)
		set_clk_off = 1;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if (ctx->clk_enabled && set_clk_off) {
		ctx->clk_enabled = 0;
		mdss_mdp_hist_intr_setup(&mdata->hist_intr, MDSS_IRQ_SUSPEND);
		mdss_mdp_ctl_intf_event
			(ctx->ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)0);
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
		mdss_bus_bandwidth_ctrl(false);
	}
	mutex_unlock(&ctx->clk_mtx);
}

static void mdss_mdp_cmd_readptr_done(void *arg)
{
	struct mdss_mdp_ctl *ctl = arg;
	struct mdss_mdp_cmd_ctx *ctx = ctl->priv_data;
	struct mdss_mdp_vsync_handler *tmp;
	ktime_t vsync_time;

	if (!ctx) {
		pr_err("invalid ctx\n");
		return;
	}

	vsync_time = ktime_get();
	ctl->vsync_cnt++;
	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
				ctx->rdptr_enabled);

	spin_lock(&ctx->clk_lock);
	list_for_each_entry(tmp, &ctx->vsync_handlers, list) {
		if (tmp->enabled && !tmp->cmd_post_flush)
			tmp->vsync_handler(ctl, vsync_time);
	}

	if (!ctx->vsync_enabled) {
		if (ctx->rdptr_enabled)
			ctx->rdptr_enabled--;

		/* keep clk on during kickoff */
		if (ctx->rdptr_enabled == 0 && atomic_read(&ctx->koff_cnt))
			ctx->rdptr_enabled++;
	}

	if (ctx->rdptr_enabled == 0) {
		mdss_mdp_irq_disable_nosync
			(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);
		complete(&ctx->stop_comp);
		schedule_work(&ctx->clk_work);
	}

	spin_unlock(&ctx->clk_lock);
}

static void mdss_mdp_cmd_underflow_recovery(void *data)
{
	struct mdss_mdp_cmd_ctx *ctx = data;
	unsigned long flags;

	if (!data) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	if (!ctx->ctl)
		return;
	spin_lock_irqsave(&ctx->koff_lock, flags);
	if (atomic_read(&ctx->koff_cnt)) {
		mdss_mdp_ctl_reset(ctx->ctl);
		pr_debug("%s: intf_num=%d\n", __func__,
					ctx->ctl->intf_num);
		atomic_dec(&ctx->koff_cnt);
		mdss_mdp_irq_disable_nosync(MDSS_MDP_IRQ_PING_PONG_COMP,
						ctx->pp_num);
		complete_all(&ctx->pp_comp);
	}
	spin_unlock_irqrestore(&ctx->koff_lock, flags);
}

static void mdss_mdp_cmd_pingpong_done(void *arg)
{
	struct mdss_mdp_ctl *ctl = arg;
	struct mdss_mdp_cmd_ctx *ctx = ctl->priv_data;
	struct mdss_mdp_vsync_handler *tmp;
	ktime_t vsync_time;

	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	mdss_mdp_ctl_perf_set_transaction_status(ctl,
		PERF_HW_MDP_STATE, PERF_STATUS_DONE);

	spin_lock(&ctx->clk_lock);
	list_for_each_entry(tmp, &ctx->vsync_handlers, list) {
		if (tmp->enabled && tmp->cmd_post_flush)
			tmp->vsync_handler(ctl, vsync_time);
	}
	spin_unlock(&ctx->clk_lock);

	spin_lock(&ctx->koff_lock);
	mdss_mdp_irq_disable_nosync(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num);
	complete_all(&ctx->pp_comp);
	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
					ctx->rdptr_enabled);

	if (atomic_read(&ctx->koff_cnt)) {
		if (atomic_dec_return(&ctx->koff_cnt)) {
			pr_err("%s: too many kickoffs=%d!\n", __func__,
			       atomic_read(&ctx->koff_cnt));
			atomic_set(&ctx->koff_cnt, 0);
		}
		if (mdss_mdp_cmd_do_notifier(ctx)) {
			atomic_inc(&ctx->pp_done_cnt);
			schedule_work(&ctx->pp_done_work);
		}
	} else
		pr_err("%s: should not have pingpong interrupt!\n", __func__);

	pr_debug("%s: ctl_num=%d intf_num=%d ctx=%d kcnt=%d\n", __func__,
		ctl->num, ctl->intf_num, ctx->pp_num, atomic_read(&ctx->koff_cnt));

	spin_unlock(&ctx->koff_lock);
}

static void pingpong_done_work(struct work_struct *work)
{
	struct mdss_mdp_cmd_ctx *ctx =
		container_of(work, typeof(*ctx), pp_done_work);

	if (ctx->ctl) {
		while (atomic_add_unless(&ctx->pp_done_cnt, -1, 0))
			mdss_mdp_ctl_notify(ctx->ctl, MDP_NOTIFY_FRAME_DONE);

		mdss_mdp_ctl_perf_release_bw(ctx->ctl);
	}
}

static void clk_ctrl_work(struct work_struct *work)
{
	struct mdss_mdp_cmd_ctx *ctx =
		container_of(work, typeof(*ctx), clk_work);

	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	mdss_mdp_cmd_clk_off(ctx);
}

static int mdss_mdp_cmd_add_vsync_handler(struct mdss_mdp_ctl *ctl,
		struct mdss_mdp_vsync_handler *handle)
{
	struct mdss_mdp_ctl *sctl = NULL;
	struct mdss_mdp_cmd_ctx *ctx, *sctx = NULL;
	unsigned long flags;
	bool enable_rdptr = false;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return -ENODEV;
	}

	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
					ctx->rdptr_enabled);
	sctl = mdss_mdp_get_split_ctl(ctl);
	if (sctl)
		sctx = (struct mdss_mdp_cmd_ctx *) sctl->priv_data;

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!handle->enabled) {
		handle->enabled = true;
		list_add(&handle->list, &ctx->vsync_handlers);

		enable_rdptr = !handle->cmd_post_flush;
		if (enable_rdptr) {
			ctx->vsync_enabled++;
			if (sctx)
				sctx->vsync_enabled++;
		}
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if (enable_rdptr) {
		mdss_mdp_cmd_clk_on(ctx);
		if (sctx)
			mdss_mdp_cmd_clk_on(sctx);
	}

	return 0;
}

static int mdss_mdp_cmd_remove_vsync_handler(struct mdss_mdp_ctl *ctl,
		struct mdss_mdp_vsync_handler *handle)
{
	struct mdss_mdp_ctl *sctl;
	struct mdss_mdp_cmd_ctx *ctx, *sctx = NULL;
	unsigned long flags;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return -ENODEV;
	}

	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
				ctx->rdptr_enabled, 0x88888);
	sctl = mdss_mdp_get_split_ctl(ctl);
	if (sctl)
		sctx = (struct mdss_mdp_cmd_ctx *) sctl->priv_data;

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (handle->enabled) {
		handle->enabled = false;
		list_del_init(&handle->list);

		if (!handle->cmd_post_flush) {
			if (ctx->vsync_enabled) {
				ctx->vsync_enabled--;
				if (sctx)
					sctx->vsync_enabled--;
			}
			else
				WARN(1, "unbalanced vsync disable");
		}
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
	return 0;
}

int mdss_mdp_cmd_reconfigure_splash_done(struct mdss_mdp_ctl *ctl, bool handoff)
{
	struct mdss_panel_data *pdata;
	struct mdss_mdp_ctl *sctl = mdss_mdp_get_split_ctl(ctl);
	int ret = 0;

	pdata = ctl->panel_data;

	mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)0);

	pdata->panel_info.cont_splash_enabled = 0;
	if (sctl)
		sctl->panel_data->panel_info.cont_splash_enabled = 0;

	return ret;
}

static int mdss_mdp_cmd_wait4pingpong(struct mdss_mdp_ctl *ctl, void *arg)
{
	struct mdss_mdp_cmd_ctx *ctx;
	struct mdss_panel_data *pdata;
	unsigned long flags;
	int need_wait = 0;
	int rc = 0;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	pdata = ctl->panel_data;

	spin_lock_irqsave(&ctx->koff_lock, flags);
	if (atomic_read(&ctx->koff_cnt) > 0)
		need_wait = 1;
	spin_unlock_irqrestore(&ctx->koff_lock, flags);

	ctl->roi_bkup.w = ctl->width;
	ctl->roi_bkup.h = ctl->height;

	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
			ctx->rdptr_enabled, ctl->roi_bkup.w,
			ctl->roi_bkup.h);

	pr_debug("%s: need_wait=%d  intf_num=%d ctx=%p\n",
			__func__, need_wait, ctl->intf_num, ctx);

	if (need_wait) {
		rc = wait_for_completion_timeout(
				&ctx->pp_comp, KOFF_TIMEOUT);

		if (rc <= 0) {
			u32 status, mask;

			mask = BIT(MDSS_MDP_IRQ_PING_PONG_COMP + ctx->pp_num);
			status = mask & readl_relaxed(ctl->mdata->mdp_base +
					MDSS_MDP_REG_INTR_STATUS);
			if (status) {
				WARN(1, "pp done but irq not triggered\n");
				mdss_mdp_irq_clear(ctl->mdata,
						MDSS_MDP_IRQ_PING_PONG_COMP,
						ctx->pp_num);
				local_irq_save(flags);
				mdss_mdp_cmd_pingpong_done(ctl);
				local_irq_restore(flags);
				rc = 1;
			}

			rc = try_wait_for_completion(&ctx->pp_comp);
		}

		if (rc <= 0) {
			WARN(1, "cmd kickoff timed out (%d) ctl=%d\n",
						rc, ctl->num);
			mdss_dsi_debug_check_te(pdata);
			MDSS_XLOG_TOUT_HANDLER("mdp", "dsi0", "dsi1",
						"edp", "hdmi", "panic");
			rc = -EPERM;
			mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_TIMEOUT);
		} else {
			rc = 0;
		}
	}

	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
					ctx->rdptr_enabled, rc);
	return rc;
}

static int mdss_mdp_cmd_do_notifier(struct mdss_mdp_cmd_ctx *ctx)
{
	struct mdss_mdp_cmd_ctx *sctx;

	sctx = ctx->sync_ctx;
	if (!sctx || atomic_read(&sctx->koff_cnt) == 0)
		return 1;

	return 0;
}

static void mdss_mdp_cmd_set_sync_ctx(
		struct mdss_mdp_ctl *ctl, struct mdss_mdp_ctl *sctl)
{
	struct mdss_mdp_cmd_ctx *ctx, *sctx;

	ctx = (struct mdss_mdp_cmd_ctx *)ctl->priv_data;

	if (!ctl->panel_data->panel_info.partial_update_enabled || !sctl) {
		/* not partial or right only at partial update */
		ctx->sync_ctx = NULL;
		return;
	}

	sctx = (struct mdss_mdp_cmd_ctx *)sctl->priv_data;

	if (!sctl->roi.w && !sctl->roi.h) {
		/* left only */
		ctx->sync_ctx = NULL;
		sctx->sync_ctx = NULL;
	} else  {
		/* left + right */
		ctx->sync_ctx = sctx;
		sctx->sync_ctx = ctx;
	}
}

static int mdss_mdp_cmd_set_partial_roi(struct mdss_mdp_ctl *ctl)
{
	int rc = 0;

	if (!ctl->panel_data->panel_info.partial_update_enabled)
		return rc;

	/* set panel col and page addr */
	rc = mdss_mdp_ctl_intf_event(ctl,
			MDSS_EVENT_ENABLE_PARTIAL_ROI, NULL);
	return rc;
}

static int mdss_mdp_cmd_set_stream_size(struct mdss_mdp_ctl *ctl)
{
	int rc = 0;

	if (!ctl->panel_data->panel_info.partial_update_enabled)
		return rc;

	/* set dsi controller stream size */
	rc = mdss_mdp_ctl_intf_event(ctl,
			MDSS_EVENT_DSI_STREAM_SIZE, NULL);
	return rc;
}

/*
 * There are 3 partial update possibilities
 * left only ==> enable left pingpong_done
 * left + right ==> enable both pingpong_done
 * right only ==> enable right pingpong_done
 *
 * notification is triggered at pingpong_done which will
 * signal timeline to release source buffer
 *
 * for left+right case, pingpong_done is enabled for both and
 * only the last pingpong_done should trigger the notification
 */
int mdss_mdp_cmd_kickoff(struct mdss_mdp_ctl *ctl, void *arg)
{
	struct mdss_mdp_ctl *sctl = NULL;
	struct mdss_mdp_cmd_ctx *ctx, *sctx = NULL;
	unsigned long flags;
	int rc;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	if (ctl->panel_data->panel_info.partial_update_enabled) {
		/* sctl will be null for right only */
		sctl = mdss_mdp_get_split_ctl(ctl);
	}

	if (sctl && (sctl->roi.w == 0 || sctl->roi.h == 0)) {
		/* left update only, set ssctl to null */
		sctl = NULL;
	}

	mdss_mdp_ctl_perf_set_transaction_status(ctl,
		PERF_HW_MDP_STATE, PERF_STATUS_BUSY);

	if (sctl) {
		sctx = (struct mdss_mdp_cmd_ctx *) sctl->priv_data;
		mdss_mdp_ctl_perf_set_transaction_status(sctl,
			PERF_HW_MDP_STATE, PERF_STATUS_BUSY);
	}

	if (!__mdss_mdp_cmd_panel_power_on(ctx)) {
		rc = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_UNBLANK, NULL);
		WARN(rc, "intf %d unblank error (%d)\n", ctl->intf_num, rc);

		rc = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_ON, NULL);
		WARN(rc, "intf %d panel on error (%d)\n", ctl->intf_num, rc);

		ctx->panel_power_state = MDSS_PANEL_POWER_ON;
		if (sctx)
			sctx->panel_power_state = MDSS_PANEL_POWER_ON;
	}

	MDSS_XLOG(ctl->num, ctl->roi.x, ctl->roi.y, ctl->roi.w,
						ctl->roi.h);

	spin_lock_irqsave(&ctx->koff_lock, flags);
	atomic_inc(&ctx->koff_cnt);
	INIT_COMPLETION(ctx->pp_comp);
	if (sctx) {
		atomic_inc(&sctx->koff_cnt);
		INIT_COMPLETION(sctx->pp_comp);
	}
	spin_unlock_irqrestore(&ctx->koff_lock, flags);

	mdss_mdp_cmd_clk_on(ctx);

	mdss_mdp_cmd_set_partial_roi(ctl);

	/*
	 * tx dcs command if had any
	 */
	mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_DSI_CMDLIST_KOFF,
						(void *)&ctx->recovery);
	mdss_mdp_cmd_set_stream_size(ctl);

	mdss_mdp_cmd_set_sync_ctx(ctl, sctl);

	mdss_mdp_irq_enable(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num);
	if (sctx)
		mdss_mdp_irq_enable(MDSS_MDP_IRQ_PING_PONG_COMP, sctx->pp_num);

	mdss_mdp_ctl_write(ctl, MDSS_MDP_REG_CTL_START, 1);	/* Kickoff */

	mdss_mdp_ctl_perf_set_transaction_status(ctl,
		PERF_SW_COMMIT_STATE, PERF_STATUS_DONE);

	if (sctl) {
		mdss_mdp_ctl_perf_set_transaction_status(sctl,
			PERF_SW_COMMIT_STATE, PERF_STATUS_DONE);
	}

	mb();
	MDSS_XLOG(ctl->num,  atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
						ctx->rdptr_enabled);
	return 0;
}

int mdss_mdp_cmd_restore(struct mdss_mdp_ctl *ctl)
{
	pr_debug("%s: called for ctl%d\n", __func__, ctl->num);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
	if (mdss_mdp_cmd_tearcheck_setup(ctl))
		pr_warn("%s: tearcheck setup failed\n", __func__);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	return 0;
}

int mdss_mdp_cmd_stop(struct mdss_mdp_ctl *ctl, int panel_power_state)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	struct mdss_mdp_vsync_handler *tmp, *handle;
	int need_wait = 0;
	int ret = 0;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	list_for_each_entry_safe(handle, tmp, &ctx->vsync_handlers, list)
		mdss_mdp_cmd_remove_vsync_handler(ctl, handle);
	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
				ctx->rdptr_enabled, XLOG_FUNC_ENTRY);

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (ctx->rdptr_enabled) {
		INIT_COMPLETION(ctx->stop_comp);
		need_wait = 1;
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if (need_wait && (wait_for_completion_timeout(&ctx->stop_comp,
		STOP_TIMEOUT) == 0)) {
		WARN(1, "stop cmd time out\n");
		mdss_mdp_irq_disable(MDSS_MDP_IRQ_PING_PONG_RD_PTR,
			ctx->pp_num);
		ctx->rdptr_enabled = 0;
		atomic_set(&ctx->koff_cnt, 0);
	}

	if (cancel_work_sync(&ctx->clk_work))
		pr_debug("no pending clk work\n");

	mdss_mdp_cmd_clk_off(ctx);
	flush_work(&ctx->pp_done_work);
	ctx->panel_power_state = panel_power_state;

	if (ctl->num == 0) {
		ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_BLANK,
				(void *) (long int) panel_power_state);
		WARN(ret, "intf %d unblank error (%d)\n", ctl->intf_num, ret);

		ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_OFF,
				(void *) (long int) panel_power_state);
		WARN(ret, "intf %d unblank error (%d)\n", ctl->intf_num, ret);
	}

	if (panel_power_state != MDSS_PANEL_POWER_OFF) {
		pr_debug("%s: cmd_off with panel always on\n", __func__);
		goto end;
	}

	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num,
				   NULL, NULL);
	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num,
				   NULL, NULL);

	memset(ctx, 0, sizeof(*ctx));
	ctl->priv_data = NULL;

	ctl->stop_fnc = NULL;
	ctl->display_fnc = NULL;
	ctl->wait_pingpong = NULL;
	ctl->add_vsync_handler = NULL;
	ctl->remove_vsync_handler = NULL;

end:
	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
				ctx->rdptr_enabled, XLOG_FUNC_EXIT);
	pr_debug("%s:-\n", __func__);

	return 0;
}

int mdss_mdp_cmd_start(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_cmd_ctx *ctx;
	struct mdss_mdp_mixer *mixer;
	int i, ret;

	pr_debug("%s:+\n", __func__);

	if (!ctl)
		return -EINVAL;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (ctx && (ctx->panel_power_state != MDSS_PANEL_POWER_OFF)) {
		pr_debug("%s: cmd_start with panel always on\n",
			__func__);
		/*
		 * It is possible that the resume was called from the panel
		 * always on state without MDSS every power-collapsed (such
		 * as a case with any other interfaces connected). In such
		 * cases, we need to explictly call the restore function to
		 * enable tearcheck logic.
		 */
		mdss_mdp_cmd_restore(ctl);
		return 0;
	}

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (!mixer) {
		pr_err("mixer not setup correctly\n");
		return -ENODEV;
	}

	for (i = 0; i < MAX_SESSIONS; i++) {
		ctx = &mdss_mdp_cmd_ctx_list[i];
		if (ctx->ref_cnt == 0) {
			ctx->ref_cnt++;
			break;
		}
	}
	if (i == MAX_SESSIONS) {
		pr_err("too many sessions\n");
		return -ENOMEM;
	}

	ctl->priv_data = ctx;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	ctx->ctl = ctl;
	ctx->pp_num = mixer->num;
	init_completion(&ctx->pp_comp);
	init_completion(&ctx->stop_comp);
	spin_lock_init(&ctx->clk_lock);
	spin_lock_init(&ctx->koff_lock);
	mutex_init(&ctx->clk_mtx);
	INIT_WORK(&ctx->clk_work, clk_ctrl_work);
	INIT_WORK(&ctx->pp_done_work, pingpong_done_work);
	atomic_set(&ctx->pp_done_cnt, 0);
	INIT_LIST_HEAD(&ctx->vsync_handlers);

	ctx->recovery.fxn = mdss_mdp_cmd_underflow_recovery;
	ctx->recovery.data = ctx;

	pr_debug("%s: ctx=%p num=%d mixer=%d\n", __func__,
				ctx, ctx->pp_num, mixer->num);
	MDSS_XLOG(ctl->num, atomic_read(&ctx->koff_cnt), ctx->clk_enabled,
					ctx->rdptr_enabled);

	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num,
				   mdss_mdp_cmd_readptr_done, ctl);

	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num,
				   mdss_mdp_cmd_pingpong_done, ctl);

	ret = mdss_mdp_cmd_tearcheck_setup(ctl);

	if (ret) {
		pr_err("tearcheck setup failed\n");
		return ret;
	}

	ctl->stop_fnc = mdss_mdp_cmd_stop;
	ctl->display_fnc = mdss_mdp_cmd_kickoff;
	ctl->wait_pingpong = mdss_mdp_cmd_wait4pingpong;
	ctl->add_vsync_handler = mdss_mdp_cmd_add_vsync_handler;
	ctl->remove_vsync_handler = mdss_mdp_cmd_remove_vsync_handler;
	ctl->read_line_cnt_fnc = mdss_mdp_cmd_line_count;
	ctl->restore_fnc = mdss_mdp_cmd_restore;
	pr_debug("%s:-\n", __func__);

	return 0;
}

