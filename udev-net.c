/*
 * Copyright (c) 2024 Future Crew LLC
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "udev-global.h"

#include <sys/socket.h>
#include <sys/stat.h>
#if defined(__OpenBSD__)
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#define LLINDEX(s) ((s)->sdl_index)
#else
#include <net/ethernet.h>
#endif
#include <net/if.h>
#include <net/if_types.h>
#ifdef HAVE_NET_IF_DL_H
#include <net/if_dl.h>
#endif

#include <ifaddrs.h>
#include <string.h>

#ifndef AF_LINK
#define	AF_LINK	AF_PACKET
#endif

int
udev_net_enumerate(struct udev_enumerate *ue)
{
	char syspath[IFNAMSIZ + 5] = "/net/";
	struct ifaddrs *ifap, *ifa;
	struct if_data *ifi;
	int ret = 0;

	if (getifaddrs(&ifap) != 0)
		return (-1);

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL ||
		    ifa->ifa_addr->sa_family != AF_LINK)
			continue;
		ifi = (struct if_data *)ifa->ifa_data;
		if (ifi && ifi->ifi_type != IFT_ETHER)
			continue;
		strlcpy(syspath + 5, ifa->ifa_name, IFNAMSIZ);
		if ((ret = udev_enumerate_add_device(ue, syspath)) < 0)
			break;
	}

	freeifaddrs(ifap);

	return (ret);
}

struct devret
udev_net_monitor(struct udev *udev, struct udev_filter_head filters,
    struct udev_list *cur, struct udev_list *prev)
{
	struct devret ret;
	int found;
	struct udev_list_entry *ce, *pe;
	char syspath[IFNAMSIZ + 5] = "/net/";
	struct ifaddrs *ifap, *ifa;

	ret.action = UD_ACTION_NONE;

	if (getifaddrs(&ifap) != 0)
		return (ret);

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL ||
		    ifa->ifa_addr->sa_family != AF_LINK)
			continue;
		strlcpy(syspath + 5, ifa->ifa_name, IFNAMSIZ);
		if (udev_list_insert(cur, syspath, NULL) == -1)
			continue;
	}

	freeifaddrs(ifap);

	/* attach */
	udev_list_entry_foreach(ce, udev_list_entry_get_first(cur)) {
		found = 0;
		if (!_udev_list_entry_get_name(ce))
			continue;
		if (udev_list_member(prev, _udev_list_entry_get_name(ce), NULL))
			found = 1;
		if (!found && udev_filter_match(udev, &filters, _udev_list_entry_get_name(ce))) {
			ret.action = UD_ACTION_ADD;
			ret.syspath = _udev_list_entry_get_name(ce);
			udev_list_insert(prev, udev_list_entry_get_name(ce), NULL);
		}
	}

	/* detach */
	udev_list_entry_foreach(pe, udev_list_entry_get_first(prev)) {
		found = 0;
		if (!_udev_list_entry_get_name(pe))
			continue;
		if (udev_list_member(cur, _udev_list_entry_get_name(pe), NULL))
			found = 1;
		if (!found && udev_filter_match(udev, &filters, _udev_list_entry_get_name(pe))) {
			ret.action = UD_ACTION_REMOVE;
			ret.syspath = _udev_list_entry_get_name(pe);
			udev_list_remove(prev, udev_list_entry_get_name(pe), NULL);
		}
	}

	return (ret);
}

void
create_net_handler(struct udev_device *ud)
{
	struct udev_list *props, *attrs;
	const char *ifname;
#ifdef HAVE_NET_IF_DL_H
	struct ifaddrs *ifap, *ifa;
#else
	unsigned int ifindex;
#endif

	ifname = _udev_device_get_sysname(ud);
	if (ifname == NULL)
		return;

	props = udev_device_get_properties_list(ud);
	attrs = udev_device_get_sysattr_list(ud);

	udev_list_insert(props, "INTERFACE", ifname);

#ifdef HAVE_NET_IF_DL_H
	if (getifaddrs(&ifap) != 0)
		return;
	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		struct sockaddr_dl *foo;
		uint8_t *lladdr;
		if (ifa->ifa_addr == NULL ||
		    ifa->ifa_addr->sa_family != AF_LINK)
			continue;
		if (strncmp(ifname, ifa->ifa_name, IFNAMSIZ) != 0)
			continue;
		foo = (struct sockaddr_dl *)ifa->ifa_addr;
		if (LLINDEX(foo) != 0) {
			udev_list_insertf(props, "IFINDEX", "%u", LLINDEX(foo));
			udev_list_insertf(attrs, "ifindex", "%u", LLINDEX(foo));
		}
		udev_list_insertf(attrs, "addr_len", "%u", foo->sdl_alen);
		if (foo->sdl_alen == ETHER_ADDR_LEN) {
			lladdr = (uint8_t *)LLADDR(foo);
			udev_list_insertf(attrs, "address",
			   "%02x:%02x:%02x:%02x:%02x:%02x",
			   lladdr[0], lladdr[1], lladdr[2],
			   lladdr[3], lladdr[4], lladdr[5]);
		}
		break;
	}
	freeifaddrs(ifap);
#else
	ifindex = if_nametoindex(ifname);
	if (ifindex != 0) {
		udev_list_insertf(props, "IFINDEX", "%u", ifindex);
		udev_list_insertf(attrs, "ifindex", "%u", ifindex);
	}
#endif
}
