/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Configuration file parser/reader. Place into the dynamic
 *              data structure representation the conf file representing
 *              the loadbalanced server pool.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "check_parser.h"
#include "check_data.h"
#include "check_api.h"
#include "global_data.h"
#include "global_parser.h"
#include "main.h"
#include "logger.h"
#include "parser.h"
#include "utils.h"
#include "ipwrapper.h"
#if defined _WITH_VRRP_
#include "vrrp_parser.h"
#endif
#if defined _WITH_BFD_
#include "bfd_parser.h"
#endif
#include "libipvs.h"
#include "track_file.h"

/* List of valid schedulers */
static const char *lvs_schedulers[] =
	{"rr", "wrr", "lc", "wlc", "lblc", "sh", "mh", "dh", "fo", "ovf", "lblcr", "sed", "nq", "twos", NULL};

virtual_server_t *current_vs;
real_server_t *current_rs;
virtual_server_group_t *current_vsg;


/* SSL handlers */
static void
ssl_handler(const vector_t *strvec)
{
	if (!strvec)
		return;

	if (check_data->ssl) {
		free_ssl();
		report_config_error(CONFIG_GENERAL_ERROR, "SSL context already specified - replacing");
	}
	check_data->ssl = alloc_ssl();
}

static void
handle_ssl_file(const vector_t *strvec, const char **file_name, const char *type)
{
	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL %s missing", type);
		return;
	}

	if (*file_name) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL %s already specified - replacing", type);
		FREE_CONST(*file_name);
	}
	*file_name = set_value(strvec);
}

static void
sslpass_handler(const vector_t *strvec)
{
	handle_ssl_file(strvec, &check_data->ssl->password, "password");
}
static void
sslca_handler(const vector_t *strvec)
{
	handle_ssl_file(strvec, &check_data->ssl->cafile, "cafile");
}
static void
sslcert_handler(const vector_t *strvec)
{
	handle_ssl_file(strvec, &check_data->ssl->certfile, "certfile");
}
static void
sslkey_handler(const vector_t *strvec)
{
	handle_ssl_file(strvec, &check_data->ssl->keyfile, "keyfile");
}

/* Virtual Servers handlers */
static void
vsg_handler(const vector_t *strvec)
{
	vector_t *my_strvec;
	ptr_hack_t word;
	unsigned i;

	if (!strvec)
		return;

	/* Fetch queued vsg */
	current_vsg = alloc_vsg(strvec_slot(strvec, 1));

	/* alloc_value_block() does not expect a name after the first word,
	 * so contruct another vector omitting the VSG name */
	my_strvec = vector_alloc();
	vector_alloc_slot(my_strvec);
	vector_set_slot(my_strvec, 0);
	for (i = 2; i < vector_size(strvec); i++) {
		vector_alloc_slot(my_strvec);
		word.cp = strvec_slot(strvec, i);
		vector_set_slot(my_strvec, word.p);
	}

	alloc_value_block(alloc_vsg_entry, my_strvec);
	vector_free(my_strvec);

	/* Ensure the virtual server group has some configuration */
	if (list_empty(&current_vsg->vfwmark) && list_empty(&current_vsg->addr_range)) {
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server group %s has no entries - removing"
							, current_vsg->gname);
		free_vsg(current_vsg);
		return;
	} else if (current_vsg->have_ipv4 && current_vsg->have_ipv6 && current_vsg->fwmark_no_family) {
/* The error here is fwmark_no_family && all rs tunnelled - but we only know that later */
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server group %s cannot have IPv4, IPv6"
							  " and fwmark without family - removing"
							, current_vsg->gname);
		free_vsg(current_vsg);
		return;
	}

	list_add_tail(&current_vsg->e_list, &check_data->vs_group);
	current_vsg = NULL;
}

static void
vs_handler(const vector_t *strvec)
{
	global_data->have_checker_config = true;

	/* If we are not in the checker process, we don't want any more info */
	if (!strvec)
		return;

	current_vs = alloc_vs(strvec_slot(strvec, 1), vector_size(strvec) >= 3 ? strvec_slot(strvec, 2) : NULL);
}
static void
vs_end_handler(void)
{
	real_server_t *rs;
	bool mixed_af;

	if (list_empty(&current_vs->rs)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Virtual server %s has no real servers - ignoring", FMT_VS(current_vs));
		free_vs(current_vs);
		return;
	}

	/* If the real (sorry) server uses tunnel forwarding, the address family
	 * does not have to match the address family of the virtual server */
	if (current_vs->s_svr
#if HAVE_DECL_IPVS_DEST_ATTR_ADDR_FAMILY
		      && current_vs->s_svr->forwarding_method != IP_VS_CONN_F_TUNNEL
#endif
									    )
	{
		if (current_vs->af == AF_UNSPEC)
			current_vs->af = current_vs->s_svr->addr.ss_family;
		else if (current_vs->af != current_vs->s_svr->addr.ss_family) {
			report_config_error(CONFIG_GENERAL_ERROR, "Address family of virtual server and sorry server %s don't match - skipping sorry server.", inet_sockaddrtos(&current_vs->s_svr->addr));
			FREE(current_vs->s_svr);
			current_vs->s_svr = NULL;
		}
	}

	if (current_vs->af == AF_UNSPEC && !current_vs->vsgname) {
		/* This only occurs if the virtual server uses a fwmark, all the
		 * real/sorry servers are tunnelled, and the address family has not
		 * been specified.
		 *
		 * Maintain backward compatibility. Prior to the commit following 17fa4a3c
		 * the address family of the virtual server was set from any of its
		 * real or sorry servers, even if they were tunnelled. However, all the real
		 * and sorry servers had to be the same address family, even if tunnelled,
		 * so only set the address family from the tunnelled real/sorry servers
		 * if all the real/sorry servers are of the same address family. */
		mixed_af = false;

		if (current_vs->s_svr)
			current_vs->af = current_vs->s_svr->addr.ss_family;

		list_for_each_entry(rs, &current_vs->rs, e_list) {
			if (current_vs->af == AF_UNSPEC)
				current_vs->af = rs->addr.ss_family;
			else if (current_vs->af != rs->addr.ss_family) {
				mixed_af = true;
				break;
			}
		}

		if (mixed_af || current_vs->af == AF_UNSPEC) {
			/* We have a mixture of IPv4 and IPv6 tunnelled real/sorry servers.
			 * Default to IPv4. */
			current_vs->af = AF_INET;
		}
	}

	list_add_tail(&current_vs->e_list, &check_data->vs);
}
static void
ip_family_handler(const vector_t *strvec)
{
	uint16_t af;

	if (!strcmp(strvec_slot(strvec, 1), "inet"))
		af = AF_INET;
	else if (!strcmp(strvec_slot(strvec, 1), "inet6")) {
#ifndef LIBIPVS_USE_NL
		report_config_error(CONFIG_GENERAL_ERROR, "IPVS with IPv6 is not supported by this build");
		skip_block(false);
		free_vs(current_vs);
		current_vs = NULL;
		return;
#endif

		/* coverity[unreachable] */
		af = AF_INET6;
	}
	else {
		report_config_error(CONFIG_GENERAL_ERROR, "unknown address family %s", strvec_slot(strvec, 1));
		return;
	}

	if (current_vs->af != AF_UNSPEC &&
	    af != current_vs->af) {
		report_config_error(CONFIG_GENERAL_ERROR, "Virtual server specified family %s conflicts with server family", strvec_slot(strvec, 1));
		return;
	}

	current_vs->af = af;
}
static void
vs_co_timeout_handler(const vector_t *strvec)
{
	unsigned long timer;

	if (!read_timer(strvec, 1, &timer, 1, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server connect_timeout %s invalid - ignoring", strvec_slot(strvec, 1));
		return;
	}
	current_vs->connection_to = timer;
}
static void
vs_delay_handler(const vector_t *strvec)
{
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 1, 0, true))
		current_vs->delay_loop = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server delay loop '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
vs_delay_before_retry_handler(const vector_t *strvec)
{
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		current_vs->delay_before_retry = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server delay before retry '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
vs_retry_handler(const vector_t *strvec)
{
	unsigned retry;

	if (!read_unsigned_strvec(strvec, 1, &retry, 1, UINT32_MAX, false)) {
		report_config_error(CONFIG_GENERAL_ERROR, "retry value invalid - %s", strvec_slot(strvec, 1));
		return;
	}
	current_vs->retry = retry;
}
static void
vs_warmup_handler(const vector_t *strvec)
{
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		current_vs->warmup = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server warmup '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
lbalgo_handler(const vector_t *strvec)
{
	const char *str = strvec_slot(strvec, 1);
	int i;

	/* Check valid scheduler name */
	for (i = 0; lvs_schedulers[i] && strcmp(str, lvs_schedulers[i]); i++);

	if (!lvs_schedulers[i] || strlen(str) >= sizeof(current_vs->sched)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Invalid lvs_scheduler '%s' - ignoring", strvec_slot(strvec, 1));
		return;
	}

	strcpy(current_vs->sched, str);
}

static void
lbflags_handler(const vector_t *strvec)
{
	const char *str = strvec_slot(strvec, 0);

	if (!strcmp(str, "ops"))
		current_vs->flags |= IP_VS_SVC_F_ONEPACKET;
#ifdef IP_VS_SVC_F_SCHED1		/* From Linux 3.11 */
	else if (!strcmp(str, "flag-1"))
		current_vs->flags |= IP_VS_SVC_F_SCHED1;
	else if (!strcmp(str, "flag-2"))
		current_vs->flags |= IP_VS_SVC_F_SCHED2;
	else if (!strcmp(str, "flag-3"))
		current_vs->flags |= IP_VS_SVC_F_SCHED3;
	else if (!strcmp(current_vs->sched , "sh") )
	{
		/* sh-port and sh-fallback flags are relevant for sh scheduler only */
		if (!strcmp(str, "sh-port")  )
			current_vs->flags |= IP_VS_SVC_F_SCHED_SH_PORT;
		if (!strcmp(str, "sh-fallback"))
			current_vs->flags |= IP_VS_SVC_F_SCHED_SH_FALLBACK;
	}
	else if (!strcmp(current_vs->sched , "mh") )
	{
		/* mh-port and mh-fallback flags are relevant for mh scheduler only */
		if (!strcmp(str, "mh-port")  )
			current_vs->flags |= IP_VS_SVC_F_SCHED_MH_PORT;
		if (!strcmp(str, "mh-fallback"))
			current_vs->flags |= IP_VS_SVC_F_SCHED_MH_FALLBACK;
	}
	else
		report_config_error(CONFIG_GENERAL_ERROR, "%s only applies to sh scheduler - ignoring", str);
#endif
}

static void
svr_forwarding_handler(real_server_t *rs, const vector_t *strvec, const char *s_type)
{
	const char *str = strvec_slot(strvec, 1);
#ifdef _HAVE_IPVS_TUN_TYPE_
	size_t i;
	int tun_type = IP_VS_CONN_F_TUNNEL_TYPE_IPIP;
	unsigned port = 0;
#ifdef _HAVE_IPVS_TUN_CSUM_
	int csum = IP_VS_TUNNEL_ENCAP_FLAG_NOCSUM;
#endif
#endif

	if (!strcmp(str, "NAT"))
		rs->forwarding_method = IP_VS_CONN_F_MASQ;
	else if (!strcmp(str, "DR"))
		rs->forwarding_method = IP_VS_CONN_F_DROUTE;
	else if (!strcmp(str, "TUN"))
		rs->forwarding_method = IP_VS_CONN_F_TUNNEL;
	else {
		report_config_error(CONFIG_GENERAL_ERROR, "PARSER : unknown [%s] routing method for %s server.", str, s_type);
		return;
	}

#ifdef _HAVE_IPVS_TUN_TYPE_
	for (i = 2; i < vector_size(strvec); i++) {
		if (!strcmp(strvec_slot(strvec, i), "type")) {
			if (vector_size(strvec) == i + 1) {
				report_config_error(CONFIG_GENERAL_ERROR, "Missing tunnel type for %s server.", s_type);
				return;
			}
			if (!strcmp(strvec_slot(strvec, i + 1), "ipip"))
				tun_type = IP_VS_CONN_F_TUNNEL_TYPE_IPIP;
			else if (!strcmp(strvec_slot(strvec, i + 1), "gue"))
				tun_type = IP_VS_CONN_F_TUNNEL_TYPE_GUE;
#ifdef _HAVE_IPVS_TUN_GRE_
			else if (!strcmp(strvec_slot(strvec, i + 1), "gre"))
				tun_type = IP_VS_CONN_F_TUNNEL_TYPE_GRE;
#endif
			else {
				report_config_error(CONFIG_GENERAL_ERROR, "Unknown tunnel type %s for %s server.", strvec_slot(strvec, i + 1), s_type);
				return;
			}
			i++;
		} else if (!strcmp(strvec_slot(strvec, i), "port")) {
			if (vector_size(strvec) == i + 1) {
				report_config_error(CONFIG_GENERAL_ERROR, "Missing port for %s server gue tunnel.", s_type);
				return;
			}
			if (!read_unsigned_strvec(strvec, i + 1, &port, 1, 65535, false)) {
				report_config_error(CONFIG_GENERAL_ERROR, "Invalid gue tunnel port %s for %s server.", strvec_slot(strvec, i + 1), s_type);
				return;
			}
			i++;
		}
#ifdef _HAVE_IPVS_TUN_CSUM_
		else if (!strcmp(strvec_slot(strvec, i), "nocsum"))
			csum = IP_VS_TUNNEL_ENCAP_FLAG_NOCSUM;
		else if (!strcmp(strvec_slot(strvec, i), "csum"))
			csum = IP_VS_TUNNEL_ENCAP_FLAG_CSUM;
		else if (!strcmp(strvec_slot(strvec, i), "remcsum"))
			csum = IP_VS_TUNNEL_ENCAP_FLAG_REMCSUM;
#endif
		else {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid tunnel option %s for %s server.", strvec_slot(strvec, i), s_type);
			return;
		}
	}

	if ((tun_type == IP_VS_CONN_F_TUNNEL_TYPE_GUE) != (port != 0)) {
		report_config_error(CONFIG_GENERAL_ERROR, "gue tunnels require port, otherwise cannot have port.");
		return;
	}
#ifdef _HAVE_IPVS_TUN_CSUM_
	if (tun_type == IP_VS_CONN_F_TUNNEL_TYPE_IPIP && csum != IP_VS_TUNNEL_ENCAP_FLAG_NOCSUM) {
		report_config_error(CONFIG_GENERAL_ERROR, "ipip tunnels do not support checksum option.");
		return;
	}
#endif
#ifdef _HAVE_IPVS_TUN_GRE_
	if (tun_type == IP_VS_CONN_F_TUNNEL_TYPE_GRE && csum == IP_VS_TUNNEL_ENCAP_FLAG_REMCSUM) {
		report_config_error(CONFIG_GENERAL_ERROR, "gre tunnels do not support remote checksum option.");
		return;
	}
#endif

#ifdef _HAVE_IPVS_TUN_TYPE_
	rs->tun_type = tun_type;
	rs->tun_port = htons(port);
#ifdef _HAVE_IPVS_TUN_CSUM_
	rs->tun_flags = csum;
#endif
#endif
#endif
}

static void
vs_forwarding_handler(const vector_t *strvec)
{
	real_server_t rs = {		 // dummy for setting parameters. Ensure previous values are preserved
#ifdef _HAVE_IPVS_TUN_TYPE_
		     .tun_type = current_vs->tun_type,
		     .tun_port = current_vs->tun_port,
#ifdef _HAVE_IPVS_TUN_CSUM_
		     .tun_flags = current_vs->tun_flags,
#endif
#endif
		     .forwarding_method = current_vs->forwarding_method };

	svr_forwarding_handler(&rs, strvec, "virtual");
	current_vs->forwarding_method = rs.forwarding_method;
#ifdef _HAVE_IPVS_TUN_TYPE_
	current_vs->tun_type = rs.tun_type;
	current_vs->tun_port = rs.tun_port;
#ifdef _HAVE_IPVS_TUN_CSUM_
	current_vs->tun_flags = rs.tun_flags;
#endif
#endif
}

static void
pto_handler(const vector_t *strvec)
{
	unsigned timeout;

	if (vector_size(strvec) < 2) {
		current_vs->persistence_timeout = IPVS_SVC_PERSISTENT_TIMEOUT;
		return;
	}

	if (!read_unsigned_strvec(strvec, 1, &timeout, 1, LVS_MAX_TIMEOUT, false)) {
		report_config_error(CONFIG_GENERAL_ERROR, "persistence_timeout invalid");
		return;
	}

	current_vs->persistence_timeout = (uint32_t)timeout;
}
static void
pengine_handler(const vector_t *strvec)
{
	const char *str = strvec_slot(strvec, 1);
	size_t size = sizeof (current_vs->pe_name);

	if (strlen(str) > size - 1)
		report_config_error(CONFIG_GENERAL_ERROR, "persistence_name too long, truncating - %s", strvec_slot(strvec, 1));
	strncpy(current_vs->pe_name, str, size - 1);
	current_vs->pe_name[size - 1] = '\0';
}
static void
pgr_handler(const vector_t *strvec)
{
	uint16_t af = current_vs->af;
	unsigned granularity;

	if (af == AF_UNSPEC)
		af = strchr(strvec_slot(strvec, 1), '.') ? AF_INET : AF_INET6;

	if (af == AF_INET6) {
		if (!read_unsigned_strvec(strvec, 1, &granularity, 1, 128, false)) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid IPv6 persistence_granularity specified - %s", strvec_slot(strvec, 1));
			return;
		}
		current_vs->persistence_granularity = granularity;
	} else {
		struct addrinfo hints = { .ai_flags = AI_NUMERICHOST, .ai_family = AF_INET };
		struct addrinfo *res;

		if (getaddrinfo(strvec_slot(strvec, 1), NULL, &hints, &res)) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid IPv4 persistence_granularity specified - %s", strvec_slot(strvec, 1));
			return;
		}

		/* It would seem sensible to force a solid netmask, but ipvsadm doesn't
		   and the kernel doesn't require it either. */
#if 0
		/* Ensure the netmask is solid */
		uint32_t haddr = ntohl(addr.s_addr);
		while (!(haddr & 1))
			haddr = (haddr >> 1) | 0x80000000;
		if (haddr != 0xffffffff) {
			report_config_error(CONFIG_GENERAL_ERROR, "IPv4 persistence_granularity netmask is not solid - %s", strvec_slot(strvec, 1));
			return;
		}
#endif

		/* Cast via void * to stop -Wcast-align warning.
		 * Since alignment of struct addrinfo >= alignment of struct sockaddr_in and res->ai_addr is
		 * aligned to a struct addrinfo, it is not a problem.
		 *   e.g. vs->persistence_granularity = ((struct sockaddr_in *)((void *)res->ai_addr))->sin_addr.s_addr;
		 */
		current_vs->persistence_granularity = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
		freeaddrinfo(res);
	}

	if (current_vs->af == AF_UNSPEC)
		current_vs->af = af;

	if (!current_vs->persistence_timeout)
		current_vs->persistence_timeout = IPVS_SVC_PERSISTENT_TIMEOUT;
}
static void
proto_handler(const vector_t *strvec)
{
	const char *str = strvec_slot(strvec, 1);

	if (!strcasecmp(str, "TCP"))
		current_vs->service_type = IPPROTO_TCP;
	else if (!strcasecmp(str, "SCTP"))
		current_vs->service_type = IPPROTO_SCTP;
	else if (!strcasecmp(str, "UDP"))
		current_vs->service_type = IPPROTO_UDP;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Unknown protocol %s - ignoring", str);
}
static void
hasuspend_handler(__attribute__((unused)) const vector_t *strvec)
{
	current_vs->ha_suspend = true;
}

static void
vs_smtp_alert_handler(const vector_t *strvec)
{
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid virtual_server smtp_alert parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	current_vs->smtp_alert = res;
	check_data->num_smtp_alert++;
}

static void
vs_virtualhost_handler(const vector_t *strvec)
{
	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server virtualhost missing");
		return;
	}

	if (!current_vs->virtualhost)
		current_vs->virtualhost = set_value(strvec);
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Duplicate vs virtualhost %s - ignoring", strvec_slot(strvec, 1));
}

#ifdef _WITH_SNMP_CHECKER_
static void
vs_snmp_name_handler(const vector_t *strvec)
{

	if (vector_size(strvec) != 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server snmp_name missing or extra parameters");
		return;
	}

	if (!current_vs->snmp_name)
		current_vs->snmp_name = set_value(strvec);
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Duplicate vs snmp_name %s - ignoring", strvec_slot(strvec, 1));
}
#endif

/* Sorry Servers handlers */
static void
ssvr_handler(const vector_t *strvec)
{
	alloc_ssvr(strvec_slot(strvec, 1), vector_size(strvec) >= 3 ? strvec_slot(strvec, 2) : NULL);
}
static void
ssvri_handler(__attribute__((unused)) const vector_t *strvec)
{
	if (current_vs->s_svr)
		current_vs->s_svr->inhibit = true;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Ignoring sorry_server inhibit used before or without sorry_server");
}
static void
ss_forwarding_handler(const vector_t *strvec)
{
	if (current_vs->s_svr)
		svr_forwarding_handler(current_vs->s_svr, strvec, "sorry");
	else
		report_config_error(CONFIG_GENERAL_ERROR, "sorry_server forwarding used without sorry_server");
}

/* Real Servers handlers */
static void
rs_handler(const vector_t *strvec)
{
	current_rs = alloc_rs(strvec_slot(strvec, 1), vector_size(strvec) >= 3 ? strvec_slot(strvec, 2) : NULL);
}

static void
rs_end_handler(void)
{
	/* For tunnelled forwarding, the address families don't have to be the same, so
	 * long as the kernel supports IPVS_DEST_ATTR_ADDR_FAMILY */
#if HAVE_DECL_IPVS_DEST_ATTR_ADDR_FAMILY
	if (current_rs->forwarding_method != IP_VS_CONN_F_TUNNEL)
#endif
	{
		if (current_vs->af == AF_UNSPEC)
			current_vs->af = current_rs->addr.ss_family;
		else if (current_vs->af != current_rs->addr.ss_family) {
			report_config_error(CONFIG_GENERAL_ERROR, "Address family of virtual server and real server %s don't match - skipping real server.", inet_sockaddrtos(&current_rs->addr));
			FREE(current_rs);
			return;
		}
	}

	list_add_tail(&current_rs->e_list, &current_vs->rs);
	current_vs->rs_cnt++;
}

static void
rs_weight_handler(const vector_t *strvec)
{
	unsigned weight;

	if (!read_unsigned_strvec(strvec, 1, &weight, 0, IPVS_WEIGHT_LIMIT, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Real server weight %s is outside range 0-%d", strvec_slot(strvec, 1), IPVS_WEIGHT_LIMIT);
		return;
	}
	current_rs->effective_weight = weight;
	current_rs->iweight = weight;
}
static void
rs_forwarding_handler(const vector_t *strvec)
{
	svr_forwarding_handler(current_rs, strvec, "real");
}
static void
uthreshold_handler(const vector_t *strvec)
{
	unsigned threshold;

	if (!read_unsigned_strvec(strvec, 1, &threshold, 0, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Invalid real_server uthreshold '%s' - ignoring", strvec_slot(strvec, 1));
		return;
	}
	current_rs->u_threshold = threshold;
}
static void
lthreshold_handler(const vector_t *strvec)
{
	unsigned threshold;

	if (!read_unsigned_strvec(strvec, 1, &threshold, 0, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Invalid real_server lthreshold '%s' - ignoring", strvec_slot(strvec, 1));
		return;
	}
	current_rs->l_threshold = threshold;
}
static void
vs_inhibit_handler(__attribute__((unused)) const vector_t *strvec)
{
	current_vs->inhibit = true;
}
static inline notify_script_t*
set_check_notify_script(__attribute__((unused)) const vector_t *strvec, const char *type)
{
	return notify_script_init(0, type);
}
static void
notify_up_handler(const vector_t *strvec)
{
	if (current_rs->notify_up) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) notify_up script already specified - ignoring %s", current_vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	current_rs->notify_up = set_check_notify_script(strvec, "notify");
}
static void
notify_down_handler(const vector_t *strvec)
{
	if (current_rs->notify_down) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) notify_down script already specified - ignoring %s", current_vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	current_rs->notify_down = set_check_notify_script(strvec, "notify");
}
static void
rs_co_timeout_handler(const vector_t *strvec)
{
	unsigned long timer;

	if (!read_timer(strvec, 1, &timer, 1, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "real server connect_timeout %s invalid - ignoring", strvec_slot(strvec, 1));
		return;
	}
	current_rs->connection_to = timer;
}
static void
rs_delay_handler(const vector_t *strvec)
{
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 1, 0, true))
		current_rs->delay_loop = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "real server delay_loop '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
rs_delay_before_retry_handler(const vector_t *strvec)
{
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		current_rs->delay_before_retry = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "real server delay_before_retry '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
rs_retry_handler(const vector_t *strvec)
{
	unsigned retry;

	if (!read_unsigned_strvec(strvec, 1, &retry, 1, UINT32_MAX, false)) {
		report_config_error(CONFIG_GENERAL_ERROR, "retry value invalid - %s", strvec_slot(strvec, 1));
		return;
	}
	current_rs->retry = (unsigned)retry;
}
static void
rs_warmup_handler(const vector_t *strvec)
{
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		current_rs->warmup = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "real server warmup '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
rs_inhibit_handler(const vector_t *strvec)
{
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid inhibit_on_failure parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	current_rs->inhibit = res;
}
static void
rs_alpha_handler(const vector_t *strvec)
{
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid alpha parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	current_rs->alpha = res;
}
static void
rs_smtp_alert_handler(const vector_t *strvec)
{
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid real_server smtp_alert parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	current_rs->smtp_alert = res;
	check_data->num_smtp_alert++;
}
static void
rs_virtualhost_handler(const vector_t *strvec)
{
	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "real server virtualhost missing");
		return;
	}

	if (!current_rs->virtualhost)
		current_rs->virtualhost = set_value(strvec);
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Duplicate rs virtualhost %s - ignoring", strvec_slot(strvec, 1));
}

#ifdef _WITH_SNMP_CHECKER_
static void
rs_snmp_name_handler(const vector_t *strvec)
{
	if (vector_size(strvec) != 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "real server snmp_name missing or extra parameters");
		return;
	}

	if (!current_rs->snmp_name)
		current_rs->snmp_name = set_value(strvec);
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Duplicate rs snmp_name %s - ignoring", strvec_slot(strvec, 1));
}
#endif

static void
vs_alpha_handler(__attribute__((unused)) const vector_t *strvec)
{
	current_vs->alpha = true;
}
static void
omega_handler(__attribute__((unused)) const vector_t *strvec)
{
	current_vs->omega = true;
}
static void
quorum_up_handler(const vector_t *strvec)
{
	if (current_vs->notify_quorum_up) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) quorum_up script already specified - ignoring %s", current_vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	current_vs->notify_quorum_up = set_check_notify_script(strvec, "quorum");
}
static void
quorum_down_handler(const vector_t *strvec)
{
	if (current_vs->notify_quorum_down) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) quorum_down script already specified - ignoring %s", current_vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	current_vs->notify_quorum_down = set_check_notify_script(strvec, "quorum");
}
static void
quorum_handler(const vector_t *strvec)
{
	unsigned quorum;

	if (!read_unsigned_strvec(strvec, 1, &quorum, 1, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Quorum %s must be in [1, %u]. Setting to 1.", strvec_slot(strvec, 1), UINT_MAX);
		quorum = 1;
	}

	current_vs->quorum = quorum;
}
static void
hysteresis_handler(const vector_t *strvec)
{
	unsigned hysteresis;

	if (!read_unsigned_strvec(strvec, 1, &hysteresis, 0, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Hysteresis %s must be in [0, %u] - ignoring", strvec_slot(strvec, 1), UINT_MAX);
		return;
	}

	current_vs->hysteresis = hysteresis;
}
static void
vs_weight_handler(const vector_t *strvec)
{
	unsigned weight;

	if (!read_unsigned_strvec(strvec, 1, &weight, 1, IPVS_WEIGHT_LIMIT, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Virtual server weight %s is outside range 1-%d", strvec_slot(strvec, 1), IPVS_WEIGHT_LIMIT);
		return;
	}
	current_vs->weight = weight;
}

void
init_check_keywords(bool active)
{
	vpp_t check_ptr;

	/* SSL mapping */
	install_keyword_root("SSL", &ssl_handler, active, VPP &check_data->ssl);
	install_keyword("password", &sslpass_handler);
	install_keyword("ca", &sslca_handler);
	install_keyword("certificate", &sslcert_handler);
	install_keyword("key", &sslkey_handler);

	/* Virtual server mapping */
	install_keyword_root("virtual_server_group", &vsg_handler, active, NULL);
	install_keyword_root("virtual_server", &vs_handler, active, VPP &current_vs);
	install_level_end_handler(&vs_end_handler);
	install_keyword("ip_family", &ip_family_handler);
	install_keyword("retry", &vs_retry_handler);
	install_keyword("delay_before_retry", &vs_delay_before_retry_handler);
	install_keyword("warmup", &vs_warmup_handler);
	install_keyword("connect_timeout", &vs_co_timeout_handler);
	install_keyword("delay_loop", &vs_delay_handler);
	install_keyword("inhibit_on_failure", &vs_inhibit_handler);
	install_keyword("lb_algo", &lbalgo_handler);
	install_keyword("lvs_sched", &lbalgo_handler);

	install_keyword("hashed", &lbflags_handler);
	install_keyword("ops", &lbflags_handler);
#ifdef IP_VS_SVC_F_SCHED1
	install_keyword("flag-1", &lbflags_handler);
	install_keyword("flag-2", &lbflags_handler);
	install_keyword("flag-3", &lbflags_handler);
	install_keyword("sh-port", &lbflags_handler);
	install_keyword("sh-fallback", &lbflags_handler);
	install_keyword("mh-port", &lbflags_handler);
	install_keyword("mh-fallback", &lbflags_handler);
#endif
	install_keyword("lb_kind", &vs_forwarding_handler);
	install_keyword("lvs_method", &vs_forwarding_handler);
	install_keyword("persistence_engine", &pengine_handler);
	install_keyword("persistence_timeout", &pto_handler);
	install_keyword("persistence_granularity", &pgr_handler);
	install_keyword("protocol", &proto_handler);
	install_keyword("ha_suspend", &hasuspend_handler);
	install_keyword("smtp_alert", &vs_smtp_alert_handler);
	install_keyword("virtualhost", &vs_virtualhost_handler);
#ifdef _WITH_SNMP_CHECKER_
	install_keyword("snmp_name", &vs_snmp_name_handler);
#endif

	/* Pool regression detection and handling. */
	install_keyword("alpha", &vs_alpha_handler);
	install_keyword("omega", &omega_handler);
	install_keyword("quorum_up", &quorum_up_handler);
	install_keyword("quorum_down", &quorum_down_handler);
	install_keyword("quorum", &quorum_handler);
	install_keyword("hysteresis", &hysteresis_handler);
	install_keyword("weight", &vs_weight_handler);

	/* Real server mapping */
	install_keyword("sorry_server", &ssvr_handler);
	install_keyword("sorry_server_inhibit", &ssvri_handler);
	install_keyword("sorry_server_lvs_method", &ss_forwarding_handler);
	install_keyword("real_server", &rs_handler);
	check_ptr = install_sublevel(VPP &current_rs);
	install_keyword("weight", &rs_weight_handler);
	install_keyword("lvs_method", &rs_forwarding_handler);
	install_keyword("uthreshold", &uthreshold_handler);
	install_keyword("lthreshold", &lthreshold_handler);
	install_keyword("inhibit_on_failure", &rs_inhibit_handler);
	install_keyword("notify_up", &notify_up_handler);
	install_keyword("notify_down", &notify_down_handler);
	install_keyword("alpha", &rs_alpha_handler);
	install_keyword("retry", &rs_retry_handler);
	install_keyword("delay_before_retry", &rs_delay_before_retry_handler);
	install_keyword("warmup", &rs_warmup_handler);
	install_keyword("connect_timeout", &rs_co_timeout_handler);
	install_keyword("delay_loop", &rs_delay_handler);
	install_keyword("smtp_alert", &rs_smtp_alert_handler);
	install_keyword("virtualhost", &rs_virtualhost_handler);
#ifdef _WITH_SNMP_CHECKER_
	install_keyword("snmp_name", &rs_snmp_name_handler);
#endif

	install_level_end_handler(&rs_end_handler);

	/* Checkers mapping */
	install_checkers_keyword();
	install_sublevel_end(check_ptr);
}

const vector_t *
check_init_keywords(void)
{
	/* global definitions mapping */
	init_global_keywords(reload);

	init_check_keywords(true);
#ifdef _WITH_VRRP_
	init_vrrp_keywords(false);
#endif
#ifdef _WITH_BFD_
	init_bfd_keywords(true);
#endif
	add_track_file_keywords(true);

	return keywords;
}
