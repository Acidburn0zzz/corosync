/*
 * Copyright (c) 2009-2011 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Christine Caulfield <ccaulfie@redhat.com>
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Red Hat Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <corosync/corotypes.h>
#include <corosync/totem/totem.h>
#include <corosync/cfg.h>
#include <corosync/confdb.h>
#include <corosync/quorum.h>
#include <corosync/votequorum.h>

typedef enum {
	NODEID_FORMAT_DECIMAL,
	NODEID_FORMAT_HEX
} nodeid_format_t;

typedef enum {
	ADDRESS_FORMAT_NAME,
	ADDRESS_FORMAT_IP
} name_format_t;

typedef enum {
	CMD_UNKNOWN,
	CMD_SHOWNODES,
	CMD_SHOWSTATUS,
	CMD_SETVOTES,
	CMD_SETEXPECTED
} command_t;

/*
 * global vars
 */

/*
 * confdb bits
 */
static confdb_handle_t confdb_handle;
static confdb_callbacks_t confdb_callbacks = {
	.confdb_key_change_notify_fn = NULL,
	.confdb_object_create_change_notify_fn = NULL,
	.confdb_object_delete_change_notify_fn = NULL
};

/*
 * quorum bits
 */
static void quorum_notification_fn(
	quorum_handle_t handle,
	uint32_t quorate,
	uint64_t ring_id,
	uint32_t view_list_entries,
	uint32_t *view_list);

static quorum_handle_t q_handle;
static quorum_callbacks_t q_callbacks = {
	.quorum_notify_fn = quorum_notification_fn
};

/*
 * quorum call back vars
 */
static uint32_t g_quorate;
static uint64_t g_ring_id;
static uint32_t g_view_list_entries;
static uint32_t *g_view_list;
static uint32_t g_called;

/*
 * votequorum bits
 */
static votequorum_handle_t v_handle;
static votequorum_callbacks_t v_callbacks = {
	.votequorum_notify_fn = NULL,
	.votequorum_expectedvotes_notify_fn = NULL
};

/*
 * cfg bits
 */
static corosync_cfg_handle_t c_handle;
static corosync_cfg_callbacks_t c_callbacks = {
	.corosync_cfg_state_track_callback = NULL,
	.corosync_cfg_shutdown_callback = NULL
};

static void show_usage(const char *name)
{
	printf("usage: \n");
	printf("%s <options>\n", name);
	printf("\n");
	printf("  options:\n");
	printf("\n");
	printf("  -s             show quorum status\n");
	printf("  -l             list nodes\n");
	printf("  -v <votes>     change the number of votes for a node *\n");
	printf("  -n <nodeid>    optional nodeid of node for -v\n");
	printf("  -e <expected>  change expected votes for the cluster *\n");
	printf("  -H             show nodeids in hexadecimal rather than decimal\n");
	printf("  -i             show node IP addresses instead of the resolved name\n");
	printf("  -h             show this help text\n");
	printf("\n");
	printf("  * Starred items only work if votequorum is the quorum provider for corosync\n");
	printf("\n");
}

/*
 * Caller should free the returned string
 */
static int get_quorum_type(char *quorum_type, size_t quorum_type_len)
{
	int err;
	hdb_handle_t quorum_handle;
	char buf[256];
	size_t namelen = 0;

	if ((!quorum_type) || (quorum_type_len <= 0)) {
		errno = EINVAL;
		return -1;
	}

	memset(quorum_type, 0, quorum_type_len);

	err = confdb_object_find_start(confdb_handle, OBJECT_PARENT_HANDLE);
	if (err != CS_OK) {
		goto out;
	}

	err = confdb_object_find(confdb_handle, OBJECT_PARENT_HANDLE, (void *)"quorum", strlen("quorum"), &quorum_handle);
	if (err != CS_OK) {
		goto out;
	}

	err = confdb_key_get(confdb_handle, quorum_handle, (void *)"provider", strlen("provider"), buf, &namelen);
	if (err != CS_OK) {
		goto out;
	}

	if (namelen >= sizeof(buf)) {
		namelen = sizeof(buf) - 1;
	}
	buf[namelen] = '\0';

	strncpy(quorum_type, buf, quorum_type_len - 1);

out:
	return err;
}

/*
 * Returns 1 if 'votequorum' is active. The called then knows that
 * votequorum calls should work and can provide extra information
 */
static int using_votequorum(void)
{
	char quorumtype[256];
	int using_voteq;

	if (get_quorum_type(quorumtype, sizeof(quorumtype))) {
		return -1;
	}

	if (strcmp(quorumtype, "corosync_votequorum") == 0) {
		using_voteq = 1;
	} else {
		using_voteq = 0;
	}

	return using_voteq;
}

static int set_votes(uint32_t nodeid, int votes)
{
	int err;

	if ((err=votequorum_initialize(&v_handle, &v_callbacks)) != CS_OK) {
		fprintf(stderr, "votequorum_initialize FAILED: %d, this is probably a configuration error\n", err);
		return err;
	}

	if ((err=votequorum_setvotes(v_handle, nodeid, votes)) != CS_OK) {
		fprintf(stderr, "set votes FAILED: %d\n", err);
	}

	return err==CS_OK?0:err;
}

static int set_expected(int expected_votes)
{
	int err;

	if ((err=votequorum_initialize(&v_handle, &v_callbacks)) != CS_OK) {
		fprintf(stderr, "votequorum_initialize FAILED: %d, this is probably a configuration error\n", err);
		return err;
	}

	if ((err=votequorum_setexpected(v_handle, expected_votes)) != CS_OK) {
		fprintf(stderr, "set expected votes FAILED: %d\n", err);
	}

	return err==CS_OK?0:err;
}

static int get_votes(uint32_t nodeid)
{
	int votes = -1;
	struct votequorum_info info;

	if (votequorum_getinfo(v_handle, nodeid, &info) == CS_OK) {
		votes = info.node_votes;
	}

	return votes;
}

/*
 * This resolves the first address assigned to a node
 * and returns the name or IP address. Use cfgtool if you need more information.
 */
static const char *node_name(uint32_t nodeid, name_format_t name_format)
{
	int ret;
	int numaddrs;
	corosync_cfg_node_address_t addrs[INTERFACE_MAX];

	if (corosync_cfg_get_node_addrs(c_handle, nodeid, INTERFACE_MAX, &numaddrs, addrs) == CS_OK) {

		static char buf[INET6_ADDRSTRLEN];
		socklen_t addrlen;
		struct sockaddr_storage *ss = (struct sockaddr_storage *)addrs[0].address;

		if (ss->ss_family == AF_INET6)
			addrlen = sizeof(struct sockaddr_in6);
		else
			addrlen = sizeof(struct sockaddr_in);

		ret = getnameinfo((struct sockaddr *)addrs[0].address, addrlen,
				  buf, sizeof(buf),
				  NULL, 0,
				  (name_format == ADDRESS_FORMAT_IP)?NI_NUMERICHOST:0);
		if (!ret)
			return buf;
	}

	return "";
}

static void quorum_notification_fn(
	quorum_handle_t handle,
	uint32_t quorate,
	uint64_t ring_id,
	uint32_t view_list_entries,
	uint32_t *view_list)
{
	g_called = 1;
	g_quorate = quorate;
	g_ring_id = ring_id;
	g_view_list_entries = view_list_entries;
	g_view_list = malloc(sizeof(uint32_t) * view_list_entries);
	if (g_view_list) {
		memcpy(g_view_list, view_list,sizeof(uint32_t) * view_list_entries);
	}
}

/*
 * return  1 if quorate
 *         0 if not quorate
 *        -1 on error
 */
static int show_status(void)
{
	struct votequorum_info info;
	int is_quorate;
	int err;
	char quorum_type[256];

	err=quorum_getquorate(q_handle, &is_quorate);
	if (err != CS_OK) {
		fprintf(stderr, "quorum_getquorate FAILED: %d\n", err);
		goto quorum_err;
	}

	err=quorum_trackstart(q_handle, CS_TRACK_CURRENT);
	if (err != CS_OK) {
		fprintf(stderr, "quorum_trackstart FAILED: %d\n", err);
		goto quorum_err;
	}

	g_called = 0;
	while (g_called == 0 && err == CS_OK) {
		err = quorum_dispatch(q_handle, CS_DISPATCH_ONE);
		if (err != CS_OK) {
			fprintf(stderr, "quorum_dispatch FAILED: %d\n", err);
		}
	}

	if (quorum_trackstop(q_handle) != CS_OK) {
		fprintf(stderr, "quorum_trackstop FAILED: %d\n", err);
	}

quorum_err:
	if (err < 0) {
		return err;
	}

	get_quorum_type(quorum_type, sizeof(quorum_type));

	printf("Version:          %s\n", VERSION);
	printf("Nodes:            %d\n", g_view_list_entries);
	printf("Ring ID:          %" PRIu64 "\n", g_ring_id);
	if (get_quorum_type(quorum_type, sizeof(quorum_type))) {
		strncpy(quorum_type, "Not configured", sizeof(quorum_type) - 1);
	}
	printf("Quorum type:      %s\n", quorum_type);
	printf("Quorate:          %s\n", is_quorate?"Yes":"No");

	if (!v_handle) {
		return is_quorate;
	}

	if ((err=votequorum_getinfo(v_handle, 0, &info)) == CS_OK) {
		printf("Node votes:       %d\n", info.node_votes);
		printf("Expected votes:   %d\n", info.node_expected_votes);
		printf("Highest expected: %d\n", info.highest_expected);
		printf("Total votes:      %d\n", info.total_votes);
		printf("Quorum:           %d %s\n", info.quorum, info.flags & VOTEQUORUM_INFO_FLAG_QUORATE?" ":"Activity blocked");
		printf("Flags:            ");
		if (info.flags & VOTEQUORUM_INFO_FLAG_HASSTATE) printf("HasState ");
		if (info.flags & VOTEQUORUM_INFO_FLAG_DISALLOWED) printf("DisallowedNodes ");
		if (info.flags & VOTEQUORUM_INFO_FLAG_TWONODE) printf("2Node ");
		if (info.flags & VOTEQUORUM_INFO_FLAG_QUORATE) printf("Quorate ");
		printf("\n");
	} else {
		fprintf(stderr, "votequorum_getinfo FAILED: %d\n", err);
	}

	if (err != CS_OK) {
		return err;
	}
	return is_quorate;
}

static int show_nodes(nodeid_format_t nodeid_format, name_format_t name_format)
{
	int i;
	int err;
	int result = EXIT_FAILURE;

	err = quorum_trackstart(q_handle, CS_TRACK_CURRENT);
	if (err != CS_OK) {
		fprintf(stderr, "quorum_trackstart FAILED: %d\n", err);
		goto err_exit;
	}

	g_called = 0;
	while (g_called == 0) {
		quorum_dispatch(q_handle, CS_DISPATCH_ONE);
	}

	quorum_finalize(q_handle);
	q_handle = 0;

	err = corosync_cfg_initialize(&c_handle, &c_callbacks);
	if (err != CS_OK) {
		fprintf(stderr, "Cannot initialise CFG service\n");
		c_handle = 0;
		goto err_exit;
	}

	if (v_handle) {
		printf("Nodeid     Votes  Name\n");
	} else {
		printf("Nodeid     Name\n");
	}

	for (i=0; i < g_view_list_entries; i++) {
		if (nodeid_format == NODEID_FORMAT_DECIMAL) {
			printf("%4u   ", g_view_list[i]);
		} else {
			printf("0x%04x   ", g_view_list[i]);
		}
		if (v_handle) {
			printf("%3d  %s\n",  get_votes(g_view_list[i]), node_name(g_view_list[i], name_format));
		} else {
			printf("%s\n", node_name(g_view_list[i], name_format));
		}
	}

	result = EXIT_SUCCESS;
err_exit:
	return result;
}

/*
 * return -1 on error
 *         0 if OK
 */

static int init_all(void) {
	confdb_handle = 0;
	q_handle = 0;
	v_handle = 0;
	c_handle = 0;

	if (confdb_initialize(&confdb_handle, &confdb_callbacks) != CS_OK) {
		fprintf(stderr, "Cannot initialize CONFDB service\n");
		confdb_handle = 0;
		goto out;
	}

	if (quorum_initialize(&q_handle, &q_callbacks) != CS_OK) {
		fprintf(stderr, "Cannot initialize QUORUM service\n");
		q_handle = 0;
		goto out;
	}

	if (corosync_cfg_initialize(&c_handle, &c_callbacks) != CS_OK) {
		fprintf(stderr, "Cannot initialise CFG service\n");
		c_handle = 0;
		goto out;
	}

	if (using_votequorum() <= 0) {
		return 0;
	}

	if (votequorum_initialize(&v_handle, &v_callbacks) != CS_OK) {
		fprintf(stderr, "Cannot initialise VOTEQUORUM service\n");
		v_handle = 0;
		goto out;
	}

	return 0;
out:
	return -1;
}

static void close_all(void) {
	if (confdb_handle) {
		confdb_finalize(confdb_handle);
	}
	if (q_handle) {
		quorum_finalize(q_handle);
	}
	if (c_handle) {
		corosync_cfg_finalize(c_handle);
	}
	if (v_handle) {
		votequorum_finalize(v_handle);
	}
}

int main (int argc, char *argv[]) {
	const char *options = "VHsle:v:hin:d:";
	char *endptr;
	int opt;
	int votes = 0;
	int ret = 0;
	uint32_t nodeid = VOTEQUORUM_NODEID_US;
	nodeid_format_t nodeid_format = NODEID_FORMAT_DECIMAL;
	name_format_t address_format = ADDRESS_FORMAT_NAME;
	command_t command_opt = CMD_UNKNOWN;

	if (argc == 1) {
		show_usage (argv[0]);
		exit(0);
	}

	if (init_all()) {
		close_all();
		exit(1);
	}

	while ( (opt = getopt(argc, argv, options)) != -1 ) {
		switch (opt) {
		case 's':
			command_opt = CMD_SHOWSTATUS;
			break;
		case 'i':
			address_format = ADDRESS_FORMAT_IP;
			break;
		case 'H':
			nodeid_format = NODEID_FORMAT_HEX;
			break;
		case 'l':
			command_opt = CMD_SHOWNODES;
			break;
		case 'e':
			if (using_votequorum() > 0) {
				votes = strtol(optarg, &endptr, 0);
				if ((votes == 0 && endptr == optarg) || votes <= 0) {
					fprintf(stderr, "New expected votes value was not valid, try a positive number\n");
				} else {
					command_opt = CMD_SETEXPECTED;
				}
			} else {
				fprintf(stderr, "You cannot change expected votes, corosync is not using votequorum\n");
				exit(2);
			}
			break;
		case 'n':
			nodeid = strtol(optarg, &endptr, 0);
			if ((nodeid == 0 && endptr == optarg) || nodeid <= 0) {
				fprintf(stderr, "The nodeid was not valid, try a positive number\n");
			}
			break;
		case 'v':
			if (using_votequorum() > 0) {
				votes = strtol(optarg, &endptr, 0);
				if ((votes == 0 && endptr == optarg) || votes < 0) {
					fprintf(stderr, "New votes value was not valid, try a positive number or zero\n");
				} else {
					command_opt = CMD_SETVOTES;
				}
			}
			else {
				fprintf(stderr, "You cannot change node votes, corosync is not using votequorum\n");
				exit(2);
			}
			break;
		case 'h':
		case '?':
		default:
		break;
		}
	}

	switch (command_opt) {
	case CMD_UNKNOWN:
		show_usage(argv[0]);
		ret = -1;
		break;
	case CMD_SHOWNODES:
		ret = show_nodes(nodeid_format, address_format);
		break;
	case CMD_SHOWSTATUS:
		ret = show_status();
		break;
	case CMD_SETVOTES:
		ret = set_votes(nodeid, votes);
		break;
	case CMD_SETEXPECTED:
		ret = set_expected(votes);
		break;
	}

	close_all();

	return (ret);
}
