/*
 * netifd - network interface daemon
 * Copyright (C) 2021 Pavel Šimerda <code@simerda.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "netifd.h"
#include "device.h"
#include "interface.h"
#include "system.h"

enum {
	TEAM_ATTR_IFNAME,
	__TEAM_ATTR_MAX
};

static const struct blobmsg_policy team_attrs[__TEAM_ATTR_MAX] = {
	[TEAM_ATTR_IFNAME] = { "ifname", BLOBMSG_TYPE_ARRAY },
};

static const struct uci_blob_param_info team_attr_info[__TEAM_ATTR_MAX] = {
	[TEAM_ATTR_IFNAME] = { .type = BLOBMSG_TYPE_STRING },
};

static const struct uci_blob_param_list team_attr_list = {
	.n_params = __TEAM_ATTR_MAX,
	.params = team_attrs,
	.info = team_attr_info,

	.n_next = 1,
	.next = { &device_attr_list },
};

struct team_device {
	struct device dev;
	device_state_cb set_state;

	struct blob_attr *config_data;
	struct blob_attr *ifnames;

	char start_cmd[64];
	char kill_cmd[64];
	char check_cmd[64];
};

static int
run_cmd(const char *cmd)
{
	printf("TEAM: cmd: %s\n", cmd);
	return system(cmd);
}

static int
team_set_state(struct device *dev, bool up)
{
	struct team_device *teamdev = container_of(dev, struct team_device, dev);

	if (up) {
		struct blob_attr *cur;
		int rem;
		int i;

		run_cmd(teamdev->start_cmd);
		for (i = 0; i < 10; i++) {
			if (!run_cmd(teamdev->check_cmd))
				break;
			sleep(1);
		}

		if (teamdev->ifnames) {
			char add_cmd[64];

			blobmsg_for_each_attr(cur, teamdev->ifnames, rem) {
				snprintf(add_cmd, sizeof add_cmd,
					"teamdctl %s port add %s",
					dev->ifname,
					blobmsg_get_string(cur));
				run_cmd(add_cmd);
			}
		}
		teamdev->set_state(dev, up);
	} else {
		run_cmd(teamdev->kill_cmd);
	}
	return 0;
}

static enum dev_change_type
team_reload(struct device *dev, struct blob_attr *attr)
{
	struct team_device *teamdev = container_of(dev, struct team_device, dev);
	struct blob_attr *tb_tm[__TEAM_ATTR_MAX];

	attr = blob_memdup(attr);

	blobmsg_parse(team_attrs, __TEAM_ATTR_MAX, tb_tm, blob_data(attr), blob_len(attr));
	teamdev->ifnames = tb_tm[TEAM_ATTR_IFNAME];

	if (!run_cmd(teamdev->check_cmd)) {
		// TODO: More fine-grained reconfiguration
		team_set_state(dev, false);
		team_set_state(dev, true);
	}

	free(teamdev->config_data);
	teamdev->config_data = attr;
	return DEV_CONFIG_APPLIED;
}

static struct device *
team_create(const char *name, struct device_type *devtype,
	struct blob_attr *attr)
{
	struct team_device *teamdev;
	struct device *dev = NULL;

	teamdev = calloc(1, sizeof(*teamdev));
	if (!teamdev)
		return NULL;
	dev = &teamdev->dev;

	if (device_init(dev, devtype, name) < 0) {
		device_cleanup(dev);
		free(teamdev);
		return NULL;
	}

	snprintf(teamdev->start_cmd, sizeof teamdev->start_cmd,
		"teamd -t %s -c '{ \"runner\": { \"name\": \"lacp\" } }' -d", dev->ifname);
	snprintf(teamdev->kill_cmd, sizeof teamdev->kill_cmd,
		"teamd -t %s -k", dev->ifname);
	snprintf(teamdev->check_cmd, sizeof teamdev->check_cmd,
		"teamd -t %s -e", dev->ifname);

	teamdev->set_state = dev->set_state;
	dev->set_state = team_set_state;

	device_set_present(dev, true);
	team_reload(dev, attr);

	return dev;
}

static void
team_free(struct device *dev)
{
	struct team_device *teamdev = container_of(dev, struct team_device, dev);

	free(teamdev->config_data);
	free(teamdev);
}

static struct device_type team_device_type = {
	.name = "team",
	.config_params = &team_attr_list,

	.name_prefix = "tm",

	.create = team_create,
	.reload = team_reload,
	.free = team_free,
};

static void __init team_device_type_init(void)
{
	device_type_add(&team_device_type);
}
