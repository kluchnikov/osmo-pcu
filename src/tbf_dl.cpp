/* Copied from tbf.cpp
 *
 * Copyright (C) 2012 Ivan Klyuchnikov
 * Copyright (C) 2012 Andreas Eversberg <jolly@eversberg.eu>
 * Copyright (C) 2013 by Holger Hans Peter Freyther
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <bts.h>
#include <tbf.h>
#include <rlc.h>
#include <gprs_rlcmac.h>
#include <gprs_debug.h>
#include <gprs_bssgp_pcu.h>
#include <gprs_codel.h>
#include <decoding.h>
#include <encoding.h>
#include <gprs_coding_scheme.h>
#include <gprs_ms.h>
#include <gprs_ms_storage.h>
#include <llc.h>
#include "pcu_utils.h"

extern "C" {
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/gprs/gprs_bssgp_bss.h>
	#include <osmocom/core/bitvec.h>
	#include <osmocom/core/linuxlist.h>
	#include <osmocom/core/logging.h>
	#include <osmocom/core/rate_ctr.h>
	#include <osmocom/core/timer.h>
	#include <osmocom/core/utils.h>
	#include <osmocom/gsm/gsm_utils.h>
	#include <osmocom/gsm/protocol/gsm_04_08.h>
}

#include <errno.h>
#include <string.h>

/* After sending these frames, we poll for ack/nack. */
#define POLL_ACK_AFTER_FRAMES 20

static inline void tbf_update_ms_class(struct gprs_rlcmac_tbf *tbf,
					const uint8_t ms_class)
{
	if (!tbf->ms_class() && ms_class)
		tbf->set_ms_class(ms_class);
}

static void llc_timer_cb(void *_tbf)
{
	struct gprs_rlcmac_dl_tbf *tbf = (struct gprs_rlcmac_dl_tbf *)_tbf;

	if (tbf->state_is_not(GPRS_RLCMAC_FLOW))
		return;

	LOGPTBFDL(tbf, LOGL_DEBUG, "LLC receive timeout, requesting DL ACK\n");

	tbf->request_dl_ack();
}

void gprs_rlcmac_dl_tbf::cleanup()
{
	osmo_timer_del(&m_llc_timer);
}

void gprs_rlcmac_dl_tbf::start_llc_timer()
{
	if (bts_data()->llc_idle_ack_csec > 0) {
		struct timeval tv;

		/* TODO: this ought to be within a constructor */
		m_llc_timer.data = this;
		m_llc_timer.cb = &llc_timer_cb;

		csecs_to_timeval(bts_data()->llc_idle_ack_csec, &tv);
		osmo_timer_schedule(&m_llc_timer, tv.tv_sec, tv.tv_usec);
	}
}

int gprs_rlcmac_dl_tbf::append_data(const uint8_t ms_class,
				const uint16_t pdu_delay_csec,
				const uint8_t *data, const uint16_t len)
{
	LOGPTBFDL(this, LOGL_DEBUG, "appending %u bytes\n", len);
	gprs_llc_queue::MetaInfo info;
	struct msgb *llc_msg = msgb_alloc(len, "llc_pdu_queue");
	if (!llc_msg)
		return -ENOMEM;

	gprs_llc_queue::calc_pdu_lifetime(bts, pdu_delay_csec, &info.expire_time);
	gettimeofday(&info.recv_time, NULL);
	memcpy(msgb_put(llc_msg, len), data, len);
	llc_queue()->enqueue(llc_msg, &info);
	tbf_update_ms_class(this, ms_class);
	start_llc_timer();

	if (state_is(GPRS_RLCMAC_WAIT_RELEASE)) {
		LOGPTBFDL(this, LOGL_DEBUG, "in WAIT RELEASE state (T3193), so reuse TBF\n");
		tbf_update_ms_class(this, ms_class);
		establish_dl_tbf_on_pacch();
	}

	return 0;
}

static int tbf_new_dl_assignment(struct gprs_rlcmac_bts *bts,
				const char *imsi,
				const uint32_t tlli, const uint32_t tlli_old,
				const uint8_t ms_class,
				const uint8_t egprs_ms_class,
				struct gprs_rlcmac_dl_tbf **tbf)
{
	bool ss;
	int8_t use_trx;
	uint16_t ta = GSM48_TA_INVALID;
	struct gprs_rlcmac_ul_tbf *ul_tbf = NULL, *old_ul_tbf;
	struct gprs_rlcmac_dl_tbf *dl_tbf = NULL;
	GprsMs *ms;

	/* check for uplink data, so we copy our informations */
	ms = bts->bts->ms_store().get_ms(tlli, tlli_old, imsi);
	if (ms) {
		ul_tbf = ms->ul_tbf();
		ta = ms->ta();
	}
	/* TODO: if (!ms) create MS before tbf_alloc is called? */

	if (ul_tbf && ul_tbf->m_contention_resolution_done
	 && !ul_tbf->m_final_ack_sent) {
		use_trx = ul_tbf->trx->trx_no;
		ss = false;
		old_ul_tbf = ul_tbf;
	} else {
		use_trx = -1;
		ss = true; /* PCH assignment only allows one timeslot */
		old_ul_tbf = NULL;
	}

	// Create new TBF (any TRX)
/* FIXME: Copy and paste with alloc_ul_tbf */
	/* set number of downlink slots according to multislot class */
	dl_tbf = tbf_alloc_dl_tbf(bts, ms, use_trx, ms_class, egprs_ms_class, ss);

	if (!dl_tbf) {
		LOGP(DTBF, LOGL_NOTICE, "No PDCH resource\n");
		return -EBUSY;
	}
	dl_tbf->update_ms(tlli, GPRS_RLCMAC_DL_TBF);
	dl_tbf->ms()->set_ta(ta);

	LOGPTBFDL(dl_tbf, LOGL_DEBUG, "[DOWNLINK] START\n");

	/* Store IMSI for later look-up and PCH retransmission */
	dl_tbf->assign_imsi(imsi);

	/* trigger downlink assignment and set state to ASSIGN.
	 * we don't use old_downlink, so the possible uplink is used
	 * to trigger downlink assignment. if there is no uplink,
	 * AGCH is used. */
	dl_tbf->trigger_ass(old_ul_tbf);
	*tbf = dl_tbf;
	return 0;
}

/**
 * TODO: split into unit test-able parts...
 */
int gprs_rlcmac_dl_tbf::handle(struct gprs_rlcmac_bts *bts,
		const uint32_t tlli, const uint32_t tlli_old, const char *imsi,
		uint8_t ms_class, uint8_t egprs_ms_class,
		const uint16_t delay_csec,
		const uint8_t *data, const uint16_t len)
{
	struct gprs_rlcmac_dl_tbf *dl_tbf = NULL;
	int rc;
	GprsMs *ms, *ms_old;

	/* check for existing TBF */
	ms = bts->bts->ms_store().get_ms(tlli, tlli_old, imsi);
	if (ms) {
		dl_tbf = ms->dl_tbf();

		/* If we known the GPRS/EGPRS MS class, use it */
		if (ms->ms_class() || ms->egprs_ms_class()) {
			ms_class = ms->ms_class();
			egprs_ms_class = ms->egprs_ms_class();
		}
	}

	if (ms && strlen(ms->imsi()) == 0) {
		ms_old = bts->bts->ms_store().get_ms(0, 0, imsi);
		if (ms_old && ms_old != ms) {
			/* The TLLI has changed (RAU), so there are two MS
			 * objects for the same MS */
			LOGP(DTBF, LOGL_NOTICE,
			     "There is a new MS object for the same MS: (0x%08x, '%s') -> (0x%08x, '%s')\n",
			     ms_old->tlli(), ms_old->imsi(), ms->tlli(), ms->imsi());

			GprsMs::Guard guard_old(ms_old);

			if (!dl_tbf && ms_old->dl_tbf()) {
				LOGP(DTBF, LOGL_NOTICE,
				     "IMSI %s, old TBF %s: moving DL TBF to new MS object\n",
				     imsi, ms_old->dl_tbf()->name());
				dl_tbf = ms_old->dl_tbf();
				/* Move the DL TBF to the new MS */
				dl_tbf->set_ms(ms);
			}
			/* Clean up the old MS object */
			/* TODO: Put this into a separate function, use timer? */
			if (ms_old->ul_tbf() && !ms_old->ul_tbf()->timers_pending(T_MAX))
				tbf_free(ms_old->ul_tbf());
			if (ms_old->dl_tbf() && !ms_old->dl_tbf()->timers_pending(T_MAX))
				tbf_free(ms_old->dl_tbf());

			ms->merge_old_ms(ms_old);
		}
	}

	if (!dl_tbf) {
		rc = tbf_new_dl_assignment(bts, imsi, tlli, tlli_old,
			ms_class, egprs_ms_class, &dl_tbf);
		if (rc < 0)
			return rc;
	}

	/* TODO: ms_class vs. egprs_ms_class is not handled here */
	rc = dl_tbf->append_data(ms_class, delay_csec, data, len);
	dl_tbf->update_ms(tlli, GPRS_RLCMAC_DL_TBF);
	dl_tbf->assign_imsi(imsi);

	return rc;
}

struct msgb *gprs_rlcmac_dl_tbf::llc_dequeue(bssgp_bvc_ctx *bctx)
{
	struct msgb *msg;
	struct timeval tv_now, tv_now2;
	uint32_t octets = 0, frames = 0;
	struct timeval hyst_delta = {0, 0};
	const unsigned keep_small_thresh = 60;
	const gprs_llc_queue::MetaInfo *info;

	if (bts_data()->llc_discard_csec)
		csecs_to_timeval(bts_data()->llc_discard_csec, &hyst_delta);

	gettimeofday(&tv_now, NULL);
	timeradd(&tv_now, &hyst_delta, &tv_now2);

	while ((msg = llc_queue()->dequeue(&info))) {
		const struct timeval *tv_disc = &info->expire_time;
		const struct timeval *tv_recv = &info->recv_time;

		gprs_bssgp_update_queue_delay(tv_recv, &tv_now);

		if (ms() && ms()->codel_state()) {
			int bytes = llc_queue()->octets();
			if (gprs_codel_control(ms()->codel_state(),
					tv_recv, &tv_now, bytes))
				goto drop_frame;
		}

		/* Is the age below the low water mark? */
		if (!gprs_llc_queue::is_frame_expired(&tv_now2, tv_disc))
			break;

		/* Is the age below the high water mark */
		if (!gprs_llc_queue::is_frame_expired(&tv_now, tv_disc)) {
			/* Has the previous message not been dropped? */
			if (frames == 0)
				break;

			/* Hysteresis mode, try to discard LLC messages until
			 * the low water mark has been reached */

			/* Check whether to abort the hysteresis mode */

			/* Is the frame small, perhaps only a TCP ACK? */
			if (msg->len <= keep_small_thresh)
				break;

			/* Is it a GMM message? */
			if (!gprs_llc::is_user_data_frame(msg->data, msg->len))
				break;
		}

		bts->llc_timedout_frame();
drop_frame:
		frames++;
		octets += msg->len;
		msgb_free(msg);
		bts->llc_dropped_frame();
		continue;
	}

	if (frames) {
		LOGPTBFDL(this, LOGL_NOTICE, "Discarding LLC PDU "
			"because lifetime limit reached, "
			"count=%u new_queue_size=%zu\n",
			  frames, llc_queue_size());
		if (frames > 0xff)
			frames = 0xff;
		if (octets > 0xffffff)
			octets = 0xffffff;
		if (bctx)
			bssgp_tx_llc_discarded(bctx, tlli(), frames, octets);
	}

	return msg;
}

bool gprs_rlcmac_dl_tbf::restart_bsn_cycle()
{
	/* If V(S) == V(A) and finished state, we would have received
	 * acknowledgement of all transmitted block.  In this case we would
	 * have transmitted the final block, and received ack from MS. But in
	 * this case we did not receive the final ack indication from MS.  This
	 * should never happen if MS works correctly.
	 */
	if (m_window.window_empty()) {
		LOGPTBFDL(this, LOGL_DEBUG, "MS acked all blocks\n");
		return false;
	}

	/* cycle through all unacked blocks */
	int resend = m_window.mark_for_resend();

	/* At this point there should be at least one unacked block
	 * to be resent. If not, this is an software error. */
	if (resend == 0) {
		LOGPTBFDL(this, LOGL_ERROR,
			  "FIXME: Software error: There are no unacknowledged blocks, but V(A) != V(S). PLEASE FIX!\n");
		return false;
	}

	return true;
}

int gprs_rlcmac_dl_tbf::take_next_bsn(uint32_t fn,
	int previous_bsn, bool *may_combine)
{
	int bsn;
	int data_len2, force_data_len = -1;
	GprsCodingScheme force_cs;

	bsn = m_window.resend_needed();

	if (previous_bsn >= 0) {
		force_cs = m_rlc.block(previous_bsn)->cs_current_trans;
		if (!force_cs.isEgprs())
			return -1;
		force_data_len = m_rlc.block(previous_bsn)->len;
	}

	if (bsn >= 0) {
		if (previous_bsn == bsn)
			return -1;

		if (previous_bsn >= 0 &&
			m_window.mod_sns(bsn - previous_bsn) > RLC_EGPRS_MAX_BSN_DELTA)
			return -1;

		if (is_egprs_enabled()) {
			/* Table 8.1.1.2 and Table 8.1.1.1 of 44.060 */
			m_rlc.block(bsn)->cs_current_trans =
				GprsCodingScheme::get_retx_mcs(
					m_rlc.block(bsn)->cs_init,
					ms()->current_cs_dl(),
					bts->bts_data()->dl_arq_type);

			LOGPTBFDL(this, LOGL_DEBUG,
				  "initial_cs_dl(%d) last_mcs(%d) demanded_mcs(%d) cs_trans(%d) arq_type(%d) bsn(%d)\n",
				  m_rlc.block(bsn)->cs_init.to_num(),
				  m_rlc.block(bsn)->cs_last.to_num(),
				  ms()->current_cs_dl().to_num(),
				  m_rlc.block(bsn)->cs_current_trans.to_num(),
				  bts->bts_data()->dl_arq_type, bsn);

			/* TODO: Need to remove this check when MCS-8 -> MCS-6
			 * transistion is handled.
			 * Refer commit be881c028fc4da00c4046ecd9296727975c206a3
			 */
			if (m_rlc.block(bsn)->cs_init == GprsCodingScheme::MCS8)
				m_rlc.block(bsn)->cs_current_trans =
					GprsCodingScheme::MCS8;
		} else
			m_rlc.block(bsn)->cs_current_trans =
					m_rlc.block(bsn)->cs_last;

		data_len2 = m_rlc.block(bsn)->len;
		if (force_data_len > 0 && force_data_len != data_len2)
			return -1;
		LOGPTBFDL(this, LOGL_DEBUG, "Resending BSN %d\n", bsn);
		/* re-send block with negative aknowlegement */
		m_window.m_v_b.mark_unacked(bsn);
		bts->rlc_resent();
	} else if (state_is(GPRS_RLCMAC_FINISHED)) {
		LOGPTBFDL(this, LOGL_DEBUG,
			  "Restarting at BSN %d, because all blocks have been transmitted.\n",
			  m_window.v_a());
		bts->rlc_restarted();
		if (restart_bsn_cycle())
			return take_next_bsn(fn, previous_bsn, may_combine);
	} else if (dl_window_stalled()) {
		LOGPTBFDL(this, LOGL_NOTICE,
			  "Restarting at BSN %d, because the window is stalled.\n",
			  m_window.v_a());
		bts->rlc_stalled();
		if (restart_bsn_cycle())
			return take_next_bsn(fn, previous_bsn, may_combine);
	} else if (have_data()) {
		GprsCodingScheme new_cs;
		/* New blocks may be send */
		new_cs = force_cs ? force_cs : current_cs();
		LOGPTBFDL(this, LOGL_DEBUG,
			  "Sending new block at BSN %d, CS=%s\n",
			  m_window.v_s(), new_cs.name());

		bsn = create_new_bsn(fn, new_cs);
	} else if (!m_window.window_empty()) {
		LOGPTBFDL(this, LOGL_DEBUG,
			  "Restarting at BSN %d, because all blocks have been transmitted (FLOW).\n",
			  m_window.v_a());
		bts->rlc_restarted();
		if (restart_bsn_cycle())
			return take_next_bsn(fn, previous_bsn, may_combine);
	} else {
		/* Nothing left to send, create dummy LLC commands */
		LOGPTBFDL(this, LOGL_DEBUG,
			  "Sending new dummy block at BSN %d, CS=%s\n",
			  m_window.v_s(), current_cs().name());
		bsn = create_new_bsn(fn, current_cs());
		/* Don't send a second block, so don't set cs_current_trans */
	}

	if (bsn < 0) {
		/* we just send final block again */
		LOGPTBFDL(this, LOGL_DEBUG,
			  "Nothing else to send, Re-transmit final block!\n");
		bsn = m_window.v_s_mod(-1);
		bts->rlc_final_block_resent();
		bts->rlc_resent();
	}

	*may_combine = m_rlc.block(bsn)->cs_current_trans.numDataBlocks() > 1;

	return bsn;
}

/*
 * Create DL data block
 * The messages are fragmented and forwarded as data blocks.
 */
struct msgb *gprs_rlcmac_dl_tbf::create_dl_acked_block(uint32_t fn, uint8_t ts)
{
	int bsn, bsn2 = -1;
	bool may_combine;

	LOGPTBFDL(this, LOGL_DEBUG, "downlink (V(A)==%d .. V(S)==%d)\n",
		  m_window.v_a(), m_window.v_s());

	bsn = take_next_bsn(fn, -1, &may_combine);
	if (bsn < 0)
		return NULL;

	if (may_combine)
		bsn2 = take_next_bsn(fn, bsn, &may_combine);

	return create_dl_acked_block(fn, ts, bsn, bsn2);
}

/* depending on the current TBF, we assign on PACCH or AGCH */
void gprs_rlcmac_dl_tbf::trigger_ass(struct gprs_rlcmac_tbf *old_tbf)
{
	/* stop pending timer */
	stop_timers("assignment (DL-TBF)");

	/* check for downlink tbf:  */
	if (old_tbf) {
		LOGPTBFDL(this, LOGL_DEBUG, "Send dowlink assignment on PACCH, because %s exists\n", old_tbf->name());
		TBF_SET_ASS_STATE_DL(old_tbf, GPRS_RLCMAC_DL_ASS_SEND_ASS);
		old_tbf->was_releasing = old_tbf->state_is(GPRS_RLCMAC_WAIT_RELEASE);

		/* change state */
		TBF_SET_ASS_ON(this, GPRS_RLCMAC_FLAG_PACCH, true);

		/* start timer */
		T_START(this, T0, T_ASS_PACCH_SEC, 0, "assignment (PACCH)", true);
	} else {
		LOGPTBFDL(this, LOGL_DEBUG, "Send dowlink assignment on PCH, no TBF exist (IMSI=%s)\n",
			  imsi());
		was_releasing = state_is(GPRS_RLCMAC_WAIT_RELEASE);

		/* change state */
		TBF_SET_ASS_ON(this, GPRS_RLCMAC_FLAG_CCCH, false);

		/* send immediate assignment */
		bts->snd_dl_ass(this, 0, imsi());
		m_wait_confirm = 1;
	}
}

void gprs_rlcmac_dl_tbf::schedule_next_frame()
{
	struct msgb *msg;

	if (m_llc.frame_length() != 0)
		return;

	/* dequeue next LLC frame, if any */
	msg = llc_dequeue(gprs_bssgp_pcu_current_bctx());
	if (!msg)
		return;

	LOGPTBFDL(this, LOGL_DEBUG, "Dequeue next LLC (len=%d)\n", msg->len);

	m_llc.put_frame(msg->data, msg->len);
	bts->llc_frame_sched();
	msgb_free(msg);
	m_last_dl_drained_fn = -1;
}

int gprs_rlcmac_dl_tbf::create_new_bsn(const uint32_t fn, GprsCodingScheme cs)
{
	uint8_t *data;
	gprs_rlc_data *rlc_data;
	const uint16_t bsn = m_window.v_s();
	gprs_rlc_data_block_info *rdbi;
	int num_chunks = 0;
	int write_offset = 0;
	Encoding::AppendResult ar;

	if (m_llc.frame_length() == 0)
		schedule_next_frame();

	OSMO_ASSERT(cs.isValid());

	/* length of usable data block (single data unit w/o header) */
	const uint8_t block_data_len = cs.maxDataBlockBytes();

	/* now we still have untransmitted LLC data, so we fill mac block */
	rlc_data = m_rlc.block(bsn);
	data = rlc_data->prepare(block_data_len);
	rlc_data->cs_last = cs;
	rlc_data->cs_current_trans = cs;

	/* Initialise the variable related to DL SPB */
	rlc_data->spb_status.block_status_dl = EGPRS_RESEG_DL_DEFAULT;
	rlc_data->cs_init = cs;

	rlc_data->len = block_data_len;

	rdbi = &(rlc_data->block_info);
	memset(rdbi, 0, sizeof(*rdbi));
	rdbi->data_len = block_data_len;

	rdbi->cv = 15; /* Final Block Indicator, set late, if true */
	rdbi->bsn = bsn; /* Block Sequence Number */
	rdbi->e = 1; /* Extension bit, maybe set later (1: no extension) */

	do {
		bool is_final;
		int payload_written = 0;

		if (m_llc.frame_length() == 0) {
			/* nothing to sent - delay the release of the TBF */

			int space = block_data_len - write_offset;
			/* A header will need to by added, so we just need
			 * space-1 octets */
			m_llc.put_dummy_frame(space - 1);

			/* The data just drained, store the current fn */
			if (m_last_dl_drained_fn < 0)
				m_last_dl_drained_fn = fn;

			/* It is not clear, when the next real data will
			 * arrive, so request a DL ack/nack now */
			request_dl_ack();

			LOGPTBFDL(this, LOGL_DEBUG,
				  "Empty chunk, added LLC dummy command of size %d, drained_since=%d\n",
				  m_llc.frame_length(), frames_since_last_drain(fn));
		}

		is_final = llc_queue_size() == 0 && !keep_open(fn);

		ar = Encoding::rlc_data_to_dl_append(rdbi, cs,
			&m_llc, &write_offset, &num_chunks, data, is_final, &payload_written);

		if (payload_written > 0)
			bts->rlc_dl_payload_bytes(payload_written);

		if (ar == Encoding::AR_NEED_MORE_BLOCKS)
			break;

		LOGPTBFDL(this, LOGL_DEBUG, "Complete DL frame, len=%d\n", m_llc.frame_length());
		gprs_rlcmac_dl_bw(this, m_llc.frame_length());
		bts->llc_dl_bytes(m_llc.frame_length());
		m_llc.reset();

		if (is_final) {
			request_dl_ack();
			TBF_SET_STATE(this, GPRS_RLCMAC_FINISHED);
		}

		/* dequeue next LLC frame, if any */
		schedule_next_frame();
	} while (ar == Encoding::AR_COMPLETED_SPACE_LEFT);

	LOGPTBFDL(this, LOGL_DEBUG, "data block (BSN %d, %s): %s\n",
		  bsn, rlc_data->cs_last.name(),
		  osmo_hexdump(rlc_data->block, block_data_len));
	/* raise send state and set ack state array */
	m_window.m_v_b.mark_unacked(bsn);
	m_window.increment_send();

	return bsn;
}

bool gprs_rlcmac_dl_tbf::handle_ack_nack()
{
	bool ack_recovered = false;

	state_flags |= (1 << GPRS_RLCMAC_FLAG_DL_ACK);
	if (check_n_clear(GPRS_RLCMAC_FLAG_TO_DL_ACK)) {
		ack_recovered = true;
	}

	/* reset N3105 */
	n_reset(N3105);
	t_stop(T3191, "ACK/NACK received");
	TBF_POLL_SCHED_UNSET(this);

	return ack_recovered;
}

struct msgb *gprs_rlcmac_dl_tbf::create_dl_acked_block(
				const uint32_t fn, const uint8_t ts,
				int index, int index2)
{
	uint8_t *msg_data;
	struct msgb *dl_msg;
	unsigned msg_len;
	bool need_poll;
	/* TODO: support MCS-7 - MCS-9, where data_block_idx can be 1 */
	uint8_t data_block_idx = 0;
	unsigned int rrbp;
	uint32_t new_poll_fn;
	int rc;
	bool is_final = false;
	gprs_rlc_data_info rlc;
	GprsCodingScheme cs;
	int bsns[ARRAY_SIZE(rlc.block_info)];
	unsigned num_bsns;
	bool need_padding = false;
	enum egprs_rlcmac_dl_spb spb = EGPRS_RLCMAC_DL_NO_RETX;
	unsigned int spb_status = get_egprs_dl_spb_status(index);

	enum egprs_puncturing_values punct[2] = {
		EGPRS_PS_INVALID, EGPRS_PS_INVALID
	};
	osmo_static_assert(ARRAY_SIZE(rlc.block_info) == 2,
			   rlc_block_info_size_is_two);

	/*
	 * TODO: This is an experimental work-around to put 2 BSN into
	 * MSC-7 to MCS-9 encoded messages. It just sends the same BSN
	 * twice in the block. The cs should be derived from the TBF's
	 * current CS such that both BSNs (that must be compatible) can
	 * be put into the data area, even if the resulting CS is higher than
	 * the current limit.
	 */
	cs = m_rlc.block(index)->cs_current_trans;
	GprsCodingScheme &cs_init = m_rlc.block(index)->cs_init;
	bsns[0] = index;
	num_bsns = 1;

	if (index2 >= 0) {
		bsns[num_bsns] = index2;
		num_bsns += 1;
	}

	update_coding_scheme_counter_dl(cs);
	/*
	 * if the intial mcs is 8 and retransmission mcs is either 6 or 3
	 * we have to include the padding of 6 octets in first segment
	 */
	if ((GprsCodingScheme::Scheme(cs_init) == GprsCodingScheme::MCS8) &&
		(GprsCodingScheme::Scheme(cs) == GprsCodingScheme::MCS6 ||
		GprsCodingScheme::Scheme(cs) == GprsCodingScheme::MCS3)) {
		if (spb_status == EGPRS_RESEG_DL_DEFAULT ||
			spb_status == EGPRS_RESEG_SECOND_SEG_SENT)
			need_padding  = true;
	} else if (num_bsns == 1) {
		/* TODO: remove the conditional when MCS-6 padding isn't
		 * failing to be decoded by MEs anymore */
		/* TODO: support of MCS-8 -> MCS-6 transition should be
		 * handled
		 * Refer commit be881c028fc4da00c4046ecd9296727975c206a3
		 * dated 2016-02-07 23:45:40 (UTC)
		 */
		if (cs != GprsCodingScheme(GprsCodingScheme::MCS8))
			cs.decToSingleBlock(&need_padding);
	}

	spb = get_egprs_dl_spb(index);

	LOGPTBFDL(this, LOGL_DEBUG, "need_padding %d spb_status %d spb %d (BSN1 %d BSN2 %d)\n",
		  need_padding, spb_status, spb, index, index2);

	gprs_rlc_data_info_init_dl(&rlc, cs, need_padding, spb);

	rlc.usf = 7; /* will be set at scheduler */
	rlc.pr = 0; /* FIXME: power reduction */
	rlc.tfi = m_tfi; /* TFI */

	/* return data block(s) as message */
	msg_len = cs.sizeDL();
	dl_msg = msgb_alloc(msg_len, "rlcmac_dl_data");
	if (!dl_msg)
		return NULL;

	msg_data = msgb_put(dl_msg, msg_len);

	OSMO_ASSERT(rlc.num_data_blocks <= ARRAY_SIZE(rlc.block_info));
	OSMO_ASSERT(rlc.num_data_blocks > 0);

	LOGPTBFDL(this, LOGL_DEBUG, "Copying %u RLC blocks, %u BSNs\n", rlc.num_data_blocks, num_bsns);

	/* Copy block(s) to RLC message: the num_data_blocks cannot be more than 2 - see assert above */
	for (data_block_idx = 0; data_block_idx < OSMO_MIN(rlc.num_data_blocks, 2);
		data_block_idx++)
	{
		int bsn;
		uint8_t *block_data;
		gprs_rlc_data_block_info *rdbi, *block_info;
		enum egprs_rlc_dl_reseg_bsn_state reseg_status;

		/* Check if there are more blocks than BSNs */
		if (data_block_idx < num_bsns)
			bsn = bsns[data_block_idx];
		else
			bsn = bsns[0];

		/* Get current puncturing scheme from block */

		m_rlc.block(bsn)->next_ps = gprs_get_punct_scheme(
			m_rlc.block(bsn)->next_ps,
			m_rlc.block(bsn)->cs_last, cs, spb);

		if (cs.isEgprs()) {
			OSMO_ASSERT(m_rlc.block(bsn)->next_ps >= EGPRS_PS_1);
			OSMO_ASSERT(m_rlc.block(bsn)->next_ps <= EGPRS_PS_3);
		}

		punct[data_block_idx] = m_rlc.block(bsn)->next_ps;

		rdbi = &rlc.block_info[data_block_idx];
		block_info = &m_rlc.block(bsn)->block_info;

		/*
		 * get data and header from current block
		 * function returns the reseg status
		 */
		reseg_status = egprs_dl_get_data(bsn, &block_data);
		m_rlc.block(bsn)->spb_status.block_status_dl = reseg_status;

		/*
		 * If it is first segment of the split block set the state of
		 * bsn to nacked. If it is the first segment dont update the
		 * next ps value of bsn. since next segment also needs same cps
		 */
		if (spb == EGPRS_RLCMAC_DL_FIRST_SEG)
			m_window.m_v_b.mark_nacked(bsn);
		else {
			/*
			 * TODO: Need to handle 2 same bsns
			 * in header type 1
			 */
			gprs_update_punct_scheme(&m_rlc.block(bsn)->next_ps,
						cs);
		}

		m_rlc.block(bsn)->cs_last = cs;
		rdbi->e   = block_info->e;
		rdbi->cv  = block_info->cv;
		rdbi->bsn = bsn;
		is_final = is_final || rdbi->cv == 0;

		LOGPTBFDL(this, LOGL_DEBUG, "Copying data unit %d (BSN %d)\n",
			  data_block_idx, bsn);

		Encoding::rlc_copy_from_aligned_buffer(&rlc, data_block_idx,
			msg_data, block_data);
	}

	/* Calculate CPS only for EGPRS case */
	if (cs.isEgprs())
		rlc.cps = gprs_rlc_mcs_cps(cs, punct[0], punct[1], need_padding);

	/* If the TBF has just started, relate frames_since_last_poll to the
	 * current fn */
	if (m_last_dl_poll_fn < 0)
		m_last_dl_poll_fn = fn;

	need_poll = state_flags & (1 << GPRS_RLCMAC_FLAG_TO_DL_ACK);

	/* poll after POLL_ACK_AFTER_FRAMES frames, or when final block is tx.
	 */
	if (m_tx_counter >= POLL_ACK_AFTER_FRAMES || m_dl_ack_requested ||
			need_poll) {
		if (m_dl_ack_requested) {
			LOGPTBFDL(this, LOGL_DEBUG,
				  "Scheduling Ack/Nack polling, because is was requested explicitly "
				  "(e.g. first final block sent).\n");
		} else if (need_poll) {
			LOGPTBFDL(this, LOGL_DEBUG,
				  "Scheduling Ack/Nack polling, because polling timed out.\n");
		} else {
			LOGPTBFDL(this, LOGL_DEBUG,
				  "Scheduling Ack/Nack polling, because %d blocks sent.\n",
				POLL_ACK_AFTER_FRAMES);
		}

		rc = check_polling(fn, ts, &new_poll_fn, &rrbp);
		if (rc >= 0) {
			set_polling(new_poll_fn, ts, GPRS_RLCMAC_POLL_DL_ACK);

			m_tx_counter = 0;
			/* start timer whenever we send the final block */
			if (is_final)
				T_START(this, T3191, bts_data()->t3191, 0, "final block (DL-TBF)", true);

			state_flags &= ~(1 << GPRS_RLCMAC_FLAG_TO_DL_ACK); /* clear poll timeout flag */

			/* Clear request flag */
			m_dl_ack_requested = false;

			/* set polling in header */
			rlc.rrbp = rrbp;
			rlc.es_p = 1; /* Polling */

			m_last_dl_poll_fn = poll_fn;

			LOGPTBFDL(this, LOGL_INFO,
				  "Scheduled Ack/Nack polling on FN=%d, TS=%d\n",
				  poll_fn, poll_ts);
		}
	}

	Encoding::rlc_write_dl_data_header(&rlc, msg_data);

	LOGPTBFDL(this, LOGL_DEBUG, "msg block (BSN %d, %s%s): %s\n",
		  index, cs.name(),
		  need_padding ? ", padded" : "",
		  msgb_hexdump(dl_msg));

	/* Increment TX-counter */
	m_tx_counter++;

	return dl_msg;
}

static uint16_t bitnum_to_bsn(int bitnum, uint16_t ssn)
{
	return ssn - 1 - bitnum;
}

int gprs_rlcmac_dl_tbf::analyse_errors(char *show_rbb, uint8_t ssn,
	ana_result *res)
{
	gprs_rlc_data *rlc_data;
	uint16_t lost = 0, received = 0, skipped = 0;
	char info[RLC_MAX_WS + 1];
	memset(info, '.', m_window.ws());
	info[m_window.ws()] = 0;
	uint16_t bsn = 0;
	unsigned received_bytes = 0, lost_bytes = 0;
	unsigned received_packets = 0, lost_packets = 0;
	unsigned num_blocks = strlen(show_rbb);

	unsigned distance = m_window.distance();

	num_blocks = num_blocks > distance
				? distance : num_blocks;

	/* SSN - 1 is in range V(A)..V(S)-1 */
	for (unsigned int bitpos = 0; bitpos < num_blocks; bitpos++) {
		bool is_received;
		int index = num_blocks - 1 - bitpos;

		is_received = (index >= 0 && show_rbb[index] == 'R');

		bsn = m_window.mod_sns(bitnum_to_bsn(bitpos, ssn));

		if (bsn == m_window.mod_sns(m_window.v_a() - 1)) {
			info[bitpos] = '$';
			break;
		}

		rlc_data = m_rlc.block(bsn);
		if (!rlc_data) {
			info[bitpos] = '0';
			continue;
		}

		/* Get general statistics */
		if (is_received && !m_window.m_v_b.is_acked(bsn)) {
			received_packets += 1;
			received_bytes += rlc_data->len;
		} else if (!is_received && !m_window.m_v_b.is_nacked(bsn)) {
			lost_packets += 1;
			lost_bytes += rlc_data->len;
		}

		/* Get statistics for current CS */

		if (rlc_data->cs_last != current_cs()) {
			/* This block has already been encoded with a different
			 * CS, so it doesn't help us to decide, whether the
			 * current CS is ok. Ignore it. */
			info[bitpos] = 'x';
			skipped += 1;
			continue;
		}

		if (is_received) {
			if (!m_window.m_v_b.is_acked(bsn)) {
				received += 1;
				info[bitpos] = 'R';
			} else {
				info[bitpos] = 'r';
			}
		} else {
			info[bitpos] = 'L';
			lost += 1;
		}
	}

	LOGPTBFDL(this, LOGL_DEBUG,
		  "DL analysis, range=%d:%d, lost=%d, recv=%d, skipped=%d, bsn=%d, info='%s'\n",
		  m_window.v_a(), m_window.v_s(), lost, received, skipped, bsn, info);

	res->received_packets = received_packets;
	res->lost_packets = lost_packets;
	res->received_bytes = received_bytes;
	res->lost_bytes = lost_bytes;

	if (lost + received <= 1)
		return -1;

	return lost * 100 / (lost + received);
}

gprs_rlc_dl_window *gprs_rlcmac_dl_tbf::window()
{
	return &m_window;
}

int gprs_rlcmac_dl_tbf::update_window(unsigned first_bsn,
	const struct bitvec *rbb)
{
	unsigned dist;
	uint16_t lost = 0, received = 0;
	char show_v_b[RLC_MAX_SNS + 1];
	char show_rbb[RLC_MAX_SNS + 1];
	int error_rate;
	struct ana_result ana_res;
	dist = m_window.distance();
	unsigned num_blocks = rbb->cur_bit > dist
				? dist : rbb->cur_bit;
	unsigned behind_last_bsn = m_window.mod_sns(first_bsn + num_blocks);

	Decoding::extract_rbb(rbb, show_rbb);
	/* show received array in debug */
	LOGPTBFDL(this, LOGL_DEBUG,
		  "ack:  (BSN=%d)\"%s\"(BSN=%d)  R=ACK I=NACK\n",
		  first_bsn, show_rbb, m_window.mod_sns(behind_last_bsn - 1));

	error_rate = analyse_errors(show_rbb, behind_last_bsn, &ana_res);

	if (bts_data()->cs_adj_enabled && ms())
		ms()->update_error_rate(this, error_rate);

	m_window.update(bts, rbb, first_bsn, &lost, &received);
	rate_ctr_add(&m_ctrs->ctr[TBF_CTR_RLC_NACKED], lost);

	/* report lost and received packets */
	gprs_rlcmac_received_lost(this, received, lost);

	/* Used to measure the leak rate */
	gprs_bssgp_update_bytes_received(ana_res.received_bytes,
		ana_res.received_packets + ana_res.lost_packets);

	/* raise V(A), if possible */
	m_window.raise(m_window.move_window());

	/* show receive state array in debug (V(A)..V(S)-1) */
	m_window.show_state(show_v_b);
	LOGPTBFDL(this, LOGL_DEBUG,
		  "V(B): (V(A)=%d)\"%s\"(V(S)-1=%d)  A=Acked N=Nacked U=Unacked X=Resend-Unacked I=Invalid\n",
		  m_window.v_a(), show_v_b, m_window.v_s_mod(-1));
	return 0;
}

int gprs_rlcmac_dl_tbf::update_window(const uint8_t ssn, const uint8_t *rbb)
{
	int16_t dist; /* must be signed */
	uint16_t lost = 0, received = 0;
	char show_rbb[65];
	char show_v_b[RLC_MAX_SNS + 1];
	int error_rate;
	struct ana_result ana_res;

	Decoding::extract_rbb(rbb, show_rbb);
	/* show received array in debug (bit 64..1) */
	LOGPTBFDL(this, LOGL_DEBUG,
		  "ack:  (BSN=%d)\"%s\"(BSN=%d)  R=ACK I=NACK\n",
		  m_window.mod_sns(ssn - 64), show_rbb, m_window.mod_sns(ssn - 1));

	/* apply received array to receive state (SSN-64..SSN-1) */
	/* calculate distance of ssn from V(S) */
	dist = m_window.mod_sns(m_window.v_s() - ssn);
	/* check if distance is less than distance V(A)..V(S) */
	if (dist >= m_window.distance()) {
		/* this might happpen, if the downlink assignment
		 * was not received by ms and the ack refers
		 * to previous TBF
		 * FIXME: we should implement polling for
		 * control ack!*/
		LOGPTBFDL(this, LOGL_NOTICE, "ack range is out of V(A)..V(S) range - Free TBF!\n");
		return 1; /* indicate to free TBF */
	}

	error_rate = analyse_errors(show_rbb, ssn, &ana_res);

	if (bts_data()->cs_adj_enabled && ms())
		ms()->update_error_rate(this, error_rate);

	m_window.update(bts, show_rbb, ssn,
			&lost, &received);
	rate_ctr_add(&m_ctrs->ctr[TBF_CTR_RLC_NACKED], lost);

	/* report lost and received packets */
	gprs_rlcmac_received_lost(this, received, lost);

	/* Used to measure the leak rate */
	gprs_bssgp_update_bytes_received(ana_res.received_bytes,
		ana_res.received_packets + ana_res.lost_packets);

	/* raise V(A), if possible */
	m_window.raise(m_window.move_window());

	/* show receive state array in debug (V(A)..V(S)-1) */
	m_window.show_state(show_v_b);
	LOGPTBFDL(this, LOGL_DEBUG,
		  "V(B): (V(A)=%d)\"%s\"(V(S)-1=%d)  A=Acked N=Nacked U=Unacked X=Resend-Unacked I=Invalid\n",
		  m_window.v_a(), show_v_b, m_window.v_s_mod(-1));

	if (state_is(GPRS_RLCMAC_FINISHED) && m_window.window_empty()) {
		LOGPTBFDL(this, LOGL_NOTICE,
			  "Received acknowledge of all blocks, but without final ack inidcation (don't worry)\n");
	}
	return 0;
}


int gprs_rlcmac_dl_tbf::maybe_start_new_window()
{
	release();

	/* check for LLC PDU in the LLC Queue */
	if (llc_queue_size() > 0)
		/* we have more data so we will re-use this tbf */
		establish_dl_tbf_on_pacch();

	return 0;
}

int gprs_rlcmac_dl_tbf::release()
{
	uint16_t received;

	/* range V(A)..V(S)-1 */
	received = m_window.count_unacked();

	/* report all outstanding packets as received */
	gprs_rlcmac_received_lost(this, received, 0);

	TBF_SET_STATE(this, GPRS_RLCMAC_WAIT_RELEASE);

	/* start T3193 */
	T_START(this, T3193, bts_data()->t3193_msec / 1000, (bts_data()->t3193_msec % 1000) * 1000,
		  "release (DL-TBF)", true);

	/* reset rlc states */
	m_tx_counter = 0;
	m_wait_confirm = 0;
	m_window.reset();

	TBF_ASS_TYPE_UNSET(this, GPRS_RLCMAC_FLAG_CCCH);

	return 0;
}

int gprs_rlcmac_dl_tbf::abort()
{
	uint16_t lost;

	if (state_is(GPRS_RLCMAC_FLOW)) {
		/* range V(A)..V(S)-1 */
		lost = m_window.count_unacked();

		/* report all outstanding packets as lost */
		gprs_rlcmac_received_lost(this, 0, lost);
		gprs_rlcmac_lost_rep(this);

		/* TODO: Reschedule all LLC frames starting with the one that is
		 * (partly) encoded in chunk 1 of block V(A). (optional) */
	}

	TBF_SET_STATE(this, GPRS_RLCMAC_RELEASING);

	/* reset rlc states */
	m_window.reset();

	TBF_ASS_TYPE_UNSET(this, GPRS_RLCMAC_FLAG_CCCH);

	return 0;
}

int gprs_rlcmac_dl_tbf::rcvd_dl_ack(bool final_ack, unsigned first_bsn,
	struct bitvec *rbb)
{
	int rc;
	LOGPTBFDL(this, LOGL_DEBUG, "downlink acknowledge\n");

	rc = update_window(first_bsn, rbb);

	if (final_ack) {
		LOGPTBFDL(this, LOGL_DEBUG, "Final ACK received.\n");
		rc = maybe_start_new_window();
	} else if (state_is(GPRS_RLCMAC_FINISHED) && m_window.window_empty()) {
		LOGPTBFDL(this, LOGL_NOTICE,
			  "Received acknowledge of all blocks, but without final ack indication (don't worry)\n");
	}

	return rc;
}

int gprs_rlcmac_dl_tbf::rcvd_dl_ack(bool final_ack, uint8_t ssn, uint8_t *rbb)
{
	LOGPTBFDL(this, LOGL_DEBUG, "downlink acknowledge\n");

	if (!final_ack)
		return update_window(ssn, rbb);

	LOGPTBFDL(this, LOGL_DEBUG, "Final ACK received.\n");
	return maybe_start_new_window();
}

bool gprs_rlcmac_dl_tbf::dl_window_stalled() const
{
	return m_window.window_stalled();
}

void gprs_rlcmac_dl_tbf::request_dl_ack()
{
	m_dl_ack_requested = true;
}

bool gprs_rlcmac_dl_tbf::need_control_ts() const
{
	if (poll_scheduled())
		return false;

	return state_flags & (1 << GPRS_RLCMAC_FLAG_TO_DL_ACK) ||
		m_tx_counter >= POLL_ACK_AFTER_FRAMES ||
		m_dl_ack_requested;
}

bool gprs_rlcmac_dl_tbf::have_data() const
{
	return m_llc.chunk_size() > 0 ||
		(llc_queue_size() > 0);
}

static inline int frames_since_last(int32_t last, unsigned fn)
{
	unsigned wrapped = (fn + GSM_MAX_FN - last) % GSM_MAX_FN;

	if (last < 0)
		return -1;

	if (wrapped < GSM_MAX_FN/2)
		return wrapped;

	return wrapped - GSM_MAX_FN;
}

int gprs_rlcmac_dl_tbf::frames_since_last_poll(unsigned fn) const
{
	return frames_since_last(m_last_dl_poll_fn, fn);
}

int gprs_rlcmac_dl_tbf::frames_since_last_drain(unsigned fn) const
{
	return frames_since_last(m_last_dl_drained_fn, fn);
}

bool gprs_rlcmac_dl_tbf::keep_open(unsigned fn) const
{
	int keep_time_frames;

	if (bts_data()->dl_tbf_idle_msec == 0)
		return false;

	keep_time_frames = msecs_to_frames(bts_data()->dl_tbf_idle_msec);
	return frames_since_last_drain(fn) <= keep_time_frames;
}

/*
 * This function returns the pointer to data which needs
 * to be copied. Also updates the status of the block related to
 * Split block handling in the RLC/MAC block.
 */
enum egprs_rlc_dl_reseg_bsn_state
	gprs_rlcmac_dl_tbf::egprs_dl_get_data(int bsn, uint8_t **block_data)
{
	gprs_rlc_data *rlc_data = m_rlc.block(bsn);
	egprs_rlc_dl_reseg_bsn_state *block_status_dl =
				&rlc_data->spb_status.block_status_dl;

	GprsCodingScheme &cs_current_trans = m_rlc.block(bsn)->cs_current_trans;
	GprsCodingScheme &cs_init = m_rlc.block(bsn)->cs_init;
	*block_data = &rlc_data->block[0];

	/*
	 * Table 10.3a.0.1 of 44.060
	 * MCS6,9: second segment starts at 74/2 = 37
	 * MCS5,7: second segment starts at 56/2 = 28
	 * MCS8: second segment starts at 31
	 * MCS4: second segment starts at 44/2 = 22
	 */
	if (cs_current_trans.headerTypeData() ==
			GprsCodingScheme::HEADER_EGPRS_DATA_TYPE_3) {
		if (*block_status_dl == EGPRS_RESEG_FIRST_SEG_SENT) {
			switch (GprsCodingScheme::Scheme(cs_init)) {
			case GprsCodingScheme::MCS6 :
			case GprsCodingScheme::MCS9 :
				*block_data = &rlc_data->block[37];
				break;
			case GprsCodingScheme::MCS7 :
			case GprsCodingScheme::MCS5 :
				*block_data = &rlc_data->block[28];
				break;
			case GprsCodingScheme::MCS8 :
				*block_data = &rlc_data->block[31];
				break;
			case GprsCodingScheme::MCS4 :
				*block_data = &rlc_data->block[22];
				break;
			default:
				LOGPTBFDL(this, LOGL_ERROR,
					  "FIXME: Software error: hit invalid condition. "
					  "headerType(%d) blockstatus(%d) cs(%s) PLEASE FIX!\n",
					  cs_current_trans.headerTypeData(),
					  *block_status_dl, cs_init.name());
				break;

			}
			return EGPRS_RESEG_SECOND_SEG_SENT;
		} else if ((cs_init.headerTypeData() ==
				GprsCodingScheme::HEADER_EGPRS_DATA_TYPE_1) ||
			(cs_init.headerTypeData() ==
				GprsCodingScheme::HEADER_EGPRS_DATA_TYPE_2)) {
			return EGPRS_RESEG_FIRST_SEG_SENT;
		} else if ((GprsCodingScheme::Scheme(cs_init) ==
					GprsCodingScheme::MCS4) &&
				(GprsCodingScheme::Scheme(cs_current_trans) ==
					GprsCodingScheme::MCS1)) {
			return EGPRS_RESEG_FIRST_SEG_SENT;
		}
	}
	return EGPRS_RESEG_DL_DEFAULT;
}

/*
 * This function returns the status of split block
 * for RLC/MAC block.
 */
unsigned int gprs_rlcmac_dl_tbf::get_egprs_dl_spb_status(const int bsn)
{
	const gprs_rlc_data *rlc_data = m_rlc.block(bsn);

	return rlc_data->spb_status.block_status_dl;
}

/*
 * This function returns the spb value to be sent OTA
 * for RLC/MAC block.
 */
enum egprs_rlcmac_dl_spb gprs_rlcmac_dl_tbf::get_egprs_dl_spb(const int bsn)
{
	struct gprs_rlc_data *rlc_data = m_rlc.block(bsn);
	egprs_rlc_dl_reseg_bsn_state block_status_dl =
				rlc_data->spb_status.block_status_dl;

	GprsCodingScheme &cs_current_trans = m_rlc.block(bsn)->cs_current_trans;
	GprsCodingScheme &cs_init = m_rlc.block(bsn)->cs_init;

	/* Table 10.4.8b.1 of 44.060 */
	if (cs_current_trans.headerTypeData() ==
			GprsCodingScheme::HEADER_EGPRS_DATA_TYPE_3) {
	/*
	 * if we are sending the second segment the spb should be 3
	 * other wise it should be 2
	 */
		if (block_status_dl == EGPRS_RESEG_FIRST_SEG_SENT) {

			/* statistics */
			bts->spb_downlink_second_segment();
			return EGPRS_RLCMAC_DL_SEC_SEG;
		} else if ((cs_init.headerTypeData() ==
				GprsCodingScheme::HEADER_EGPRS_DATA_TYPE_1) ||
			(cs_init.headerTypeData() ==
				GprsCodingScheme::HEADER_EGPRS_DATA_TYPE_2)) {
			bts->spb_downlink_first_segment();
			return EGPRS_RLCMAC_DL_FIRST_SEG;
		} else if ((GprsCodingScheme::Scheme(cs_init) ==
					GprsCodingScheme::MCS4) &&
				(GprsCodingScheme::Scheme(cs_current_trans) ==
					GprsCodingScheme::MCS1)) {
			bts->spb_downlink_first_segment();
			return EGPRS_RLCMAC_DL_FIRST_SEG;
		}
	}
	/* Non SPB cases 0 is reurned */
	return EGPRS_RLCMAC_DL_NO_RETX;
}

void gprs_rlcmac_dl_tbf::set_window_size()
{
	uint16_t ws = egprs_window_size(bts->bts_data(), dl_slots());
	LOGPTBFDL(this, LOGL_INFO, "setting EGPRS DL window size to %u, base(%u) slots(%u) ws_pdch(%u)\n",
		  ws, bts->bts_data()->ws_base, pcu_bitcount(dl_slots()), bts->bts_data()->ws_pdch);
	m_window.set_ws(ws);
}

void gprs_rlcmac_dl_tbf::update_coding_scheme_counter_dl(const GprsCodingScheme cs)
{
	uint8_t coding_scheme = 0;

	coding_scheme = GprsCodingScheme::Scheme(cs);
	if (cs.isGprs()) {
		switch (coding_scheme) {
		case GprsCodingScheme::CS1 :
			bts->gprs_dl_cs1();
			rate_ctr_inc(&m_dl_gprs_ctrs->ctr[TBF_CTR_GPRS_DL_CS1]);
			break;
		case GprsCodingScheme::CS2 :
			bts->gprs_dl_cs2();
			rate_ctr_inc(&m_dl_gprs_ctrs->ctr[TBF_CTR_GPRS_DL_CS2]);
			break;
		case GprsCodingScheme::CS3 :
			bts->gprs_dl_cs3();
			rate_ctr_inc(&m_dl_gprs_ctrs->ctr[TBF_CTR_GPRS_DL_CS3]);
			break;
		case GprsCodingScheme::CS4 :
			bts->gprs_dl_cs4();
			rate_ctr_inc(&m_dl_gprs_ctrs->ctr[TBF_CTR_GPRS_DL_CS4]);
			break;
		}
	} else {
		switch (coding_scheme) {
		case GprsCodingScheme::MCS1 :
			bts->egprs_dl_mcs1();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS1]);
			break;
		case GprsCodingScheme::MCS2 :
			bts->egprs_dl_mcs2();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS2]);
			break;
		case GprsCodingScheme::MCS3 :
			bts->egprs_dl_mcs3();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS3]);
			break;
		case GprsCodingScheme::MCS4 :
			bts->egprs_dl_mcs4();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS4]);
			break;
		case GprsCodingScheme::MCS5 :
			bts->egprs_dl_mcs5();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS5]);
			break;
		case GprsCodingScheme::MCS6 :
			bts->egprs_dl_mcs6();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS6]);
			break;
		case GprsCodingScheme::MCS7 :
			bts->egprs_dl_mcs7();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS7]);
			break;
		case GprsCodingScheme::MCS8 :
			bts->egprs_dl_mcs8();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS8]);
			break;
		case GprsCodingScheme::MCS9 :
			bts->egprs_dl_mcs9();
			rate_ctr_inc(&m_dl_egprs_ctrs->ctr[TBF_CTR_EGPRS_DL_MCS9]);
			break;
		}
	}
}
