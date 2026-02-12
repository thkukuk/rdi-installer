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

  if ((*str)[0] == '[')
    {
      if (!strchr(*str, ']'))
	return -EINVAL;
      token = strsep(str, "]");
      (*str)[0]='\0'; // XXX overwrite ":", safety check
      (*str)++;
      token[strlen(token)] = ']';
    }
  else
    token = strsep(str, ":");
  if (isempty(token) && required)
    return -EINVAL;

  *ret = token;

  return 0;
}

int
parse_ip_arg(int nr, char *arg, ip_t *cfg)
{
  char *token;
  _cleanup_free_ char *orig = strdup(arg); // for syntax error msg
  int r;

  if (orig == NULL)
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
      cfg->autoconf = arg;
    }
  else
    {
      // IP or interface
      r = extract_ip_addr(&arg, true, &token);
      if (r == 0)
	{
	  // <client-IP>:[<peer>]:<gateway-IP>:<netmask>:<client_hostname>:<interface>:...
	  cfg->client_ip = token;

	  r = extract_ip_addr(&arg, false, &token);
	  if (r < 0)
	    return return_syntax_error(nr, orig, r);
	  cfg->peer_ip = token;

	  r = extract_ip_addr(&arg, true, &token);
	  if (r < 0)
	    return return_syntax_error(nr, orig, r);
	  cfg->gateway = token;

	  r = extract_word(&arg, true, &token);
	  if (r < 0)
	    return return_syntax_error(nr, orig, r);
	  if (strchr(token, '.')) // something like 255.255.0.0
	    {
	      int cidr;

	      r = netmask_to_cidr(token, &cidr);
	      if (r < 0)
		return return_syntax_error(nr, orig, r);
	      cfg->netmask = cidr;
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
	      cfg->netmask = l;
	    }

	  r = extract_word(&arg, false, &token);
	  if (r < 0)
	    return return_syntax_error(nr, orig, r);
	  cfg->hostname = token;

	  r = extract_word(&arg, true, &token);
	  if (r < 0)
	    return return_syntax_error(nr, orig, r);
	  cfg->interface = token;

	  r = extract_word(&arg, false, &token);
	  if (r < 0)
	    return return_syntax_error(nr, orig, r);
	  cfg->autoconf = token;

	  // either <mtu>:<macaddr> or <dns1>:<dns2>:<ntp>
	  if (!isempty(arg))
	    {
	      r = extract_word(&arg, false, &token);
	      if (r < 0)
		return return_syntax_error(nr, orig, r);

	      // XXX IPv6 with [] are broken here!
	      if (is_ip_addr(token))
		{
		  cfg->dns1 = token;
		  if (!isempty(arg))
		    {
		      r = extract_ip_addr(&arg, false, &token);
		      if (r < 0)
			return return_syntax_error(nr, orig, r);
		      cfg->dns2 = token;
		      if (!isempty(arg))
			{
			  r = extract_ip_addr(&arg, false, &token);
			  if (r < 0)
			    return return_syntax_error(nr, orig, r);
			  cfg->ntp = token;
			}
		      // we are at the end, if there is more stuff...
		      if (!isempty(arg))
			return return_syntax_error(nr, orig, -EINVAL);
		    }
		}
	      else if (!isempty(token))
		{
		  // must be <mtu>:<macaddr>
		  cfg->mtu = token;
		  cfg->macaddr = arg;
		}
	      else if (isempty(token) && !isempty(arg))
		{
		  int count = 0;

		  for (size_t i = 0; i < strlen(arg); i++)
		    if (arg[i] == ':')
		      count++;
		  if (count == 5)
		    cfg->macaddr = arg; // must be macaddr
		  else
		    {
		      r = extract_word(&arg, false, &token);
		      if (r < 0)
			return return_syntax_error(nr, orig, r);
		      cfg->dns2 = token;

		      if (!isempty(arg))
			{
			  if (is_ip_addr(arg)) // XXX IPv6
			    cfg->ntp = arg;
			  else
			    return return_syntax_error(nr, orig, r);
			}
		    }
		}
	    }
	}
      else
	{
	  // - ip=<interface>:{dhcp|on|any|dhcp6|auto6|link6}[:[<mtu>][:<macaddr>]]
	  cfg->interface = token;
	  token = strsep(&arg, ":");
	  cfg->autoconf = token;
	  if (!isempty(arg))
	    {
	      /* [<mtu>][:<macaddr>] */

	      token = strsep(&arg, ":");
	      /* XXX verify that mtu is in bytes and is >= 68 for IPv4
		 and >= 1280 for IPv6 */
	      cfg->mtu = token;

	      if (!isempty(arg))
		{
		  if (arg[strlen(arg)-1] == ':')
		    return return_syntax_error(nr, orig, -EINVAL);
		  cfg->macaddr = arg;
		}
	    }
	}
    }

  return 0;
}

int
parse_nameserver_arg(int nr, char *arg, ip_t *cfg)
{
  char *token;
  _cleanup_free_ char *orig = strdup(arg); // for syntax error msg
  int r;

  if (orig == NULL)
    return -ENOMEM;

  r = extract_ip_addr(&arg, true, &token);
  if (r < 0)
    return return_syntax_error(nr, orig, r);
  cfg->dns1 = token;

  if (!isempty(arg))
    return return_syntax_error(nr, orig, -EINVAL);

  return 0;
}

int
parse_rd_peerdns_arg(int nr, char *arg, ip_t *cfg)
{
  char *token;
  _cleanup_free_ char *orig = strdup(arg); // for syntax error msg
  int r;

  if (orig == NULL)
    return -ENOMEM;

  r = extract_word(&arg, true, &token);
  if (r < 0)
    return return_syntax_error(nr, orig, r);
  if (streq(token, "0"))
    cfg->use_dns = 1;
  else if (streq(token, "1"))
    cfg->use_dns = 2;
  else
    return return_syntax_error(nr, orig, -EINVAL);

  if (!isempty(arg))
    return return_syntax_error(nr, orig, -EINVAL);

  return 0;
}

int
parse_rd_route_arg(int nr, char *arg, ip_t *cfg)
{
  char *token;
  _cleanup_free_ char *orig = strdup(arg); // for syntax error msg
  int r;

  if (orig == NULL)
    return -ENOMEM;

  r = extract_word(&arg, true, &token);
  if (r < 0)
    return return_syntax_error(nr, arg, r);

  if (token[0] == '[')
    {
      token++;
      if (token[strlen(token)-1] != ']')
	return return_syntax_error(nr, arg, r);
      token[strlen(token)-1] = '\0';
    }
  cfg->destination=token;

  r = extract_ip_addr(&arg, false, &token);
  if (r < 0)
    return return_syntax_error(nr, orig, r);
  cfg->gateway = token;

  if (!isempty(arg))
    { // interface is optional
      r = extract_word(&arg, true, &token);
      if (r < 0)
	return return_syntax_error(nr, orig, r);
      cfg->interface = token;
    }

  if (!isempty(arg))
    return return_syntax_error(nr, orig, -EINVAL);

  return 0;
}

int
parse_vlan_arg(int nr, char *arg, ip_t *cfg)
{
  char *token;
  _cleanup_free_ char *orig = strdup(arg); // for syntax error msg
  int r;

  if (orig == NULL)
    return -ENOMEM;

  r = extract_word(&arg, true, &token);
  if (r < 0)
    return return_syntax_error(nr, orig, r);

  r = get_vlan_id(token, &cfg->vlan1);
  if (r < 0)
    return return_syntax_error(nr, orig, r);

  r = extract_word(&arg, true, &token);
  if (r < 0)
    return return_syntax_error(nr, orig, r);
  cfg->interface = token;

  if (!isempty(arg))
    return return_syntax_error(nr, orig, -EINVAL);

  return 0;
}
