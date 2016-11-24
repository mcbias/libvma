/*
 * Copyright (c) 2016 Mellanox Technologies, Ltd. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "vma/lwip/tcp.h"    /* display TCP states */
#include "vma/util/agent.h"  /* get message layout format */
#include "hash.h"
#include "daemon.h"


int open_message(void);
void close_message(void);
int proc_message(void);

static int proc_msg_init(struct vma_hdr *msg_hdr, size_t size, struct sockaddr_un *peeraddr);
static int proc_msg_exit(struct vma_hdr *msg_hdr, size_t size);
static int proc_msg_state(struct vma_hdr *msg_hdr, size_t size);


int open_message(void)
{
	int rc = 0;
	int optval = 1;
	struct sockaddr_un server_addr;

	/* Create UNIX UDP socket to receive data from VMA processes */
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sun_family = AF_UNIX;
	strncpy(server_addr.sun_path, daemon_cfg.sock_file, sizeof(server_addr.sun_path) - 1);
	/* remove possible old socket */
	unlink(daemon_cfg.sock_file);

	if ((daemon_cfg.sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0) {
		log_error("Failed to call socket() errno %d (%s)\n", errno,
				strerror(errno));
		rc = -errno;
		goto err;
	}

	optval = 1;
	rc = setsockopt(daemon_cfg.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (rc < 0) {
		log_error("Failed to call setsockopt() errno %d (%s)\n", errno,
				strerror(errno));
		rc = -errno;
		goto err;
	}

	/* bind created socket */
	if (bind(daemon_cfg.sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		log_error("Failed to call bind() errno %d (%s)\n", errno,
				strerror(errno));
		rc = -errno;
		goto err;
	}

	/* Make the socket non-blocking */
	optval = fcntl(daemon_cfg.sock_fd, F_GETFL);
	if (optval < 0) {
		rc = -errno;
		log_error("Failed to get socket flags errno %d (%s)\n", errno,
				strerror(errno));
		goto err;
	}
	optval |= O_NONBLOCK;
	rc = fcntl(daemon_cfg.sock_fd, F_SETFL, optval);
	if (rc < 0) {
		rc = -errno;
		log_error("Failed to set socket flags errno %d (%s)\n", errno,
				strerror(errno));
		goto err;
	}

err:
	return rc;
}

void close_message(void)
{
	if (daemon_cfg.sock_fd > 0) {
		close(daemon_cfg.sock_fd);
	}
	unlink(daemon_cfg.sock_file);
}

int proc_message(void)
{
	int rc = 0;
	struct sockaddr_un peeraddr;
	socklen_t addrlen = sizeof(peeraddr);
	char msg_recv[4096];
	int len = 0;
	struct vma_hdr *msg_hdr = NULL;

again:
	len = recvfrom(daemon_cfg.sock_fd, &msg_recv, sizeof(msg_recv), 0,
			(struct sockaddr *) &peeraddr, &addrlen);
	if ((len < 0 || len < (int)sizeof(struct vma_hdr))) {
		if (errno == EINTR) {
			goto again;
		}
		rc = -errno;
		log_error("Failed recvfrom() errno %d (%s)\n", errno,
				strerror(errno));
		goto err;
	}

	/* Parse and process messages */
	while (len > 0) {
		if (len < (int)sizeof(struct vma_hdr)) {
			rc = -EBADMSG;
			log_error("Invalid message lenght from %s as %d errno %d (%s)\n",
					(addrlen > 0 ? peeraddr.sun_path: "n/a"), len, errno,	strerror(errno));
			goto err;
		}
		msg_hdr = (struct vma_hdr *)&msg_recv;
		log_debug("getting message ([%d] ver: %d pid: %d)\n",
				msg_hdr->code, msg_hdr->ver, msg_hdr->pid);

		switch (msg_hdr->code) {
		case VMA_MSG_INIT:
			rc = proc_msg_init(msg_hdr, len, &peeraddr);
			break;
		case VMA_MSG_STATE:
			rc = proc_msg_state(msg_hdr, len);
			break;
		case VMA_MSG_EXIT:
			rc = proc_msg_exit(msg_hdr, len);
			break;
		default:
			rc = -EPROTO;
			log_error("Received unknow message errno %d (%s)\n", errno,
					strerror(errno));
			goto err;
		}
		if (0 < rc) {
			len -= rc;
			rc = 0;
		} else {
			goto err;
		}
	}

err:
	return rc;
}

static int proc_msg_init(struct vma_hdr *msg_hdr, size_t size, struct sockaddr_un *peeraddr)
{
	struct vma_msg_init *data;
	struct store_pid *value;

	assert(msg_hdr);
	assert(msg_hdr->code == VMA_MSG_INIT);
	assert(size);

	data = (struct vma_msg_init *)msg_hdr;
	if (size < sizeof(*data)) {
		return -EBADMSG;
	}

	/* Message protocol version check */
	if (data->hdr.ver > VMA_AGENT_VER) {
		log_error("Protocol message mismatch (VMA_AGENT_VER = %d) errno %d (%s)\n",
				VMA_AGENT_VER, errno, strerror(errno));
		return -EBADMSG;
	}

	/* Allocate memory for this value in this place
	 * Free this memory during hash_del() call or hash_destroy()
	 */
	value = (void *)calloc(1, sizeof(*value));
	if (NULL == value) {
		return -ENOMEM;
	}

	value->pid = data->hdr.pid;
	value->lib_ver = data->ver;
	gettimeofday(&value->t_start, NULL);

	value->ht = hash_create(&free, daemon_cfg.opt.max_fid_num);
	if (NULL == value->ht) {
		log_error("Failed hash_create() for %d entries errno %d (%s)\n",
				daemon_cfg.opt.max_fid_num, errno, strerror(errno));
		free(value);
		return -EFAULT;
	}

	if (hash_put(daemon_cfg.ht, value->pid, value) != value) {
		log_error("Failed hash_put() count: %d size: %d errno %d (%s)\n",
				hash_count(daemon_cfg.ht), hash_size(daemon_cfg.ht),
				errno, strerror(errno));
		hash_destroy(value->ht);
		free(value);
		return -EFAULT;
	}

	log_debug("[%d] put into the storage\n", data->hdr.pid);

	data->hdr.code |= VMA_MSG_ACK;
	data->hdr.ver = VMA_AGENT_VER;
	if (0 > sys_sendto(daemon_cfg.sock_fd, data, sizeof(*data), 0,
			(struct sockaddr *)peeraddr, sizeof(*peeraddr))) {
		log_error("Failed sendto() message errno %d (%s)\n", errno,
				strerror(errno));
	}

	return (sizeof(*data));
}

static int proc_msg_exit(struct vma_hdr *msg_hdr, size_t size)
{
	struct vma_msg_exit *data;

	assert(msg_hdr);
	assert(msg_hdr->code == VMA_MSG_EXIT);
	assert(size);

	data = (struct vma_msg_exit *)msg_hdr;
	if (size < sizeof(*data)) {
		return -EBADMSG;
	}

	hash_del(daemon_cfg.ht, data->hdr.pid);

	log_debug("[%d] remove from the storage\n", data->hdr.pid);

	return (sizeof(*data));
}

static int proc_msg_state(struct vma_hdr *msg_hdr, size_t size)
{
	struct vma_msg_state *data;
	struct store_pid *pid_value;
	struct store_fid *value;

	assert(msg_hdr);
	assert(msg_hdr->code == VMA_MSG_STATE);
	assert(size);

	data = (struct vma_msg_state *)msg_hdr;
	if (size < sizeof(*data)) {
		return -EBADMSG;
	}

	pid_value = hash_get(daemon_cfg.ht, data->hdr.pid);
	if (NULL == pid_value) {
		log_error("Failed hash_get() for pid %d errno %d (%s)\n",
				data->hdr.pid, errno, strerror(errno));
		return -ENOENT;
	}

	/* Do not store information about closed socket
	 * It is a protection for hypothetical scenario when number for new
	 * sockets are incremented instead of using number
	 * of closed sockets
	 */
	if ((CLOSED == data->state) && (SOCK_STREAM == data->type)) {
		hash_del(pid_value->ht, data->fid);

		log_debug("[%d] remove fid: %d type: %d state: %s\n",
				data->hdr.pid, data->fid, data->type,
				(data->state < (sizeof(tcp_state_str)/sizeof(tcp_state_str[0])) ?
						tcp_state_str[data->state] : "n/a"));
		return (sizeof(*data));
	}

	/* Allocate memory for this value in this place
	 * Free this memory during hash_del() call or hash_destroy()
	 */
	value = (void *)calloc(1, sizeof(*value));
	if (NULL == value) {
		return -ENOMEM;
	}

	value->fid = data->fid;
	value->type = data->type;
	value->state = data->state;
	value->src_ip = data->src_ip;
	value->dst_ip = data->dst_ip;
	value->src_port = data->src_port;
	value->dst_port = data->dst_port;

	if (hash_put(pid_value->ht, value->fid, value) != value) {
		log_error("Failed hash_put() count: %d size: %d errno %d (%s)\n",
				hash_count(pid_value->ht), hash_size(pid_value->ht),
				errno, strerror(errno));
		free(value);
		return -EFAULT;
	}

	log_debug("[%d] update fid: %d type: %d state: %s\n",
			pid_value->pid, value->fid, value->type,
			(value->state < (sizeof(tcp_state_str)/sizeof(tcp_state_str[0])) ?
					tcp_state_str[value->state] : "n/a"));

	return (sizeof(*data));
}