// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "basics.h"
#include "rdii-networkd.h"
#include "ip.h"

#define IP_PREFIX   "66-ip"

typedef struct {
    const char *dracut;
    const char *networkd;
} dhcp_dracut_networkd_t;

static const char*
map_dracut_to_networkd(const char *input)
{
  const dhcp_dracut_networkd_t mappings[] =
    {
      { "none",       "no" },
      { "off",        "no" },
      { "on",         "yes" },
      { "any",        "yes" },
      { "dhcp",       "ipv4" },
      { "dhcp6",      "ipv6" },
      { "auto6",      "no" },
      { "either6",    "ipv6" },
      { "ibft",       "no" },
      { "link6",      "no" },
      { "link-local", "no" },
      { NULL,         NULL }
    };

  if (isempty(input))
    return NULL;

  for (int i = 0; mappings[i].dracut != NULL; i++)
    {
      // Use strcmp for exact match, or strcasecmp for case-insensitive
      if (streq(input, mappings[i].dracut))
	return mappings[i].networkd;
    }

  fprintf(stderr, "Unknown autoconf option '%s', valid are {dhcp|on|any|dhcp6|auto6|either6|link6|single-dhcp}\n", input);

  return NULL;
}

/* Converts a netmask string (e.g., "255.255.0.0") to a CIDR integer (e.g., 16).
   Returns -1 if the format is invalid or if the mask is not contiguous. */
static int
netmask_to_cidr(const char *netmask_str, int *cidr)
{
  struct in_addr addr;
  int r;

  r = inet_pton(AF_INET, netmask_str, &addr);
  if (r == -1)
    return -errno;
  if (r == 0)
    return -EINVAL;

  uint32_t mask = ntohl(addr.s_addr);
  int bits = 0;

  for (int i = 0; i < 32; i++)
    {
      // Check if the most significant bit (leftmost) is set
      if ((mask & 0x80000000) != 0)
	{
	  bits++;
	  mask <<= 1; // Shift left to bring the next bit to the MSB position
        }
      else
	{
	  // Once we hit a '0', the rest of the mask MUST be 0
	  if (mask != 0)
	    return -EINVAL;

	  break;
        }
    }
  *cidr = bits;
  return 0;
}

static int
write_network_config(const char *output_dir, int line_num, ip_t *cfg)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;

  if (asprintf(&filepath, "%s/%s-%02d.network",
	       output_dir, IP_PREFIX, line_num) < 0)
    return -ENOMEM;

  if (debug)
    printf("Line %2d: %s config\n", line_num, filepath);

  fp = fopen(filepath, "w");
  if (!fp)
    {
      int r = -errno;
      fprintf(stderr, "Failed to open network file '%s' for writing: %s",
	      filepath, strerror(-r));
      return r;
    }

  fputs("[Match]\n", fp);

  if (isempty(cfg->interface) || streq(cfg->interface, "*"))
    fputs("Kind=!*\n"
	  "Type=!loopback\n", fp);
  else
    {
      /* Heuristic: If the interface contains ':', assume MAC.
	 Otherwise Name (supports globs like eth*). */
      if (strchr(cfg->interface, ':'))
	fprintf(fp, "Name=*\nMACAddress=%s\n", cfg->interface);
      else
	fprintf(fp, "Name=%s\n", cfg->interface);
    }

  if (!isempty(cfg->mtu) || !isempty(cfg->macaddr))
    {
      fputs("\n[Link]\n", fp);
      if (!isempty(cfg->macaddr))
	fprintf(fp, "MACAddress=%s\n", cfg->macaddr);
      if (!isempty(cfg->mtu))
	fprintf(fp, "MTUBytes=%s\n", cfg->mtu);
    }

  if (!isempty(cfg->autoconf) || !isempty(cfg->dns1) || !isempty(cfg->dns2) ||
      !isempty(cfg->ntp))
    {
      fputs("\n[Network]\n", fp);
      if (!isempty(cfg->autoconf))
	{
	  fprintf(fp, "DHCP=%s\n", map_dracut_to_networkd(cfg->autoconf));
	  if (streq(cfg->autoconf, "off"))
	    fputs("LinkLocalAddressing=no\n"
		  "IPv6AcceptRA=no\n", fp);
	}
      if (!isempty(cfg->dns1))
	fprintf(fp, "DNS=%s\n", cfg->dns1);
      if (!isempty(cfg->dns2))
	fprintf(fp, "DNS=%s\n", cfg->dns2);
      if (!isempty(cfg->domains))
	fprintf(fp, "Domains=%s\n", cfg->domains);
      if (!isempty(cfg->ntp))
	fprintf(fp, "NTP=%s\n", cfg->ntp);
    }

  if (!isempty(cfg->hostname))
    {
      fputs("\n[DHCP]\n", fp);
      fprintf(fp, "Hostname=%s\n", cfg->hostname);
    }

  if (!isempty(cfg->client_ip))
    {
      fputs("\n[Address]\n", fp);
      fprintf(fp, "Address=%s/%d\n", cfg->client_ip, cfg->netmask);
      if (!isempty(cfg->peer_ip))
	fprintf(fp, "Peer=%s\n", cfg->peer_ip);
    }

  if (!isempty(cfg->gateway))
    {
      fputs("\n[Route]\n", fp);
      fprintf(fp, "Gateway=%s\n", cfg->gateway);
    }

  // if (!isempty(cfg->netmask))    printf("  Netmask:   %s\n", cfg->netmask);
  // if (!isempty(cfg->domains))    printf("  Domains:   %s\n", cfg->domains);

  return 0;
}

static bool
is_ip_addr(char *token)
{
  unsigned char buf[sizeof(struct in6_addr)];

  if (inet_pton(AF_INET, token, buf) == 1)
    return true;

  if (inet_pton(AF_INET6, token, buf) == 1)
    return true;

  return false;
}

static int
extract_ip_addr(char **str, bool required, char **ret)
{
  char *token;

  if ((*str)[0] == '[') // IPv6, e.g. [2001:DB8::1]
    {
      token = strsep(str, "]");
      if (isempty(token) || isempty(*str)) // str == NULL means no "]:..." found
	return -EINVAL;
      // token points to '['
      token++;
      // str points to ':'
      (*str)++;
    }
  else
    {
      token = strsep(str, ":");
      if (!isempty(token) && !is_ip_addr(token))
	{
	  /* XXX return token as we could need it as interface.
	     better revert the strsep call */
	  *ret = token;
	  return -EINVAL;
	}
    }

  if (required && isempty(token))
    return -EINVAL;

  *ret = token;

  return 0;
}

static int
extract_word(char **str, bool required, char **ret)
{
  char *token;

  token = strsep(str, ":");
  if (isempty(token) && required)
    return -EINVAL;

  *ret = token;

  return 0;
}

int
parse_ip_arg(const char *output_dir, int nr, const char *arg)
{
  ip_t cfg = {0}; // Initialize all pointers to NULL
  char *token;
  _cleanup_free_ char *copy_to_free = strdup(arg); // to free everything
  char *str = copy_to_free; // Pointer for strsep
  int r;

  if (copy_to_free == NULL)
    return -ENOMEM;

  // Dracut format is roughly:
  // - ip={dhcp|on|any|dhcp6|auto6|either6|link6|single-dhcp}
  // - ip=<interface>:{dhcp|on|any|dhcp6|auto6|link6}[:[<mtu>][:<macaddr>]]
  // - ip=<client-IP>:[<peer>]:<gateway-IP>:<netmask>:<client_hostname>:<interface>:{none|off|dhcp|on|any|dhcp6|auto6|ibft}[:[<mtu>][:<macaddr>]]
  // - ip=<client-IP>:[<peer>]:<gateway-IP>:<netmask>:<client_hostname>:<interface>:{none|off|dhcp|on|any|dhcp6|auto6|ibft}[:[<dns1>][:<dns2>]]

  // Handle the case where ip=dhcp (no colons)
  if (strchr(arg, ':') == NULL)
    {
      // If there are no colons, dracut treats the whole string as the autoconf method
      // OR a single client IP. Context depends on the content, but usually single word = autoconf
      cfg.autoconf = str;
    }
  else
    {
      // IP or interface
      r = extract_ip_addr(&str, true, &token);
      if (r == 0)
	{
	  // <client-IP>:[<peer>]:<gateway-IP>:<netmask>:<client_hostname>:<interface>:...
	  cfg.client_ip = token;

	  r = extract_ip_addr(&str, false, &token);
	  if (r < 0)
	    return return_syntax_error(arg, r);
	  cfg.peer_ip = token;

	  r = extract_ip_addr(&str, true, &token);
	  if (r < 0)
	    return return_syntax_error(arg, r);
	  cfg.gateway = token;

	  r = extract_word(&str, true, &token);
	  if (r < 0)
	    return return_syntax_error(arg, r);
	  if (strchr(token, '.')) // something like 255.255.0.0
	    {
	      int cidr;

	      r = netmask_to_cidr(token, &cidr);
	      if (r < 0)
		return return_syntax_error(arg, r);
	      cfg.netmask = cidr;
	    }
	  else
	    {
	      char *ep;
	      long l;

	      l = strtol(token, &ep, 10);
	      if (errno == ERANGE || l < 0 || l > 128 ||
		  token == ep || *ep != '\0')
		{
		  fprintf(stderr, "Invalid netmask: %s\n", token);
		  return -EINVAL;
		}
	      cfg.netmask = l;
	    }

	  r = extract_word(&str, false, &token);
	  if (r < 0)
	    return return_syntax_error(arg, r);
	  cfg.hostname = token;

	  r = extract_word(&str, true, &token);
	  if (r < 0)
	    return return_syntax_error(arg, r);
	  cfg.interface = token;

	  r = extract_word(&str, false, &token);
	  if (r < 0)
	    return return_syntax_error(arg, r);
	  cfg.autoconf = token;

	  // either <mtu>:<macaddr> or <dns1>:<dns2>:<ntp>
	  if (!isempty(str))
	    {
	      r = extract_word(&str, false, &token);
	      if (r < 0)
		return return_syntax_error(arg, r);

	      // XXX IPv6 with [] are broken here!
	      if (is_ip_addr(token))
		{
		  cfg.dns1 = token;
		  if (!isempty(str))
		    {
		      r = extract_ip_addr(&str, false, &token);
		      if (r < 0)
			return return_syntax_error(arg, r);
		      cfg.dns2 = token;
		      if (!isempty(str))
			{
			  r = extract_ip_addr(&str, false, &token);
			  if (r < 0)
			    return return_syntax_error(arg, r);
			  cfg.ntp = token;
			}
		      // we are at the end, if there is more stuff...
		      if (!isempty(str))
			return return_syntax_error(arg, -EINVAL);
		    }
		}
	      else if (!isempty(token))
		{
		  // must be <mtu>:<macaddr>
		  cfg.mtu = token;
		  cfg.macaddr = str;
		}
	      else if (isempty(token) && !isempty(str))
		{
		  int count = 0;

		  for (size_t i = 0; i < strlen(str); i++)
		    if (str[i] == ':')
		      count++;
		  if (count == 5)
		    cfg.macaddr = str; // must be macaddr
		  else
		    {
		      r = extract_word(&str, false, &token);
		      if (r < 0)
			return return_syntax_error(arg, r);
		      cfg.dns2 = token;

		      if (!isempty(str))
			{
			  if (is_ip_addr(str)) // XXX IPv6
			    cfg.ntp = str;
			  else
			    return return_syntax_error(arg, r);
			}
		    }
		}
	    }
	}
      else
	{
	  // - ip=<interface>:{dhcp|on|any|dhcp6|auto6|link6}[:[<mtu>][:<macaddr>]]
	  cfg.interface = token;
	  token = strsep(&str, ":");
	  cfg.autoconf = token;
	  if (!isempty(str))
	    {
	      /* [<mtu>][:<macaddr>] */

	      token = strsep(&str, ":");
	      /* XXX verify that mtu is in bytes and is >= 68 for IPv4
		 and >= 1280 for IPv6 */
	      cfg.mtu = token;

	      if (!isempty(str))
		{
		  if (str[strlen(str)-1] == ':')
		    return return_syntax_error(arg, -EINVAL);
		  cfg.macaddr = str;
		}
	    }
	}
    }

  write_network_config(output_dir, nr, &cfg);

  return 0;
}
