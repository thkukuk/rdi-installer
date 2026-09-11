// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "basics.h"
#include "ifcfg.h"
#include "rdii-networkd.h"
#include "logger.h"
#include "ip.h"

/* Configuration */
#define NETDEV_PREFIX  "62-ifcfg"
#define VLAN_PREFIX    "64-ifcfg-vlan"
#define IFCFG_PREFIX   "66-ifcfg-dev"

/* Helper to trim whitespace */
static char *
trim_whitespace(char *str)
{
  char *end;

  if (isempty(str))
    return NULL;

  while(isspace(*str))
    str++;
  if(*str == '\0')
    return str;
  end = str + strlen(str) - 1;

  while(end > str && isspace(*end))
    end--;

  *(end+1) = '\0';
  return str;
}


static int
write_vlan_file(const char *output_dir, const char *interface, int vlanid)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;
  int r;

  if (asprintf(&filepath, "%s/%s-%s.network",
	       output_dir, VLAN_PREFIX, interface) < 0)
    return -ENOMEM;

  MSG_INFO("Creating vlan config: %s for interface '%s.%d'", filepath,
          interface, vlanid);

  if (access(filepath, F_OK) != 0)
    { // file does not exist
      fp = fopen(filepath, "w");
      if (!fp)
	{
	  r = -errno;
          MSG_ERROR("Failed to open network file '%s' for writing: %s",
                 filepath, strerror(-r));
	  return r;
	}

      fprintf(fp, "[Match]\n");
      fprintf(fp, "Name=%s\n", interface);
      fprintf(fp, "Type=ether\n");

      fprintf(fp, "\n[Network]\n");
      fprintf(fp, "Description=The unconfigured physical ethernet device\n");
      fprintf(fp, "VLAN=vlan%04d\n", vlanid);
      fprintf(fp, "# 'tagged only' setup\n");
      fprintf(fp, "LinkLocalAddressing=no\n");
      fprintf(fp, "LLDP=no\n");
      fprintf(fp, "EmitLLDP=no\n");
      fprintf(fp, "IPv6AcceptRA=no\n");
      fprintf(fp, "IPv6SendRA=no\n");
    }
  else
    {
      fp = fopen(filepath, "a");
      if (!fp)
	{
	  r = -errno;
          MSG_ERROR("Failed to open network file '%s' for appending: %s",
                 filepath, strerror(-r));
	  return r;
	}
      fprintf(fp, "VLAN=vlan%04d\n", vlanid);
    }

  return 0;
}

static const char*
map_ifcfg_to_networkd(const char *input)
{
  static const kv_map_t mappings[] =
    {
      { "dhcp",       "yes" },
      { "dhcp4",      "ipv4" },
      { "dhcp6",      "ipv6" },
      { NULL,         NULL }
    };

  return map_lookup(mappings, input, "{dhcp|dhcp4|dhcp6}");
}

/* Parses a single ifcfg string */
int
parse_ifcfg_arg(const char *output_dir, int nr, const char *arg)
{
  ip_t cfg = {0}; // Initialize all pointers to NULL
  _cleanup_free_ char *copy_to_free = strdup(arg); // to free everything
  cfg.netmask = -1; // ifcfg has no separate netmask field; CIDR is embedded in the address itself
  char *str = copy_to_free; // Pointer for strsep
  char *token;
  /* vlan */
  int vlanid = 0;
  /* dhcp */
  bool rfc2132 = false;
  int r;

  MSG_DEBUG("parse_ifcfg_arg=%d - '%s'", nr, arg);

  // Syntax: <interface>=<str>

  r = extract_word(&str, "=", true, &token);
  if (r < 0)
    return return_syntax_error(nr, arg, -EINVAL);

  if (isempty(token) || isempty(str))
    return return_syntax_error(nr, arg, -ENOENT);

  MSG_DEBUG("Interface - Config: '%s' - '%s'",
	    token, str);

  if (!isempty(token))
    {
      char *vlanid_str = strrchr(token, '.');
      if (vlanid_str != NULL)
	{
	  char *ep;
	  long l;

	  *vlanid_str++ = '\0';

	  l = strtol(vlanid_str, &ep, 10);
	  // valid: 1 <= VLAN ID <= 4095
	  if (errno == ERANGE || l < 1 || l > 4095 ||
	      vlanid_str == ep || *ep != '\0')
	    {
	      MSG_ERROR("Invalid VLAN interface: %s", token);
	      return -EINVAL;
	    }
	  vlanid = l;

          char *vlan_name;
	  if (asprintf(&vlan_name, "vlan%04d", vlanid) < 0)
	    return -ENOMEM;

	  r = register_vlan_netdev(vlanid, vlan_name, NETDEV_PREFIX);
	  if (r < 0)
	    return r;
	}
    }

  // token is only the interace, possible vlan ids got removed
  cfg.interface = token;

  // Format: IP_LIST,GATEWAY_LIST,NAMESERVER_LIST,DOMAINSEARCH_LIST
  char *ip_list = trim_whitespace(strsep(&str, ","));
  char *gw_list = trim_whitespace(strsep(&str, ","));
  char *dns_list = trim_whitespace(strsep(&str, ","));
  char *domains = trim_whitespace(strsep(&str, ","));

  if (!isempty(ip_list) && strneq(ip_list, "dhcp", 4))
    {
      cfg.autoconf = ip_list;
      cfg.autoconf_networkd = map_ifcfg_to_networkd(cfg.autoconf);
      if (!isempty(gw_list) && streq(gw_list, "rfc2132"))
        rfc2132 = true;
    }
  else
    {
      cfg.client_ip = ip_list;
      r = append_route_settings(gw_list, NULL, &cfg);
      if (r < 0)
            return r;
      cfg.dns1 = dns_list;
      cfg.domains = domains;
    }

  r = write_network_config(output_dir, IFCFG_PREFIX, nr, &cfg, rfc2132, false, vlanid);
  if (r < 0)
    return r;

  if (vlanid > 0)
    {
      r = write_vlan_file(output_dir, cfg.interface, vlanid);
      if (r <0)
        return r;
    }

  return 0;
}
