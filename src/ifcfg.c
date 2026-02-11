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

/* Configuration */
#define NETDEV_PREFIX  "62-ifcfg-vlan"
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
split_and_write(FILE *fp, const char *key, const char *list)
{
  _cleanup_free_ char *values = NULL; // initial pointer to free memory
  char *token = NULL;
  char *copy = NULL;

  if (isempty(list))
    return 0;

  values = strdup(list);
  if (!values)
    return -ENOMEM;

  copy = values;
  token = strsep(&copy, " ");
  while (token)
    {
      fprintf(fp, "%s=%s\n", key, token);
      token = strsep(&copy, " ");
    }
  return 0;
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

  printf("Creating vlan config: %s for interface '%s.%d'\n", filepath,
	 interface, vlanid);

  if (access(filepath, F_OK) != 0)
    { // file does not exist
      fp = fopen(filepath, "w");
      if (!fp)
	{
	  r = -errno;
	  fprintf(stderr, "Failed to open network file '%s' for writing: %s",
		  filepath, strerror(-r));
	  return r;
	}

      fprintf(fp, "[Match]\n");
      fprintf(fp, "Name=%s\n", interface);
      fprintf(fp, "Type=ether\n");

      fprintf(fp, "\n[Network]\n");
      fprintf(fp, "Description=The unconfigured physical ethernet device\n");
      fprintf(fp, "VLAN=Vlan%04d\n", vlanid);
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
	  fprintf(stderr, "Failed to open network file '%s' for appending: %s",
		  filepath, strerror(-r));
	  return r;
	}
      fprintf(fp, "VLAN=Vlan%04d\n", vlanid);
    }

  return 0;
}

// XXX merge with ip.c
/* Writes the systemd-networkd .network file */
static int
write_network_file(const char *output_dir, int nr, ip_t *cfg,
		   int rfc2132, int vlanid)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;
  int r;

  if (asprintf(&filepath, "%s/%s-%02d.network",
	       output_dir, IFCFG_PREFIX, nr) < 0)
    return -ENOMEM;

  printf("Creating config: %s for interface '%s'\n", filepath,
	 cfg->interface);

  fp = fopen(filepath, "w");
  if (!fp)
    {
      r = -errno;
      fprintf(stderr, "Failed to open network file '%s' for writing: %s",
	      filepath, strerror(-r));
      return r;
    }

  /* [Match] Section: */
  fprintf(fp, "[Match]\n");
  if (vlanid)
    {
      fprintf(fp, "Name=Vlan%04d\n", vlanid);
      fprintf(fp, "Type=vlan\n");
    }
  else
    {
      /* Heuristic: If the interface contains ':', assume MAC.
	 Otherwise Name (supports globs like eth*). */
      if (strchr(cfg->interface, ':'))
	fprintf(fp, "Name=*\nMACAddress=%s\n", cfg->interface);
      else
	fprintf(fp, "Name=%s\n", cfg->interface);
    }

  /* [Network] Section: */
  fprintf(fp, "\n[Network]\n");

  if (!isempty(cfg->autoconf))
    {
      if (streq(cfg->autoconf, "dhcp"))
	fprintf(fp, "DHCP=yes\n");
      else if (streq(cfg->autoconf, "dhcp4"))
	fprintf(fp, "DHCP=ipv4\n");
      else if (streq(cfg->autoconf, "dhcp6"))
	fprintf(fp, "DHCP=ipv6\n");
    }

  /* Static IPs (space separated) */
  r = split_and_write(fp, "Address", cfg->client_ip);
  if (r < 0)
    return r;

  r = split_and_write(fp, "Gateway", cfg->gateway);
  if (r < 0)
    return r;

  r = split_and_write(fp, "DNS", cfg->dns1);
  if (r < 0)
    return r;

  if (!isempty(cfg->domains))
    fprintf(fp, "Domains=%s\n", cfg->domains);

  /* DHCP Specific Options */
  if (!isempty(cfg->autoconf))
    {
      if (streq(cfg->autoconf, "dhcp") || streq(cfg->autoconf, "dhcp4"))
	{
	  fprintf(fp, "\n[DHCPv4]\n");
	  fprintf(fp, "UseHostname=false\n");
	  fprintf(fp, "UseDNS=true\n");
	  fprintf(fp, "UseNTP=true\n");

	  if (rfc2132)
	    fprintf(fp, "ClientIdentifier=mac\n");
	}
      if (streq(cfg->autoconf, "dhcp") || streq(cfg->autoconf, "dhcp6"))
	{
	  fprintf(fp, "\n[DHCPv6]\n");
	  fprintf(fp, "UseHostname=false\n");
	  fprintf(fp, "UseDNS=true\n");
	  fprintf(fp, "UseNTP=true\n");
	}
    }

  if (vlanid)
    return write_vlan_file(output_dir, cfg->interface, vlanid);

  return 0;
}

/* VLAN functions */
#define VLAN_CAPACITY 10
static const int vlan_capacity = VLAN_CAPACITY;
static int vlans[VLAN_CAPACITY];
static int nr_vlanids = 0;

static bool
is_duplicate(int *list, int count, int new_id)
{
  for (int i = 0; i < count; i++)
    if (list[i] == new_id)
	return true;

  return false;
}

static int
write_netdev_file(const char *output_dir, int vlanid)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;
  int r;

  if (asprintf(&filepath, "%s/%s%04d.netdev",
	       output_dir, NETDEV_PREFIX, vlanid) < 0)
    return -ENOMEM;

  printf("Creating vlan netdev: %s for vlan id '%d'\n", filepath,
	 vlanid);

  fp = fopen(filepath, "w");
  if (!fp)
    {
      r = -errno;
      fprintf(stderr, "Failed to open network file '%s' for writing: %s",
	      filepath, strerror(-r));
      return r;
    }

  fprintf(fp, "[NetDev]\n");
  fprintf(fp, "Name=Vlan%04d\n", vlanid);
  fprintf(fp, "Kind=vlan\n");

  fprintf(fp, "\n[VLAN]\n");
  fprintf(fp, "Id=Vlan%d\n", vlanid);

  return 0;
}

int
create_netdev_files(const char *output_dir)
{
  int r;

  for (int i = 0; i < nr_vlanids; i++)
    {
      r = write_netdev_file(output_dir, vlans[i]);
      if (r != 0)
	return r;
    }
  return 0;
}

// XXX merge with ip.c
static int
extract_word(char **str, const char *sep, bool required, char **ret)
{
  char *token;

  token = strsep(str, sep);
  if (isempty(token) && required)
    return -EINVAL;

  *ret = token;

  return 0;
}

/* Parses a single ifcfg string */
int
parse_ifcfg_arg(const char *output_dir, int nr, const char *arg)
{
  ip_t cfg = {0}; // Initialize all pointers to NULL
  _cleanup_free_ char *copy_to_free = strdup(arg); // to free everything
  char *str = copy_to_free; // Pointer for strsep
  char *token;
  /* vlan */
  int vlanid = 0;
  /* dhcp */
  int rfc2132 = 0;
  int r;

  if (debug)
    printf("parse_ifcfg_arg=%d - '%s'\n", nr, arg);

  // Syntax: <interface>=<str>

  r = extract_word(&str, "=", true, &token);
  if (r < 0)
    return return_syntax_error(nr, arg, -EINVAL);

  cfg.interface = token;

  if (isempty(cfg.interface) || isempty(str))
    return return_syntax_error(nr, arg, -ENOENT);

  if (debug)
    printf("Interface - Config: '%s' - '%s'\n",
	   cfg.interface, str);

  if (!isempty(cfg.interface))
    {
      char *vlanid_str = strrchr(cfg.interface, '.');
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
	      fprintf(stderr, "Invalid VLAN interface: %s\n", cfg.interface);
	      return -EINVAL;
	    }
	  vlanid = l;

	  if (!is_duplicate(vlans, nr_vlanids, vlanid))
	    {
	      if ((nr_vlanids+1) == vlan_capacity)
		{
		  fprintf(stderr, "Too many vlans!\n");
		  return -ENOMEM;
		}

	      vlans[nr_vlanids] = vlanid;
	      nr_vlanids++;
	      if (debug)
		printf("Stored VLAN ID: %d\n", vlanid);
	    }
	}
    }

  // Format: IP_LIST,GATEWAY_LIST,NAMESERVER_LIST,DOMAINSEARCH_LIST
  char *ip_list = trim_whitespace(strsep(&str, ","));
  char *gw_list = trim_whitespace(strsep(&str, ","));
  char *dns_list = trim_whitespace(strsep(&str, ","));
  char *domains = trim_whitespace( strsep(&str, ","));

  if (strneq(ip_list, "dhcp", 4))
    {
      cfg.autoconf = ip_list;
      if (!isempty(gw_list) && streq(gw_list, "rfc2132"))
	rfc2132 = 1;
    }
  else
    {
      cfg.client_ip = ip_list;
      cfg.gateway = gw_list;
      cfg.dns1 = dns_list;
      cfg.domains = domains;
    }

  r = write_network_file(output_dir, nr, &cfg, rfc2132, vlanid);
  if (r == -ENOMEM)
    return r;

  return 0;
}
