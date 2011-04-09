/* 
 * xNBD - an enhanced Network Block Device program
 *
 * Copyright (C) 2008-2011 National Institute of Advanced Industrial Science
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


#include "xnbd.h"



struct remote_read_request {
	/* block index */
	off_t bindex_iofrom;
	size_t bindex_iolen;
};

#define MAXNBLOCK 10

struct proxy_priv {
	int clientfd;



	uint32_t iotype;

	/* number of remote read requests */
	int nreq;
	struct remote_read_request req[MAXNBLOCK];

	off_t iofrom;
	size_t iolen;

	unsigned long block_index_start;
	unsigned long block_index_end;

	struct nbd_reply reply;

	char *write_buff;
	char *read_buff;


	GAsyncQueue *tx_queue;


	int need_exit;
};




struct xnbd_proxy {
	pthread_t tid_fwd_tx, tid_fwd_rx;


	/* queue between rx threads and forwarder_tx thread */ 
	GAsyncQueue *fwd_tx_queue;

	/* queue between forwarder_tx and forwarder_rx */
	GAsyncQueue *fwd_rx_queue;

	struct xnbd_info *xnbd;

	int remotefd;

	int cachefd;

	/* cached bitmap array (mmaped) */
	unsigned long *cbitmap;
	size_t cbitmaplen;
};

enum xnbd_proxy_cmd_type {
	XNBD_PROXY_CMD_UNKNOWN = 0,
	XNBD_PROXY_CMD_QUERY_STATUS,
	XNBD_PROXY_CMD_REGISTER_FD
};

/* query about current status via a unix socket */
struct xnbd_proxy_query {
	off_t disksize;
	char diskpath[PATH_MAX];
	char bmpath[PATH_MAX];
	pid_t master_pid;
} query;


void *forwarder_rx_thread_main(void *arg);
void *forwarder_tx_thread_main(void *arg);

extern struct proxy_priv priv_stop_forwarder;
void block_all_signals(void);