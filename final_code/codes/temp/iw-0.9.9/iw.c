/*
 * nl80211 userspace tool
 *
 * Copyright 2007, 2008	Johannes Berg <johannes@sipsolutions.net>
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
                     
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>  
#include <netlink/msg.h>
#include <netlink/attr.h>

#include "nl80211.h"
#include "iw.h"
#include "version.h"

#ifndef CONFIG_LIBNL20
/* libnl 2.0 compatibility code */

static inline struct nl_handle *nl_socket_alloc(void)
{
	return nl_handle_alloc();
}

static inline void nl_socket_free(struct nl_handle *h)
{
	nl_handle_destroy(h);
}

static inline int __genl_ctrl_alloc_cache(struct nl_handle *h, struct nl_cache **cache)
{
	struct nl_cache *tmp = genl_ctrl_alloc_cache(h);
	if (!tmp)
		return -ENOMEM;
	*cache = tmp;
	return 0;
}
#define genl_ctrl_alloc_cache __genl_ctrl_alloc_cache
#endif /* CONFIG_LIBNL20 */

static int debug = 0;

static int nl80211_init(struct nl80211_state *state)
{
	int err;

	state->nl_handle = nl_socket_alloc();
	if (!state->nl_handle) {
		fprintf(stderr, "Failed to allocate netlink handle.\n");
		return -ENOMEM;
	}

	if (genl_connect(state->nl_handle)) {
		fprintf(stderr, "Failed to connect to generic netlink.\n");
		err = -ENOLINK;
		goto out_handle_destroy;
	}

	if (genl_ctrl_alloc_cache(state->nl_handle, &state->nl_cache)) {
		fprintf(stderr, "Failed to allocate generic netlink cache.\n");
		err = -ENOMEM;
		goto out_handle_destroy;
	}

	state->nl80211 = genl_ctrl_search_by_name(state->nl_cache, "nl80211");
	if (!state->nl80211) {
		fprintf(stderr, "nl80211 not found.\n");
		err = -ENOENT;
		goto out_cache_free;
	}

	return 0;

 out_cache_free:
	nl_cache_free(state->nl_cache);
 out_handle_destroy:
	nl_socket_free(state->nl_handle);
	return err;
}

static void nl80211_cleanup(struct nl80211_state *state)
{
	genl_family_put(state->nl80211);
	nl_cache_free(state->nl_cache);
	nl_socket_free(state->nl_handle);
}

__COMMAND(NULL, NULL, NULL, 0, 0, 0, CIB_NONE, NULL);
__COMMAND(NULL, NULL, NULL, 1, 0, 0, CIB_NONE, NULL);

static int cmd_size;

static void usage(const char *argv0)
{
	struct cmd *cmd;

	fprintf(stderr, "Usage:\t%s [options] command\n", argv0);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t--debug\t\tenable netlink debugging\n");
	fprintf(stderr, "\t--version\tshow version\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "\thelp\n");
	fprintf(stderr, "\tevent\n");
	for (cmd = &__start___cmd; cmd < &__stop___cmd;
	     cmd = (struct cmd *)((char *)cmd + cmd_size)) {
		if (!cmd->handler || cmd->hidden)
			continue;
		switch (cmd->idby) {
		case CIB_NONE:
			fprintf(stderr, "\t");
			/* fall through */
		case CIB_PHY:
			if (cmd->idby == CIB_PHY)
				fprintf(stderr, "\tphy <phyname> ");
			/* fall through */
		case CIB_NETDEV:
			if (cmd->idby == CIB_NETDEV)
				fprintf(stderr, "\tdev <devname> ");
			if (cmd->section)
				fprintf(stderr, "%s ", cmd->section);
			fprintf(stderr, "%s", cmd->name);
			if (cmd->args)
				fprintf(stderr, " %s", cmd->args);
			fprintf(stderr, "\n");
			break;
		}
	}
}

static void version(void)
{
	printf("iw version " IW_VERSION "\n");
}

static int phy_lookup(char *name)
{
	char buf[200];
	int fd, pos;

	snprintf(buf, sizeof(buf), "/sys/class/ieee80211/%s/index", name);

	fd = open(buf, O_RDONLY);
	pos = read(fd, buf, sizeof(buf) - 1);
	if (pos < 0)
		return -1;
	buf[pos] = '\0';
	return atoi(buf);
}

static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err,
			 void *arg)
{
	int *ret = arg;
	*ret = err->error;
	return NL_STOP;
}

static int finish_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;
	*ret = 0;
	return NL_SKIP;
}

static int ack_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;
	*ret = 0;
	return NL_STOP;
}

static int handle_cmd(struct nl80211_state *state,
		      enum command_identify_by idby,
		      int argc, char **argv)
{
	struct cmd *cmd;
	struct nl_cb *cb = NULL;
	struct nl_msg *msg;
	int devidx = 0;
	int err;
	const char *command, *section;

	if (argc <= 1 && idby != CIB_NONE)
		return 1;

	switch (idby) {
	case CIB_PHY:
		devidx = phy_lookup(*argv);
		argc--;
		argv++;
		break;
	case CIB_NETDEV:
		devidx = if_nametoindex(*argv);
		argc--;
		argv++;
		break;
	default:
		break;
	}

	section = command = *argv;
	argc--;
	argv++;

	for (cmd = &__start___cmd; cmd < &__stop___cmd;
	     cmd = (struct cmd *)((char *)cmd + cmd_size)) {
		if (!cmd->handler)
			continue;
		if (cmd->idby != idby)
			continue;
		if (cmd->section) {
			if (strcmp(cmd->section, section))
				continue;
			/* this is a bit icky ... */
			if (command == section) {
				if (argc <= 0)
					return 1;
				command = *argv;
				argc--;
				argv++;
			}
		} else if (section != command)
			continue;
		if (strcmp(cmd->name, command))
			continue;
		if (argc && !cmd->args)
			continue;
		break;
	}

	if (cmd >= &__stop___cmd)
		return 1;

	msg = nlmsg_alloc();
	if (!msg) {
		fprintf(stderr, "failed to allocate netlink message\n");
		return 2;
	}

	cb = nl_cb_alloc(debug ? NL_CB_DEBUG : NL_CB_DEFAULT);
	if (!cb) {
		fprintf(stderr, "failed to allocate netlink callbacks\n");
		err = 2;
		goto out_free_msg;
	}

	genlmsg_put(msg, 0, 0, genl_family_get_id(state->nl80211), 0,
		    cmd->nl_msg_flags, cmd->cmd, 0);

	switch (idby) {
	case CIB_PHY:
		NLA_PUT_U32(msg, NL80211_ATTR_WIPHY, devidx);
		break;
	case CIB_NETDEV:
		NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
		break;
	default:
		break;
	}

	err = cmd->handler(cb, msg, argc, argv);
	if (err)
		goto out;

	err = nl_send_auto_complete(state->nl_handle, msg);
	if (err < 0)
		goto out;

	err = 1;

	nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);

	while (err > 0)
		nl_recvmsgs(state->nl_handle, cb);
 out:
	nl_cb_put(cb);
 out_free_msg:
	nlmsg_free(msg);
	return err;
 nla_put_failure:
	fprintf(stderr, "building message failed\n");
	return 2;
}

static int no_seq_check(struct nl_msg *msg, void *arg)
{
	return NL_OK;
}

static int print_event(struct nl_msg *msg, void *arg)
{
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *tb[NL80211_ATTR_MAX + 1];

	nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
                          
	switch (gnlh->cmd) {
	case NL80211_CMD_NEW_WIPHY:
		printf("wiphy rename: phy #%d to %s\n",
		       nla_get_u32(tb[NL80211_ATTR_WIPHY]),
		       nla_get_string(tb[NL80211_ATTR_WIPHY_NAME]));
		break;
	default:
		printf("unknown event: %d\n", gnlh->cmd);
		break;
	}

	return NL_SKIP;
}

static int listen_events(struct nl80211_state *state,
			 int argc, char **argv)
{
	int mcid, ret;
	struct nl_cb *cb = nl_cb_alloc(debug ? NL_CB_DEBUG : NL_CB_DEFAULT);

	if (!cb) {
		fprintf(stderr, "failed to allocate netlink callbacks\n");
		return -ENOMEM;
	}

	mcid = nl_get_multicast_id(state->nl_handle, "nl80211", "config");
	if (mcid < 0)
		return mcid;

	ret = nl_socket_add_membership(state->nl_handle, mcid);
	if (ret)
		return ret;
	
	/* no sequence checking for multicast messages */
	nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);
	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, print_event, NULL);

	while (1)
		nl_recvmsgs(state->nl_handle, cb);

	nl_cb_put(cb);

	return 0;
}

int main(int argc, char **argv)
{
	struct nl80211_state nlstate;
	int err;
	const char *argv0;

	/* calculate command size including padding */
	cmd_size = abs((long)&__cmd_NULL_1_CIB_NONE_0
	             - (long)&__cmd_NULL_0_CIB_NONE_0);
	/* strip off self */
	argc--;
	argv0 = *argv++;

	if (argc > 0 && strcmp(*argv, "--debug") == 0) {
		debug = 1;
		argc--;
		argv++;
	}

	if (argc > 0 && strcmp(*argv, "--version") == 0) {
		version();
		return 0;
	}

	if (argc == 0 || strcmp(*argv, "help") == 0) {
		usage(argv0);
		return 0;
	}

	err = nl80211_init(&nlstate);
	if (err)
		return 1;

	if (strcmp(*argv, "event") == 0) {
		err = listen_events(&nlstate, argc, argv);
	} else if (strcmp(*argv, "dev") == 0) {
		argc--;
		argv++;
		err = handle_cmd(&nlstate, CIB_NETDEV, argc, argv);
	} else if (strcmp(*argv, "phy") == 0) {
		argc--;
		argv++;
		err = handle_cmd(&nlstate, CIB_PHY, argc, argv);
	} else
		err = handle_cmd(&nlstate, CIB_NONE, argc, argv);

	if (err == 1)
		usage(argv0);
	if (err < 0)
		fprintf(stderr, "command failed: %s (%d)\n", strerror(-err), err);

	nl80211_cleanup(&nlstate);

	return err;
}
