/*
 * xNBD - an enhanced Network Block Device program
 *
 * Copyright (C) 2008-2014 National Institute of Advanced Industrial Science
 * and Technology
 *
 * Author: Takahiro Hirofuchi <t.hirofuchi _at_ aist.go.jp>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307, USA.
 */


#include "xnbd_proxy.h"

void proxy_priv_dump(struct proxy_priv *priv)
{
	dbg("priv %p", priv);
	dbg(" iotype %s", nbd_get_iotype_string(priv->iotype));
	dbg(" seqnum %lu", priv->seqnum);
	dbg(" need_exit %d", priv->need_exit);
	dbg(" need_retry %d", priv->need_retry);
	dbg(" prepare_done %d", priv->prepare_done);

	dbg(" tx_queue %p", priv->tx_queue);
	dbg(" nreq   %d", priv->nreq);
	for (int i = 0; i < priv->nreq; i++) {
		dbg("  bindex_iofrom %ju", priv->req[i].bindex_iofrom);
		dbg("  bindex_iolen  %zu", priv->req[i].bindex_iolen);
	}

	dbg(" iofrom %ju", priv->iofrom);
	dbg(" iolen  %zu", priv->iolen);
	dbg(" block_index_start  %lu", priv->block_index_start);
	dbg(" block_index_end    %lu", priv->block_index_end);

	dbg(" reply.magic  %x", priv->reply.magic);
	dbg(" reply.error  %u", priv->reply.error);
	dbg(" reply.handle");
	//dump_buffer(priv->reply.handle, 8);
}



void add_read_block_to_tail(struct proxy_priv *priv, unsigned long i)
{
	int cur_nreq = priv->nreq;

	if (cur_nreq > 0) {
		struct remote_read_request *last_req = &priv->req[cur_nreq - 1];

		if (i == (last_req->bindex_iofrom + last_req->bindex_iolen)) {
			/* extend the iolen of the last request */
			last_req->bindex_iolen += 1;
			return;
		}
	}

	/* add a new request */
	priv->req[cur_nreq].bindex_iofrom = i;
	priv->req[cur_nreq].bindex_iolen  = 1;
	priv->nreq += 1;

	if (priv->nreq == MAXNBLOCK)
		err("bug, MAXNBLOCK is too small");
}



void prepare_read_priv(struct xnbd_proxy *proxy, struct proxy_priv *priv)
{
	unsigned long block_index_start = priv->block_index_start;
	unsigned long block_index_end   = priv->block_index_end;

	for (unsigned long i = block_index_start; i <= block_index_end; i++) {
		/* counter */
		cachestat_read_block();

		if (!bitmap_test(proxy->cbitmap, i)) {
			/* this block will be cached later in the completion thread */
			bitmap_on(proxy->cbitmap, i);

			/* counter */
			//monitor_cached_by_ondemand(i);
			cachestat_miss();
			cachestat_cache_odread();

			add_read_block_to_tail(priv, i);
		} else {

			/* counter */
			cachestat_hit();
		}

	}


}


void prepare_write_priv(struct xnbd_proxy *proxy, struct proxy_priv *priv)
{
	unsigned long block_index_start = priv->block_index_start;
	unsigned long block_index_end   = priv->block_index_end;
	off_t iofrom = priv->iofrom;
	size_t iolen  = priv->iolen;


	/*
	 * First, send read requests for start/end blocks to a source node
	 * if they are partial blocks and not yet cached.
	 **/
	int get_start_block = 0;
	int get_end_block   = 0;

	{
		if (iofrom % CBLOCKSIZE)
			if (!bitmap_test(proxy->cbitmap, block_index_start))
				get_start_block = 1;


		if ((iofrom + iolen) % CBLOCKSIZE) {
			/*
			 * Handle the end of the io range is not aligned.
			 * Case 1: The IO range covers more than one block.
			 * Case 2: One block, but the start of the io range is aligned.
			 */
			if ((block_index_end > block_index_start) ||
					((block_index_end == block_index_start) && !get_start_block))
				if (!bitmap_test(proxy->cbitmap, block_index_end))
					get_end_block = 1;

			/* bitmap_on() is performed in the below forloop */
		}


		/*
		 * Mark all write data blocks as cached. The following I/O
		 * requests to this area never retrieve these blocks from remote.
		 **/
		for (unsigned long i = block_index_start; i <= block_index_end; i++) {
			/* counter */
			cachestat_write_block();

			if (!bitmap_test(proxy->cbitmap, i)) {
				bitmap_on(proxy->cbitmap, i);

				/* counter */
				//monitor_cached_by_ondemand(i);
				cachestat_cache_odwrite();
			}
		}
	}

	if (get_start_block) {
		g_assert(priv->nreq + 1 <= MAXNBLOCK);

		int cur_nreq = priv->nreq;
		priv->req[cur_nreq].bindex_iofrom = block_index_start;
		priv->req[cur_nreq].bindex_iolen  = 1;
		priv->nreq += 1;

		cachestat_miss();
	} else {
		cachestat_hit();
	}

	if (get_end_block) {
		g_assert(priv->nreq + 1 <= MAXNBLOCK);

		int cur_nreq = priv->nreq;
		priv->req[cur_nreq].bindex_iofrom = block_index_end;
		priv->req[cur_nreq].bindex_iolen  = 1;
		priv->nreq += 1;

		cachestat_miss();
	} else {
		cachestat_hit();
	}


	/*
	 * For a WRITE request, we received all write data from the client.
	 * But, a reply must be sent later in the completion thread.
	 * send(clientfd) may be blocked while holding send_lock.
	 *
	 * The completion threads holds send_lock, and unfortunately sometimes
	 * becomes blocked due to a TCP flow control: A client is still
	 * submitting the following requests, and not receiving replies from a
	 * server.
	 * The main thread is blocked for send_lock, and cannot receive the
	 * following requests anymore.
	 *
	 * Also, the reordering of request results should be avoided?
	 *
	 * If the completion thread is sending READ reply data, and at the same
	 * time the main thread is sending a reply, a deadlock occurs.
	 *
	 * Therefore, sending reply should be done in the completion thread.
	 *
	 * UPDATE: the main thread does not perform send() to the client
	 * anymore; send(clientfd) is only performed at the completion thread.
	 * So, we remove send_lock for clientfd.
	 **/
}


static unsigned long fwd_counter = 0;

void *forwarder_tx_thread_main(void *arg)
{
	struct xnbd_proxy *proxy = (struct xnbd_proxy *) arg;
	int sending_failed = 0;

	set_process_name("proxy_fwd_tx");

	block_all_signals();

	info("create forwarder_tx thread %lu", pthread_self());


	for (;;) {
		struct proxy_priv *priv;

		priv = (struct proxy_priv *) g_async_queue_pop(proxy->fwd_tx_queue);
		dbg("%lu --- process new queue element", pthread_self());

		if (priv == &priv_stop_forwarder) {
			g_async_queue_push(proxy->fwd_rx_queue, (gpointer) priv);
			goto out_of_loop;
		}

		if (priv->need_exit) {
			g_async_queue_push(proxy->fwd_rx_queue, (gpointer) priv);
			continue;
		}



		if (!priv->prepare_done) {
			if (priv->iotype == NBD_CMD_WRITE)
				prepare_write_priv(proxy, priv);
			else if (priv->iotype == NBD_CMD_READ || priv->iotype == NBD_CMD_CACHE)
				prepare_read_priv(proxy, priv);

			priv->seqnum = fwd_counter;
			fwd_counter += 1;

			/* in retry, skip setting up forward requests */
			priv->prepare_done = 1;
		}


		/* send read request as soon as possible */
		for (int i = 0; i < priv->nreq; i++) {
			off_t iofrom = (off_t) priv->req[i].bindex_iofrom * CBLOCKSIZE;
			size_t length = priv->req[i].bindex_iolen * CBLOCKSIZE;

			length = confine_iolen_within_disk(proxy->xnbd->disksize, iofrom, length);

			int ret = nbd_client_send_read_request(proxy->remotefd, iofrom, length);
			if (ret < 0) {
				warn("sending read request failed, seqnum %lu", priv->seqnum);
				sending_failed = 1;
				break;
			}
		}

		/* Once sending failed, this marking works if nreq == 0. All
		 * the following requests are enqueued to the retry queue. */
		if (sending_failed)
			priv->need_retry = 1;

		g_async_queue_push(proxy->fwd_rx_queue, (gpointer) priv);
	}

out_of_loop:


	info("bye forwarder_tx thread");;
	return NULL;
}

static int receiving_failed = 0;
int forwarder_rx_thread_mainloop(struct xnbd_proxy *proxy)
{
	struct xnbd_info *xnbd = proxy->xnbd;

	struct proxy_priv *priv;
	int ret;

	dbg("wait new queue element");


	priv = (struct proxy_priv *) g_async_queue_pop(proxy->fwd_rx_queue);
	dbg("--- process new queue element %p", priv);

	proxy_priv_dump(priv);

	if (priv == &priv_stop_forwarder)
		return -1;

	if (priv->need_exit)
		goto hand_to_tx_queue;


	struct mmap_block_region *mbr = mmap_block_region_create(proxy->cachefd, proxy->xnbd->disksize, priv->iofrom, priv->iolen, 0);
	char *iobuf = mbr->iobuf;

	for (int i = 0; i < priv->nreq; i++) {
		dbg("priv req %d", i);
		off_t block_iofrom = priv->req[i].bindex_iofrom * CBLOCKSIZE;
		size_t block_iolen = priv->req[i].bindex_iolen  * CBLOCKSIZE;
		block_iolen = confine_iolen_within_disk(xnbd->disksize, block_iofrom, block_iolen);

		char *iobuf_partial = (char *) mbr->ba_iobuf + (block_iofrom - mbr->ba_iofrom);

		/* recv from server */
		ret = nbd_client_recv_read_reply(proxy->remotefd, iobuf_partial, block_iolen);
		if (ret < 0) {
			warn("forwarder: receiving a read reply failed, seqnum %lu", priv->seqnum);
			receiving_failed = 1;
			break;
		}

		/*
		 * Do not mark cbitmap here. Do it before. Otherwise, when the
		 * following request covers an over-wrapped I/O region, the
		 * main thread may retrieve remote blocks and overwrite them to
		 * an updated region.
		 **/
	}

	if (receiving_failed)
		priv->need_retry = 1;


	if (!priv->need_retry) {
		if (priv->iotype == NBD_CMD_READ) {
			/* we have to serialize all io to the cache disk. */
			memcpy(priv->read_buff, iobuf, priv->iolen);

		} else if (priv->iotype == NBD_CMD_WRITE) {
			/*
			 * This memcpy() must come before sending reply, so that xnbd-tester
			 * avoids memcmp() mismatch.
			 **/
			memcpy(iobuf, priv->write_buff, priv->iolen);

			/* Do not mark cbitmap here. */

		} else if (priv->iotype == NBD_CMD_CACHE) {
			/* NBD_CMD_CACHE does not do nothing here */
			;

		} else if (priv->iotype == NBD_CMD_FLUSH) {
			dbg("disk flush");
			/* FLUSH ensure that the data of all the blocks is
			 * written out to the physical storage. If all the
			 * blocks are already cached, the FLUSH command works
			 * as intended.
			 *
			 * If some blocks are not yet cached, FLUSH cannot
			 * ensure that the data of all the blocks is written
			 * out to the physical storage of the proxy server. It
			 * only ensures that the data of already-cached blocks
			 * is written out.
			 *
			 * We can consider this behavior is okay because the
			 * data of not-yet-cached blocks exit in the remote
			 * server. Even if the proxy sever crashes before all
			 * the blocks are cached, we will be able to recover
			 * the disk data from the local and remote storage, in
			 * theory.
			 **/
			ret = fsync(proxy->cachefd);
			if (ret < 0)
				err("fsync %m");

			bitmap_sync_file(proxy->cbitmap, proxy->cbitmaplen);

		} else if (priv->iotype == NBD_CMD_TRIM) {
			/* If some blocks in the range are not yet cached, we
			 * can mark them as cached. */
			punch_hole(proxy->cachefd, priv->iofrom, priv->iolen);

		} else
			err("bug");
	}

	mmap_block_region_free(mbr);


	if (priv->need_retry) {
		g_async_queue_push(proxy->fwd_retry_queue, priv);
		return 0;
	}

hand_to_tx_queue:
	/* do not touch priv after enqueue */
	dbg("seqnum %lu", priv->seqnum);
	g_async_queue_push(priv->tx_queue, priv);

	dbg("send reply to client done");

	return 0;
}


void *forwarder_rx_thread_main(void *arg)
{
	struct xnbd_proxy *proxy = (struct xnbd_proxy *) arg;
	set_process_name("proxy_fwd_rx");

	receiving_failed = 0;

	block_all_signals();

	info("create forwarder_rx thread %lu", pthread_self());

	for (;;) {
		int ret = forwarder_rx_thread_mainloop(proxy);
		if (ret < 0)
			break;
	}


	info("bye forwarder_rx thread");

	return NULL;
}

