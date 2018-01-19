/*
 * Copyright (C) 2017-2018  B.A.T.M.A.N. contributors:
 *
 * Sven Eckelmann <sven@narfation.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 *
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <net/ethernet.h>
#include <netinet/ether.h>

#include "main.h"
#include "batfilter_genl.h"
#include "functions.h"
#include "peerfilter.h"
#include "list.h"

/**
 * struct batadv_nlquery_opts - internal state for batfilter_genl_query()
 *
 * This structure should be used as member of a struct which tracks the state
 * for the callback. The macro container_of can be used to convert the
 * arg pointer from batadv_nlquery_opts to the member which contains this
 * struct.
 */
struct batadv_nlquery_opts {
	/** @err: current error  */
	int err;
};

/**
 * BATADV_ARRAY_SIZE() - Get number of items in static array
 * @x: array with known length
 *
 * Return:  number of items in array
 */
#ifndef BATADV_ARRAY_SIZE
#define BATADV_ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#endif


/**
 * batadv_genl_missing_attrs() - Check whether @attrs is missing mandatory
 *  attribute
 * @attrs: attributes which was parsed by nla_parse()
 * @mandatory: list of required attributes
 * @num: number of required attributes in @mandatory
 *
 * Return: Return true when a attribute is missing, false otherwise
 */
static inline bool
batadv_genl_missing_attrs(struct nlattr *attrs[],
			  const enum batfilter_genl_attrs mandatory[], size_t num)
{
	size_t i;

	for (i = 0; i < num; i++) {
		if (!attrs[mandatory[i]])
			return true;
	}

	return false;
}

static struct nla_policy batfilter_genl_policy[NUM_BATFILTER_ATTR] = {
	[BATFILTER_ATTR_MESH_IFINDEX]	= { .type = NLA_U32 },
	[BATFILTER_ATTR_PLAYDEAD]	= { .type = NLA_FLAG },
	[BATFILTER_ATTR_NEIGH_ADDRESS]	= { .type = NLA_UNSPEC,
					    .minlen = ETH_ALEN,
					    .maxlen = ETH_ALEN },
	[BATFILTER_ATTR_LOSS_RATE]	= { .type = NLA_U8 },
};

/**
 * nlquery_error_cb() - Store error value in &batadv_nlquery_opts->error and
 *  stop processing
 * @nla: netlink address of the peer
 * @nlerr: netlink error message being processed
 * @arg: &struct batadv_nlquery_opts given to batfilter_genl_query()
 *
 * Return: Always NL_STOP
 */
static int nlquery_error_cb(struct sockaddr_nl *nla __attribute__((unused)),
			    struct nlmsgerr *nlerr, void *arg)
{
	struct batadv_nlquery_opts *query_opts = arg;

	query_opts->err = nlerr->error;

	return NL_STOP;
}

/**
 * nlquery_stop_cb() - Store error value in &batadv_nlquery_opts->error and
 *  stop processing
 * @msg: netlink message being processed
 * @arg: &struct batadv_nlquery_opts given to batfilter_genl_query()
 *
 * Return: Always NL_STOP
 */
static int nlquery_stop_cb(struct nl_msg *msg, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	struct batadv_nlquery_opts *query_opts = arg;
	int *error = nlmsg_data(nlh);

	if (*error)
		query_opts->err = *error;

	return NL_STOP;
}

/**
 * batfilter_genl_query() - Start a common batman-adv generic netlink query
 * @mesh_iface: name of the batman-adv mesh interface
 * @nl_cmd: &enum batfilter_genl_commands which should be sent to kernel
 * @attribute_cb: callback to add more attributes
 * @callback: receive callback for valid messages
 * @flags: additional netlink message header flags
 * @query_opts: pointer to &struct batadv_nlquery_opts which is used to save
 *  the current processing state. This is given as arg to @callback
 *
 * Return: 0 on success or negative error value otherwise
 */
static int batfilter_genl_query(const char *mesh_iface,
				enum batfilter_genl_commands nl_cmd,
				nl_recvmsg_msg_cb_t attribute_cb,
				nl_recvmsg_msg_cb_t callback, int flags,
				struct batadv_nlquery_opts *query_opts)
{
	struct nl_sock *sock;
	struct nl_msg *msg;
	struct nl_cb *cb;
	int ifindex;
	int family;
	int ret;

	query_opts->err = 0;

	sock = nl_socket_alloc();
	if (!sock)
		return -ENOMEM;

	ret = genl_connect(sock);
	if (ret < 0) {
		query_opts->err = ret;
		goto err_free_sock;
	}

	family = genl_ctrl_resolve(sock, BATFILTER_GENL_NAME);
	if (family < 0) {
		query_opts->err = -EOPNOTSUPP;
		goto err_free_sock;
	}

	ifindex = if_nametoindex(mesh_iface);
	if (!ifindex) {
		query_opts->err = -ENODEV;
		goto err_free_sock;
	}

	cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (!cb) {
		query_opts->err = -ENOMEM;
		goto err_free_sock;
	}

	if (callback)
		nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, callback, query_opts);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, nlquery_stop_cb, query_opts);
	nl_cb_err(cb, NL_CB_CUSTOM, nlquery_error_cb, query_opts);

	msg = nlmsg_alloc();
	if (!msg) {
		query_opts->err = -ENOMEM;
		goto err_free_cb;
	}

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0, flags,
		    nl_cmd, 1);

	nla_put_u32(msg, BATFILTER_ATTR_MESH_IFINDEX, ifindex);
	if (attribute_cb) {
		ret = attribute_cb(msg, query_opts);
		if (ret < 0) {
			query_opts->err = -ENOMEM;
			goto err_free_cb;
		}
	}

	nl_send_auto_complete(sock, msg);
	nlmsg_free(msg);

	nl_recvmsgs(sock, cb);

err_free_cb:
	nl_cb_put(cb);
err_free_sock:
	nl_socket_free(sock);

	return query_opts->err;
}

struct playdead_list_genl_opts {
	bool playdead;
	struct batadv_nlquery_opts query_opts;
};

static int peerfilter_playdead_list_cb(struct nl_msg *msg, void *arg)
{
	struct nlattr *attrs[BATFILTER_ATTR_MAX+1];
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	struct batadv_nlquery_opts *query_opts = arg;
	struct genlmsghdr *ghdr;
	struct playdead_list_genl_opts *opts;

	opts = container_of(query_opts, struct playdead_list_genl_opts,
			    query_opts);

	if (!genlmsg_valid_hdr(nlh, 0))
		return NL_OK;

	ghdr = nlmsg_data(nlh);

	if (ghdr->cmd != BATFILTER_CMD_GET_PLAYDEAD)
		return NL_OK;

	if (nla_parse(attrs, BATFILTER_ATTR_MAX, genlmsg_attrdata(ghdr, 0),
		      genlmsg_len(ghdr), batfilter_genl_policy))
		return NL_OK;

	opts->playdead = !!attrs[BATFILTER_ATTR_PLAYDEAD];

	return NL_STOP;
}

static int peerfilter_playdead_list(const char *mesh_iface)
{
	struct playdead_list_genl_opts opts = {
		.playdead = false,
		.query_opts = {
			.err = 0,
		},
	};
	int ret;

	ret = batfilter_genl_query(mesh_iface, BATFILTER_CMD_GET_PLAYDEAD,
				   NULL, peerfilter_playdead_list_cb, 0,
				   &opts.query_opts);
	if (ret < 0) {
		fprintf(stderr, "Failed to retrieve playdead state\n");
		return ret;
	}

	if (opts.playdead)
		printf("on\n");
	else
		printf("off\n");

	return 0;
}

struct playdead_set_genl_opts {
	bool playdead;
	struct batadv_nlquery_opts query_opts;
};

static int peerfilter_playdead_set_argscb(struct nl_msg *msg, void *arg)
{
	struct batadv_nlquery_opts *query_opts = arg;
	struct playdead_set_genl_opts *opts;

	opts = container_of(query_opts, struct playdead_set_genl_opts,
			    query_opts);

	if (opts->playdead)
		return nla_put_flag(msg, BATFILTER_ATTR_PLAYDEAD);
	else
		return 0;
}

static int peerfilter_playdead_set(const char *mesh_iface, const char *val)
{
	struct playdead_set_genl_opts opts = {
		.playdead = false,
		.query_opts = {
			.err = 0,
		},
	};
	int ret;

	if (strcasecmp(val, "off") == 0 ||
	    strcasecmp(val, "0") == 0 ||
	    strcasecmp(val, "disable") == 0) {
		opts.playdead = false;
	} else if (strcasecmp(val, "on") == 0 ||
		   strcasecmp(val, "1") == 0 ||
		   strcasecmp(val, "enable") == 0) {
		opts.playdead = true;
	} else {
		fprintf(stderr, "Failed to parse playdead argument: %s\n", val);
		return -EINVAL;
	}

	ret = batfilter_genl_query(mesh_iface, BATFILTER_CMD_SET_PLAYDEAD,
				   peerfilter_playdead_set_argscb, NULL, 0,
				   &opts.query_opts);
	if (ret < 0) {
		fprintf(stderr, "Failed to retrieve playdead state\n");
		return ret;
	}

	return 0;
}

static int peerfilter_playdead(const char *mesh_iface, int argc, char **argv)
{
	if (argc == 0)
		return peerfilter_playdead_list(mesh_iface);
	else
		return peerfilter_playdead_set(mesh_iface, argv[0]);

	return -EINVAL;
}

static const enum batfilter_genl_attrs peerfilter_list_mandatory[] = {
	BATFILTER_ATTR_NEIGH_ADDRESS,
	BATFILTER_ATTR_LOSS_RATE,
};

static int peerfilter_list_cb(struct nl_msg *msg, void *arg __maybe_unused)
{
	struct nlattr *attrs[BATFILTER_ATTR_MAX+1];
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	struct genlmsghdr *ghdr;
	const uint8_t *neigh_addr;
	uint8_t loss_rate;

	if (!genlmsg_valid_hdr(nlh, 0))
		return NL_OK;

	ghdr = nlmsg_data(nlh);

	if (ghdr->cmd != BATFILTER_CMD_GET_PEERFILTER)
		return NL_OK;

	if (nla_parse(attrs, BATFILTER_ATTR_MAX, genlmsg_attrdata(ghdr, 0),
		      genlmsg_len(ghdr), batfilter_genl_policy))
		return NL_OK;

	if (batadv_genl_missing_attrs(attrs, peerfilter_list_mandatory,
				      BATADV_ARRAY_SIZE(peerfilter_list_mandatory)))
		return NL_OK;

	neigh_addr = nla_data(attrs[BATFILTER_ATTR_NEIGH_ADDRESS]);
	loss_rate = nla_get_u8(attrs[BATFILTER_ATTR_LOSS_RATE]);

	printf("%s,%u\n", ether_ntoa_long((struct ether_addr *)neigh_addr), loss_rate);

	return NL_OK;
}

static int peerfilter_list(const char *mesh_iface)
{
	struct batadv_nlquery_opts opts = {
		.err = 0,
	};
	int ret;

	ret = batfilter_genl_query(mesh_iface, BATFILTER_CMD_GET_PEERFILTER,
				   NULL, peerfilter_list_cb,
				   NLM_F_DUMP, &opts);
	if (ret < 0) {
		fprintf(stderr, "Failed to retrieve peer filters\n");
		return ret;
	}

	return 0;
}

struct peerfilter_add_genl_opts {
	uint8_t neigh_addr[ETH_ALEN];
	uint8_t loss_rate;
	struct batadv_nlquery_opts query_opts;
};

static int peerfilter_add_argscb(struct nl_msg *msg, void *arg)
{
	struct batadv_nlquery_opts *query_opts = arg;
	struct peerfilter_add_genl_opts *opts;

	opts = container_of(query_opts, struct peerfilter_add_genl_opts,
			    query_opts);

	nla_put(msg, BATFILTER_ATTR_NEIGH_ADDRESS, ETH_ALEN, opts->neigh_addr);
	nla_put_u8(msg, BATFILTER_ATTR_LOSS_RATE, opts->loss_rate);

	return 0;
}

static int peerfilter_add(const char *mesh_iface, int argc, char **argv)
{
	struct peerfilter_add_genl_opts opts = {
		.query_opts = {
			.err = 0,
		},
	};
	struct ether_addr *mac_addr;
	unsigned long loss_rate;
	int ret;

	if (argc < 1 || argc > 2) {
		fprintf(stderr, "Invalid numer of parameters for peerfilter add\n");
		return -EINVAL;
	}

	mac_addr = ether_aton(argv[0]);
	if (!mac_addr) {
		fprintf(stderr, "Failed to parse mac address: %s\n", argv[0]);
		return -EINVAL;
	}

	memcpy(opts.neigh_addr, mac_addr->ether_addr_octet, ETH_ALEN);

	if (argc < 2) {
		opts.loss_rate = 255;
	} else {
		loss_rate = strtoul(argv[1], NULL, 0);
		if (loss_rate > 255) {
			fprintf(stderr, "Loss rate is higher than 255\n");
			return -EINVAL;
		}

		opts.loss_rate = loss_rate;
	}

	ret = batfilter_genl_query(mesh_iface, BATFILTER_CMD_ADD_PEERFILTER,
				   peerfilter_add_argscb, NULL, 0,
				   &opts.query_opts);
	if (ret < 0 && ret != -EEXIST) {
		fprintf(stderr, "Failed to add new peer filter\n");
		return ret;
	}

	return 0;
}

struct peerfilter_del_genl_opts {
	uint8_t neigh_addr[ETH_ALEN];
	struct batadv_nlquery_opts query_opts;
};

static int peerfilter_del_argscb(struct nl_msg *msg, void *arg)
{
	struct batadv_nlquery_opts *query_opts = arg;
	struct peerfilter_del_genl_opts *opts;

	opts = container_of(query_opts, struct peerfilter_del_genl_opts,
			    query_opts);

	nla_put(msg, BATFILTER_ATTR_NEIGH_ADDRESS, ETH_ALEN, opts->neigh_addr);

	return 0;
}

static int peerfilter_del(const char *mesh_iface, int argc, char **argv)
{
	struct peerfilter_del_genl_opts opts = {
		.query_opts = {
			.err = 0,
		},
	};
	struct ether_addr *mac_addr;
	int ret;

	if (argc != 1) {
		fprintf(stderr, "Invalid numer of parameters for peerfilter del\n");
		return -EINVAL;
	}

	mac_addr = ether_aton(argv[0]);
	if (!mac_addr) {
		fprintf(stderr, "Failed to parse mac address: %s\n", argv[0]);
		return -EINVAL;
	}

	memcpy(opts.neigh_addr, mac_addr->ether_addr_octet, ETH_ALEN);

	ret = batfilter_genl_query(mesh_iface, BATFILTER_CMD_DEL_PEERFILTER,
				   peerfilter_del_argscb, NULL, 0,
				   &opts.query_opts);
	if (ret < 0) {
		fprintf(stderr, "Failed to remove peer filter\n");
		return ret;
	}

	return 0;
}

static void peerfilter_usage(void)
{
	fprintf(stderr, "Usage: batctl [options] peerfilter\n");
	fprintf(stderr, "Usage: batctl [options] peerfilter add MACADDR LOSS\n");
	fprintf(stderr, "Usage: batctl [options] peerfilter del MACADDR\n");
	fprintf(stderr, "Usage: batctl [options] peerfilter playdead\n");
	fprintf(stderr, "Usage: batctl [options] peerfilter playdead [0|1]\n");
	fprintf(stderr, "parameters:\n");
	fprintf(stderr, " \t -h print this help\n");
}

int peerfilter(char *mesh_iface, int argc, char **argv)
{
	int ret = EXIT_FAILURE, res;
	int optchar;
	int rest_argc;
	char **rest_argv;

	while ((optchar = getopt(argc, argv, "h")) != -1) {
		switch (optchar) {
		case 'h':
			peerfilter_usage();
			return EXIT_SUCCESS;
		default:
			peerfilter_usage();
			return EXIT_FAILURE;
		}
	}

	check_root_or_die("batctl peerfilter");

	rest_argc = argc - optind;
	rest_argv = &argv[optind];

	if (rest_argc == 0) {
		peerfilter_list(mesh_iface);
	} else if (strcmp(rest_argv[0], "add") == 0) {
		res = peerfilter_add(mesh_iface, rest_argc - 1, &rest_argv[1]);
		if (res < 0)
			return EXIT_FAILURE;
	} else if (strcmp(rest_argv[0], "del") == 0) {
		res = peerfilter_del(mesh_iface, rest_argc - 1, &rest_argv[1]);
		if (res < 0)
			return EXIT_FAILURE;
	} else if (strcmp(rest_argv[0], "playdead") == 0) {
		res = peerfilter_playdead(mesh_iface, rest_argc - 1, &rest_argv[1]);
		if (res < 0)
			return EXIT_FAILURE;
	} else {
		fprintf(stderr, "Error - subcommand unknown: %s\n", rest_argv[0]);
		peerfilter_usage();
		return EXIT_FAILURE;
	}

	ret = EXIT_SUCCESS;

	return ret;
}
