/*
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

#pragma once

extern "C" {
	#include <osmocom/core/linuxlist.h>
}

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#define LLC_MAX_LEN 1543

struct BTS;

/**
 * I represent the LLC data to a MS
 */
struct gprs_llc {
	static bool is_user_data_frame(uint8_t *data, size_t len);

	void init();
	void reset();
	void reset_frame_space();

	void put_frame(const uint8_t *data, size_t len);
	void put_dummy_frame(size_t req_len);
	void append_frame(const uint8_t *data, size_t len);

	void consume(size_t len);
	void consume(uint8_t *data, size_t len);

	uint16_t chunk_size() const;
	uint16_t remaining_space() const;
	uint16_t frame_length() const;

	bool fits_in_current_frame(uint8_t size) const;

	uint8_t frame[LLC_MAX_LEN]; /* current DL or UL frame */
	uint16_t m_index; /* current write/read position of frame */
	uint16_t m_length; /* len of current DL LLC_frame, 0 == no frame */
};

/**
 * I store the LLC frames that come from the SGSN.
 */
struct gprs_llc_queue {
	struct MetaInfo {
		struct timeval recv_time;
		struct timeval expire_time;
	};

	static void calc_pdu_lifetime(BTS *bts, const uint16_t pdu_delay_csec,
		struct timeval *tv);
	static bool is_frame_expired(const struct timeval *now,
		const struct timeval *tv);
	static bool is_user_data_frame(uint8_t *data, size_t len);

	void init();

	void enqueue(struct msgb *llc_msg, const MetaInfo *info = 0);
	struct msgb *dequeue(const MetaInfo **info = 0);
	void clear(BTS *bts);
	void move_and_merge(gprs_llc_queue *o);
	size_t size() const;
	size_t octets() const;

private:
	uint32_t m_avg_queue_delay; /* Average delay of data going through the queue */
	size_t m_queue_size;
	size_t m_queue_octets;
	struct llist_head m_queue; /* queued LLC DL data */

};


inline uint16_t gprs_llc::chunk_size() const
{
	return m_length - m_index;
}

inline uint16_t gprs_llc::remaining_space() const
{
	return LLC_MAX_LEN - m_length;
}

inline uint16_t gprs_llc::frame_length() const
{
	return m_length;
}

inline void gprs_llc::consume(size_t len)
{
	m_index += len;
}

inline void gprs_llc::consume(uint8_t *data, size_t len)
{
	/* copy and increment index */
	memcpy(data, frame + m_index, len);
	consume(len);
}

inline bool gprs_llc::fits_in_current_frame(uint8_t chunk_size) const
{
	return m_length + chunk_size <= LLC_MAX_LEN;
}

inline size_t gprs_llc_queue::size() const
{
	return m_queue_size;
}

inline size_t gprs_llc_queue::octets() const
{
	return m_queue_octets;
}
