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

#include "nbd.h"


const char *nbd_get_iotype_string(uint32_t iotype)
{
	const char *nbd_iotype_string_table[] = {
		"NBD_CMD_READ",
		"NBD_CMD_WRITE",
		"NBD_CMD_DISC",
		"NBD_CMD_FLUSH",
		"NBD_CMD_TRIM",
		"NBD_CMD_CACHE",
		"NBD_CMD_READ_COMPRESS",
		"NBD_CMD_READ_COMPRESS_LZO",
		"NBD_CMD_UNDEFINED"
	};

	/* UNDEFINED is one of the known commands. The others are unknown. */
	if (iotype >= sizeof(nbd_iotype_string_table) / sizeof(nbd_iotype_string_table[0]))
		return "NBD_CMD_(unknown)";

	return nbd_iotype_string_table[iotype];
}


void nbd_request_dump(struct nbd_request *request)
{
	info("nbd_request %p", request);
	info(" request.magic  %x %x", request->magic, ntohl(request->magic));
	info(" request.type  %u %u", request->type, ntohl(request->type));
	info(" request.from  %ju %ju", request->from, ntohll(request->from));
	info(" request.len  %u %u", request->len, ntohl(request->len));
	info(" request.handle %ju %ju", request->handle, ntohll(request->handle));
}

void nbd_reply_dump(struct nbd_reply *reply)
{
	info("nbd_reply %p", reply);
	info(" reply.magic  %x %x", reply->magic, ntohl(reply->magic));
	info(" reply.error  %u %u", reply->error, ntohl(reply->magic));
	info(" reply.handle %ju %ju", reply->handle, ntohll(reply->handle));
}


int nbd_client_send_request_header(int remotefd, uint32_t iotype, off_t iofrom, size_t len, uint64_t handle)
{
	g_assert(len <= UINT32_MAX);
	g_assert(iofrom + len <= OFF_MAX);
	g_assert(iofrom >= 0);

	dbg("send_request_header iofrom %ju len %zu", iofrom, len);


	struct nbd_request request;
	memset(&request, 0, sizeof(request));

	request.magic = htonl(NBD_REQUEST_MAGIC);
	request.type = htonl(iotype);
	request.from = htonll(iofrom);
	request.len = htonl(len);
	request.handle = htonll(handle);

	ssize_t ret = net_send_all(remotefd, &request, sizeof(request));
	if (ret < (ssize_t) sizeof(request)) {
		warn("sending a nbd client header failed");
		return -1;
	}

	return 0;
}


int nbd_client_recv_reply_header(int remotefd, uint64_t handle)
{
	struct nbd_reply reply;

	dbg("now receiving read reply");

	memset(&reply, 0, sizeof(reply));

	int ret = net_recv_all_or_error(remotefd, &reply, sizeof(reply));
	if (ret < 0) {
		warn("recv header");
		return -EPIPE;
	}

	//nbd_reply_dump(&reply);

	if (ntohl(reply.magic) != NBD_REPLY_MAGIC) {
		warn("unknown reply magic, %x %x", reply.magic, ntohl(reply.magic));
		return -EPIPE;
	}

	/* check reply handle here */
	if (reply.handle != ntohll(handle)) {
		warn("unknown reply handle, %ju %ju", reply.handle, ntohll(handle));
		return -EPIPE;
	}

	uint32_t error = ntohl(reply.error);
	if (error) {
		warn("error in remote internal, reply state %d", error);
		return -error;
	}

	return 0;
}

int nbd_client_recv_read_reply_iov(int remotefd, struct iovec *iov, unsigned int count, uint64_t handle)
{
	int ret;

	ret = nbd_client_recv_reply_header(remotefd, handle);
	if (ret < 0) {
		warn("recv header");
		return -EPIPE;
	}

	dbg("recv data iov %p %u\n", iov, count);
	ret = net_readv_all_or_error(remotefd, iov, count);
	if (ret < 0) {
		warn("recv data");
		return -EPIPE;
	}

	return 0;
}


static const uint64_t myhandle = UINT64_MAX;

int nbd_client_send_read_request(int remotefd, off_t iofrom, size_t len)
{
	dbg("sending request of iotype %s iofrom %ju len %zu",
			"read", iofrom, len);

	return nbd_client_send_request_header(remotefd, NBD_CMD_READ, iofrom, len, myhandle);
}

int nbd_client_recv_read_reply(int remotefd, char *buf, size_t len)
{
	dbg("now receiving read reply");

	g_assert(buf);
	g_assert(len <= UINT32_MAX);

	struct iovec iov[1];
	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	return nbd_client_recv_read_reply_iov(remotefd, iov, 1, myhandle);
}



void nbd_client_send_disc_request(int remotefd)
{
	struct nbd_request request;
	int ret;

	memset(&request, 0, sizeof(request));

	request.magic = htonl(NBD_REQUEST_MAGIC);
	request.type = htonl(NBD_CMD_DISC);

	ret = net_send_all(remotefd, &request, sizeof(request));
	if (ret < (ssize_t) sizeof(request))
		warn("sending NBD_DISC failed");
}








/**
 * Returning 0:  request is good.
 * Returning NBD_SERVER_RECV__BAD_REQUEST: bad request.
 * 		An error is notified the client by using reply.errcode.
 * Returning NBD_SERVER_RECV__MAGIC_MISMATCH: protocol violation.
 * 	 	The connection is going to be discarded.
 * Returning NBD_SERVER_RECV__TERMINATE: terminate request.
 */
int nbd_server_recv_request(int clientfd, off_t disksize, uint32_t *iotype_arg, off_t *iofrom_arg,
		size_t *iolen_arg, struct nbd_reply *reply)
{
	struct nbd_request request;
	uint32_t magic  = 0;
	uint32_t iotype = 0;
	uint64_t iofrom = 0;
	uint32_t iolen  = 0;
	int ret;

	memset(&request, 0, sizeof(request));

	ret = net_recv_all_or_error(clientfd, &request, sizeof(request));
	if (ret < 0) {
		warn("recv_request: peer closed or error");
		return NBD_SERVER_RECV__TERMINATE;
	}

	magic  = ntohl(request.magic);
	iotype = ntohl(request.type);
	iofrom = ntohll(request.from);
	iolen  = ntohl(request.len);

	/* protocol violation */
	if (magic != NBD_REQUEST_MAGIC) {
		warn("recv_request: magic mismatch, %u %u", magic, NBD_REQUEST_MAGIC);
		nbd_request_dump(&request);
		dump_buffer((char *) &request, sizeof(request));
		return NBD_SERVER_RECV__MAGIC_MISMATCH;
	}

	dbg("%s from %ju (%ju) len %u, ", nbd_get_iotype_string(iotype), iofrom, iofrom / 512U, iolen);

	if (iotype == NBD_CMD_DISC) {
		info("recv_request: disconnect request");
		return NBD_SERVER_RECV__TERMINATE;
	}


	/* do not touch the handle value at the server side */
	reply->handle = request.handle;


	/*
	 * nbd-server.c defines the maximum disk size as OFFT_MAX =
	 * ~((off_t)1<<(sizeof(off_t)*8-1)), so it's ok that our disksize is
	 * defined with off_t.
	 **/

	/* bad request */
	if (iotype == NBD_CMD_FLUSH) {
		if (!(iofrom == 0 && iolen == 0)) {
			warn("recv_request: length and offset must be zero for NBD_CMD_FLUSH");
			reply->error = htonl(EINVAL);
			return NBD_SERVER_RECV__BAD_REQUEST;
		}
	}

	if ((iofrom + iolen) > (uint64_t) disksize) {
		warn("error offset exceeds the end of disk, offset %ju (iofrom %ju + iolen %u) disksize %jd",
				(iofrom + iolen), iofrom, iolen, disksize);
		reply->error = htonl(EINVAL);
		return NBD_SERVER_RECV__BAD_REQUEST;
	}


	*iotype_arg = iotype;
	*iofrom_arg = iofrom;  /* disksize is off_t, so checked already */
	*iolen_arg  = iolen;

	/* io realtime monitor */
	//monitor_access(iofrom, iolen, iotype);

	return 0;
}






/* 8 chars */
// const char nbd_password[8] = {'N', 'B', 'D', 'M', 'A', 'G', 'I', 'C'};
const uint64_t NBD_PASSWD = 0x4e42444d41474943LL;

const uint64_t NBD_NEGOTIATE_VERSION1_MAGIC = 0x0000420281861253LL;
const uint64_t NBD_NEGOTIATE_VERSION2_MAGIC = 0x49484156454F5054LL;
const uint32_t NBD_NEGOTIATE_VERSION2_OPT_EXPORT_NAME = 1;
#define XNBD_EXPORT_NAME_MAXLEN (256)


/*
 * For the new negotination protocol.
 * We name it VERSION 2.
 **/

struct nbd_negotiate_v2_pdu_type0 {
	uint64_t passwd;
	uint64_t magic;
	uint16_t hs_flags16; // handshake flags
} __attribute__((__packed__));

struct nbd_negotiate_v2_pdu_type1 {
	uint32_t reserved;
	uint64_t opt_magic;
	uint32_t opt;
	uint32_t namesize;
} __attribute__((__packed__));

struct nbd_negotiate_v2_pdu_type2 {
	uint64_t size;
	uint16_t tm_flags16;  // trasmission flags
	char padding[124];
} __attribute__((__packed__));


/*
 * The option NBD_NEGOTIATE_VERSION2_OPT_EXPORT_NAME is introduced in the recent version of the
 * original NBD. It allows a client to specify a target image name.
 *
 * I feel the negotiation phase of the NBD protocol is not so smart; it
 * should be changed to be reasonable. But, for the compatibility with the
 * original NBD and qemu, here the option is implemented.
 *
 * From the viewpoint of the client, the new protocol is summarized as follows:
 *
 *    recv pdu_new_0
 *    send pdu_new_1
 *    send target_name (any size ok!?)
 *    recv pdu_new_2
 *
 */


/*
 * get a target name from a client.
 * Note: must free a returned buffer.
 **/
char *nbd_negotiate_v2_server_phase0(int sockfd)
{
	{
		struct nbd_negotiate_v2_pdu_type0 pdu0;

		pdu0.passwd = htonll(NBD_PASSWD);
		pdu0.magic  = htonll(NBD_NEGOTIATE_VERSION2_MAGIC);
		pdu0.hs_flags16 = 0;

		int ret = net_send_all_or_error(sockfd, &pdu0, sizeof(pdu0));
		if (ret < 0)
			return NULL;
	}


	{
		struct nbd_negotiate_v2_pdu_type1 pdu1;

		int ret = net_recv_all_or_error(sockfd, &pdu1, sizeof(pdu1));
		if (ntohll(pdu1.opt_magic) != NBD_NEGOTIATE_VERSION2_MAGIC ||
				ntohl(pdu1.opt) != NBD_NEGOTIATE_VERSION2_OPT_EXPORT_NAME) {
			warn("header mismatch");
			return NULL;
		}

		uint32_t namesize = ntohl(pdu1.namesize);
		if (namesize > XNBD_EXPORT_NAME_MAXLEN) {
			warn("namesize error");
			return NULL;
		}

		char *target_name = g_malloc0(namesize + 1);

		ret = net_recv_all_or_error(sockfd, target_name, namesize);
		if (ret < 0)
			return NULL;

		info("requested target_name %s", target_name);

		return target_name;
	}
}


/* return the size and readonly of the target image */
int nbd_negotiate_v2_server_phase1(int sockfd, off_t exportsize, int readonly)
{
	g_assert(exportsize >= 0);

	struct nbd_negotiate_v2_pdu_type2 pdu2;
	/* clear the padding field with zero */
	memset(&pdu2, 0, sizeof(pdu2));

	uint16_t tm_flags16 = NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH;
	if (readonly) {
		info("nbd_negotiate: readonly");
		tm_flags16 |= NBD_FLAG_READ_ONLY;
	}

	pdu2.size       = htonll(exportsize);
	pdu2.tm_flags16 = htons(tm_flags16);

	int ret = net_send_all_or_error(sockfd, &pdu2, sizeof(pdu2));
	if (ret < 0) {
		warn("negotiation failed");
		return -1;
	}

	dbg("negotiate done");

	return 0;
}


int nbd_negotiate_v2_client_side(int sockfd, off_t *exportsize, uint32_t *exportflags, size_t namesize, const char *target_name)
{
	int ret;

	{
		struct nbd_negotiate_v2_pdu_type0 pdu0;

		ret = net_recv_all_or_error(sockfd, &pdu0, sizeof(pdu0));
		if (ret < 0)
			goto err_out;

		if (ntohll(pdu0.passwd) != NBD_PASSWD) {
			warn("password mismatch");
			goto err_out;
		}

		if (ntohll(pdu0.magic) == NBD_NEGOTIATE_VERSION1_MAGIC) {
			warn("A wrapped server was expected, but a plain server is found");
			goto err_out;
		}

		if (ntohll(pdu0.magic) != NBD_NEGOTIATE_VERSION2_MAGIC) {
			warn("negotiate magic does not match VERSION 1");
			goto err_out;
		}
	}


	{
		struct nbd_negotiate_v2_pdu_type1 pdu1;
		pdu1.reserved  = 0;
		pdu1.opt_magic = htonll(NBD_NEGOTIATE_VERSION2_MAGIC);
		pdu1.opt       = htonl(NBD_NEGOTIATE_VERSION2_OPT_EXPORT_NAME);
		pdu1.namesize  = htonl(namesize);

		ret = net_send_all_or_error(sockfd, &pdu1, sizeof(pdu1));
		if (ret < 0)
			goto err_out;

		ret = net_send_all_or_error(sockfd, target_name, namesize);
		if (ret < 0)
			goto err_out;
	}


	{
		struct nbd_negotiate_v2_pdu_type2 pdu2;

		ret = net_recv_all_or_error(sockfd, &pdu2, sizeof(pdu2));
		if (ret < 0)
			goto err_out;

		uint64_t size       = ntohll(pdu2.size);
		uint16_t tm_flags16 = ntohs(pdu2.tm_flags16);

		info("remote size: %ju bytes (%ju MBytes)", size, size /1024 /1024);


		if (size > OFF_MAX) {
			warn("remote size exceeds a local off_t(%zd bytes) value", sizeof(off_t));
			return -1;
		}

		if (exportsize)
			*exportsize  = (off_t) size;
		if (exportflags)
			*exportflags = tm_flags16;
	}


	return 0;

err_out:
	return -1;
}



/*
 * For the old negotination protocol.
 * We name it VERSION 1.
 **/

struct nbd_negotiate_v1_pdu {
	uint64_t passwd;
	uint64_t magic;
	uint64_t size;
	uint32_t flags;
	char padding[124];
} __attribute__((__packed__));


static int nbd_negotiate_v1_server_side_main(int sockfd, off_t exportsize, int readonly)
{
	g_assert(exportsize >= 0);

	struct nbd_negotiate_v1_pdu pdu;
	memset(&pdu, 0, sizeof(pdu));

	uint32_t flags = NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH;
	if (readonly) {
		info("nbd_negotiate: readonly");
		flags |= NBD_FLAG_READ_ONLY;
	}

	pdu.passwd = htonll(NBD_PASSWD);
	pdu.magic  = htonll(NBD_NEGOTIATE_VERSION1_MAGIC);
	pdu.size   = htonll(exportsize);
	pdu.flags  = htonl(flags);

	int ret = net_send_all_or_error(sockfd, &pdu, sizeof(pdu));
	if (ret < 0)
		goto err_out;

	dbg("negotiate done");

	return 0;

err_out:
	warn("negotiation failed");
	return -1;
}

int nbd_negotiate_v1_server_side_readonly(int sockfd, off_t exportsize)
{
	return nbd_negotiate_v1_server_side_main(sockfd, exportsize, 1);
}

int nbd_negotiate_v1_server_side(int sockfd, off_t exportsize)
{
	return nbd_negotiate_v1_server_side_main(sockfd, exportsize, 0);
}

int nbd_negotiate_v1_client_side(int sockfd, off_t *exportsize, uint32_t *exportflags)
{
	struct nbd_negotiate_v1_pdu pdu;

	/* Since both <nbd_negotiate_pdu_old> and <nbd_negotiate_pdu_new_0>
	 * share these first 128 bits
	 *
	 * struct .... {
	 *   uint64_t passwd;
	 *   uint64_t magic;
	 * } __attribute__((__packed__));
	 *
	 * we first read 128 bits only.  From the magic value, we know if
	 * we're daeling with a wrapper (NBD_NEGOTIATE_VERSION2_MAGIC) or a
	 * plain server (NBD_NEGOTIATE_VERSION1_MAGIC).  That way we can produce
	 * a more helpful error and save waiting for more bytes without hope.
	 */
	const size_t passwd_plus_magic_len = sizeof(pdu.passwd) + sizeof(pdu.magic);
	int ret = net_recv_all_or_error(sockfd, &pdu, passwd_plus_magic_len);
	if (ret < 0) {
		warn("receiving negotiate header failed");
		return -1;
	}

	if (ntohll(pdu.passwd) != NBD_PASSWD) {
		warn("password mismatch");
		return -1;
	}

	if (ntohll(pdu.magic) == NBD_NEGOTIATE_VERSION2_MAGIC) {
		warn("A plain server was expected, but a wrapped server is found");
		return -1;
	}

	if (ntohll(pdu.magic) != NBD_NEGOTIATE_VERSION1_MAGIC) {
		warn("negotiate magic does not match VERSION 1");
		return -1;
	}

	ret = net_recv_all_or_error(sockfd, ((char *)&pdu) + passwd_plus_magic_len, sizeof(pdu) - passwd_plus_magic_len);
	if (ret < 0) {
		warn("receiving negotiate header failed");
		return -1;
	}


	uint64_t size = ntohll(pdu.size);
	uint32_t flags = ntohl(pdu.flags);

	if (size > OFF_MAX) {
		warn("remote size exceeds a local off_t(%zd bytes) value", sizeof(off_t));
		return -1;
	}

	info("remote size: %ju bytes (%ju MBytes)", size, size /1024 /1024);


	if (exportsize)
		*exportsize  = (off_t) size;
	if (exportflags)
		*exportflags = flags;

	return 0;
}
