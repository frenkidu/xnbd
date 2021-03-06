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

#ifndef LIB_XNBD_NBD_H
#define LIB_XNBD_NBD_H

#include "common.h"
#include "net.h"
#include "io.h"
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>


/* Be compatible with the NBD protocol. */

#define NBD_REQUEST_MAGIC 0x25609513
#define NBD_REPLY_MAGIC   0x67446698

struct nbd_request {
	uint32_t magic;
	uint32_t type;
	uint64_t handle;
	uint64_t from;
	uint32_t len;
} __attribute__((__packed__));

struct nbd_reply {
	uint32_t magic;
	uint32_t error;
	uint64_t handle;
};

enum {
	NBD_CMD_READ = 0,
	NBD_CMD_WRITE = 1,
	NBD_CMD_DISC = 2,
	NBD_CMD_FLUSH = 3,
	NBD_CMD_TRIM = 4,

	NBD_CMD_CACHE = 5,

	NBD_CMD_READ_COMPRESS = 6,
	NBD_CMD_READ_COMPRESS_LZO = 7,

	NBD_CMD_UNDEFINED = 8
};

const char *nbd_get_iotype_string(uint32_t iotype);


int nbd_negotiate_v1_server_side(int sockfd, off_t exportsize);
int nbd_negotiate_v1_server_side_readonly(int sockfd, off_t exportsize);
int nbd_negotiate_v1_client_side(int sockfd, off_t *exportsize, uint32_t *exportflags);

char *nbd_negotiate_v2_server_phase0(int sockfd);
int   nbd_negotiate_v2_server_phase1(int sockfd, off_t exportsize, int readonly);
int   nbd_negotiate_v2_client_side(int sockfd, off_t *exportsize, uint32_t *exportflags, size_t namesize, const char *target_name);

#define NBD_FLAG_HAS_FLAGS      (1 << 0)
#define NBD_FLAG_READ_ONLY      (1 << 1)
#define NBD_FLAG_SEND_FLUSH     (1 << 2)
/* skip _SEND_FUA and _ROTATIONAL */
#define NBD_FLAG_SEND_TRIM      (1 << 5)





/* for debug */
void nbd_request_dump(struct nbd_request *request);
void nbd_reply_dump(struct nbd_reply *reply);


#define NBD_SERVER_RECV__BAD_REQUEST     (-1)
#define NBD_SERVER_RECV__MAGIC_MISMATCH  (-2)
#define NBD_SERVER_RECV__TERMINATE       (-3)

int  nbd_server_recv_request(int clientfd, off_t disksize, uint32_t *iotype_arg, off_t *iofrom_arg,
		size_t *iolen_arg, struct nbd_reply *reply);

int nbd_client_send_request_header(int remotefd, uint32_t iotype, off_t iofrom, size_t len, uint64_t handle);
int nbd_client_recv_reply_header(int remotefd, uint64_t handle);

int nbd_client_recv_read_reply_iov(int remotefd, struct iovec *iov, unsigned int count, uint64_t handle);

int nbd_client_send_read_request(int remotefd, off_t iofrom, size_t len);
int nbd_client_recv_read_reply(int remotefd, char *buf, size_t len);

void nbd_client_send_disc_request(int remotefd);

#endif
