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
#include "mkdir_p.h"
#include "rdii-networkd.h"
#include "ifcfg.h"
#include "ip.h"

bool debug = false;

/* Configuration */
#define CMDLINE_PATH "/proc/cmdline"

#define IP_PREFIX   "66-ip" // XXX replace with 66-rdii
#define NETDEV_PREFIX "62-rdii"

#define MAX_INTERFACES 10
static ip_t configs[MAX_INTERFACES] = {0};
static int used_configs = 0;

/* VLAN */
typedef struct {
  int id;
  const char *name;
} vlan_t;

#define VLAN_CAPACITY 10
static const int vlan_capacity = VLAN_CAPACITY;
static vlan_t vlans[VLAN_CAPACITY];
static int nr_vlanids = 0;

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

#if 0 // XXX strjoin needed?
static char *
strjoin(const char *str1, const char *str2, const char *str3)
{
  size_t length = 0;
  char *cp;

  if (!isempty(str1))
    length += strlen(str1);
  if (!isempty(str2))
    length += strlen(str2);
  if (!isempty(str3))
    length += strlen(str3);

  length += 1; // for \0

  _cleanup_free_ char *result = malloc(length);
  if (result == NULL)
    return NULL;

  cp = result;

  if (!isempty(str1))
    cp = stpcpy(cp, str1);
  if (!isempty(str2))
    cp = stpcpy(cp, str2);
  if (!isempty(str3))
    cp = stpcpy(cp, str3);

  return TAKE_PTR(result);
}
#endif

static int
dup_config(ip_t *cfg, int slot)
{
  // XXX check for OOM
  if (!isempty(cfg->client_ip))
    configs[slot].client_ip = strdup(cfg->client_ip);
  if (!isempty(cfg->peer_ip))
    configs[slot].peer_ip = strdup(cfg->peer_ip);
  if (!isempty(cfg->gateway))
    {
      // XXX hack for rd.route=<destination>:<gateway>,
      // ip= has no destination.
      if (configs[slot].gateway)
	configs[slot].gateway1 = configs[slot].gateway;
      configs[slot].gateway = strdup(cfg->gateway);
    }
  if (!isempty(cfg->destination))
    configs[slot].destination = strdup(cfg->destination);
  if (cfg->netmask)
    configs[slot].netmask = cfg->netmask;
  if (!isempty(cfg->hostname))
    configs[slot].hostname = strdup(cfg->hostname);
  if (!isempty(cfg->interface))
    configs[slot].interface = strdup(cfg->interface);
  if (!isempty(cfg->autoconf))
    configs[slot].autoconf = strdup(cfg->autoconf);
  if (cfg->use_dns)
    configs[slot].use_dns = cfg->use_dns;
  if (!isempty(cfg->dns1))
    configs[slot].dns1 = strdup(cfg->dns1);
  if (!isempty(cfg->dns2))
    configs[slot].dns2 = strdup(cfg->dns2);
  if (!isempty(cfg->ntp))
    configs[slot].ntp = strdup(cfg->ntp);
  if (!isempty(cfg->mtu))
    configs[slot].mtu = strdup(cfg->mtu);
  if (!isempty(cfg->macaddr))
    configs[slot].macaddr = strdup(cfg->macaddr);
  if (!isempty(cfg->domains))
    configs[slot].domains = strdup(cfg->domains);
  if (cfg->vlan1)
    {
      if (configs[slot].vlan1 == 0)
	configs[slot].vlan1 = cfg->vlan1;
      else if (configs[slot].vlan2 == 0)
	configs[slot].vlan2 = cfg->vlan1;
      else if (configs[slot].vlan3 == 0)
	configs[slot].vlan3 = cfg->vlan1;
      else
	{
	  fprintf(stderr, "More than 3 VLAN IDs!\n");
	  return -ENOMEM;
	}
    }

  return 0;
}

static int
merge_configs(ip_t *cfg)
{
  bool found = false;
  int r;

  if (used_configs == MAX_INTERFACES)
    {
      fprintf(stderr, "Too many interfaces!\n");
      return -ENOMEM;
    }

  if (used_configs != 0)
    {
      for (int i = 0; i < used_configs; i++)
	{
	  if (configs[i].interface && cfg->interface &&
	      streq(configs[i].interface, cfg->interface))
	    {
	      r = dup_config(cfg, i);
	      if (r < 0)
		return r;
	      return 0;
	    }
	  if (configs[i].interface && !cfg->interface)
	    {
	      // existing config contains interface, new one not.
	      // "merge" them. (e.g. ip=xxx rd.route=yyy)
	      r = dup_config(cfg, i);
	      if (r < 0)
		return r;
	      found = true;
	    }
	}
    }

  if (!found)
    {
      r = dup_config(cfg, used_configs);
      if (r < 0)
	return r;
      used_configs++;
    }

  return 0;
}

static int
write_vlan_entry(FILE *fp, int vlanid)
{
  for (int i = 0; i < nr_vlanids; i++)
    if (vlans[i].id == vlanid)
      {
	fprintf(fp, "VLAN=%s\n", vlans[i].name);
	return 0;
      }
  return -ENOKEY;
}

int
write_network_config(const char *output_dir, int line_num, ip_t *cfg)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;

  if (asprintf(&filepath, "%s/%s-%02d.network",
               output_dir, IP_PREFIX, line_num) < 0)
    return -ENOMEM;

  if (debug)
    printf("Entry %2d: %s config\n", line_num, filepath);

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
      !isempty(cfg->ntp) || cfg->vlan1)
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
      if (cfg->vlan1)
	write_vlan_entry(fp, cfg->vlan1); // XXX return value
      if (cfg->vlan2)
	write_vlan_entry(fp, cfg->vlan2); // XXX return value
      if (cfg->vlan3)
	write_vlan_entry(fp, cfg->vlan3); // XXX return value
    }

  if (!isempty(cfg->hostname) || cfg->use_dns > 0)
    {
      fputs("\n[DHCP]\n", fp);
      if (cfg->hostname)
        fprintf(fp, "Hostname=%s\n", cfg->hostname);
      if (cfg->use_dns == 1)
        fputs("UseDNS=no\n", fp);
      if (cfg->use_dns == 2)
        fputs("UseDNS=yes\n", fp);
    }

  if (!isempty(cfg->client_ip))
    {
      fputs("\n[Address]\n", fp);
      fprintf(fp, "Address=%s/%d\n", cfg->client_ip, cfg->netmask);
      if (!isempty(cfg->peer_ip))
        fprintf(fp, "Peer=%s\n", cfg->peer_ip);
    }

  if (!isempty(cfg->gateway) || !isempty(cfg->destination))
    {
      fputs("\n[Route]\n", fp);
      if (!isempty(cfg->destination))
        fprintf(fp, "Destination=%s\n", cfg->destination);
      if (!isempty(cfg->gateway))
        fprintf(fp, "Gateway=%s\n", cfg->gateway);
    }

  if (!isempty(cfg->gateway1))
    {
      fputs("\n[Route]\n", fp);
      fprintf(fp, "Gateway=%s\n", cfg->gateway1);
    }

  return 0;
}

/* VLAN functions */
static int
write_netdev_file(const char *output_dir, vlan_t *vlan)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;
  int r;

  if (asprintf(&filepath, "%s/%s-%s.netdev",
               output_dir, NETDEV_PREFIX, vlan->name) < 0)
    return -ENOMEM;

  if (debug)
    printf("Creating vlan netdev: %s for vlan id '%d'\n", filepath,
	   vlan->id);

  fp = fopen(filepath, "w");
  if (!fp)
    {
      r = -errno;
      fprintf(stderr, "Failed to open network file '%s' for writing: %s",
              filepath, strerror(-r));
      return r;
    }

  fprintf(fp, "[NetDev]\n");
  fprintf(fp, "Name=%s\n", vlan->name);
  fprintf(fp, "Kind=vlan\n");

  fprintf(fp, "\n[VLAN]\n");
  fprintf(fp, "Id=%d\n", vlan->id);

  return 0;
}

static int
write_netdev_config(const char *output_dir)
{
  int r;

  for (int i = 0; i < nr_vlanids; i++)
    {
      r = write_netdev_file(output_dir, &vlans[i]);
      if (r != 0)
        return r;
    }
  return 0;
}


static bool
is_duplicate(vlan_t *list, int count, int new_id)
{
  for (int i = 0; i < count; i++)
    if (list[i].id == new_id)
        return true;

  return false;
}

int
get_vlan_id(const char *vlan_name, int *ret)
{
  /* From dracut.cmdline(7):
   * We support the four styles of vlan names:
   *   VLAN_PLUS_VID (vlan0005),
   *   VLAN_PLUS_VID_NO_PAD (vlan5),
   *   DEV_PLUS_VID (eth0.0005), and
   *   DEV_PLUS_VID_NO_PAD (eth0.5). */

  for (const char *p = vlan_name + strlen(vlan_name) - 1; p > vlan_name; p--)
    if (!isdigit(*p))
      {
	char *ep;
	long l;
	int vlanid = 0;

	p++;
	l = strtol(p, &ep, 10);
	// valid: 1 <= VLAN ID <= 4095
	if (errno == ERANGE || l < 1 || l > 4095 ||
	    p == ep || *ep != '\0')
	  {
	    fprintf(stderr, "Invalid VLAN interface: %s\n", vlan_name);
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

	    vlans[nr_vlanids].id = vlanid;
	    vlans[nr_vlanids].name = vlan_name;
	    nr_vlanids++;
	    if (debug)
	      printf("Stored VLAN ID: %d (%s)\n", vlanid, vlan_name);
	  }
	*ret = vlanid;
	return 0;
      }

  return -EINVAL;
}

int
return_syntax_error(int nr, const char *value, const int ret)
{
  fprintf(stderr, "Syntax error in entry %d: '%s'\n", nr, value);
  return ret;
}

static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: rdii-networkd [--help]|[--version]|[--debug]\n");
}

static void
print_help(void)
{
  fprintf(stdout, "rdii-networkd - create networkd config from cmdline\n\n");
  print_usage(stdout);

  fputs("  -a, --parse-all      Parse all network options on cmdline\n", stdout);
  fputs("  -c, --config <file>  File with configuration\n", stdout);
  fputs("  -d, --debug          Write config to stdout\n", stdout);
  fputs("  -o, --output         Directory in which to write config\n", stdout);
  fputs("  -h, --help           Give this help list\n", stdout);
  fputs("  -v, --version        Print program version\n", stdout);
}

static void
print_error(void)
{
  fputs("Try `rdii-networkd --help' for more information.\n", stderr);
}

/* Reads /proc/cmdline and parses quoted arguments */
int
main(int argc, char *argv[])
{
  const char *output_dir = "/run/systemd/network";
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;
  const char *cfgfile = NULL;
  struct stat st;
  bool parse_all = false;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"config",    required_argument, NULL, 'c' },
          {"debug",     no_argument,       NULL, 'd' },
	  {"output",    required_argument, NULL, 'o' },
	  {"parse-all", no_argument,       NULL, 'a' },
	  {"help",      no_argument,       NULL, 'h' },
          {"version",   no_argument,       NULL, 'v' },
          {NULL,        0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "ac:do:hv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
	case 'a':
	  parse_all = true;
	  break;
	case 'c':
	  cfgfile = optarg;
	  break;
        case 'd':
	  debug = true;
          break;
	case 'o':
	  output_dir = optarg;
	  break;
        case 'h':
          print_help();
          return 0;
        case 'v':
          printf("rdii-networkd (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return 1;
        }
    }

  argc -= optind;
  argv += optind;

  if (!isempty(cfgfile) && argc > 0)
    {
      fputs("Using a configuration file with additional arguments is not possible\n", stderr);
      print_error();
      return 1;
    }

  if (stat(output_dir, &st) == -1)
    {
      if (mkdir_p(output_dir, 0755) == -1 && errno != EEXIST)
	{
	  r = errno;
	  fprintf(stderr, "Could not create output directory: %s\n",
		  strerror(r));
	  return r;
	}
    }

  if (!isempty(cfgfile))
    {
      size_t line_count = 0;

      fp = fopen(cfgfile, "r");
      if (!fp)
	{
	  r = errno;
	  fprintf(stderr, "Error opening '%s': %s\n",
		  cfgfile, strerror(r));
	  return r;
	}


      while ((nread = getline(&line, &line_size, fp)) != -1)
	{
	  ip_t cfg = {0};

	  line_count++;

	  if (isempty(line) || line[0] == '#' || line[0] == '\n')
	    continue;

	  if (line[nread-1] == '\n')
	    line[nread-1] = '\0';

	  if (startswith(line, "ip="))
	    r = parse_ip_arg(line_count, line+3, &cfg);
	  else if (startswith(line, "nameserver="))
	    r = parse_nameserver_arg(line_count, line+11, &cfg);
	  else if (startswith(line, "rd.peerdns="))
	    r = parse_rd_peerdns_arg(line_count, line+11, &cfg);
	  else if (startswith(line, "rd.route="))
	    r = parse_rd_route_arg(line_count, line+9, &cfg);
	  else if (startswith(line, "vlan="))
	    r = parse_vlan_arg(line_count, line+5, &cfg);
	  else if (startswith(line, "ifcfg="))
	    r = parse_ifcfg_arg(output_dir, line_count, line+6);
	  else
	    {
	      r = 255;
	      if (debug)
		printf("Ignoring: '%s'\n", line);
	    }

	  if (r < 0)
	    return -r;

	  if (r == 0)
	    merge_configs(&cfg);
	}

      if (ferror(fp))
	{
	  r = errno;
	  fprintf(stderr, "Error reading '%s': %s\n",
		  cfgfile, strerror(errno));
	  return r;
	}
    }
  else
    {
      // do cmdline parsing...
      char *cp;

      if (argc > 0)
	{
	  // Allow overriding input for testing: rdii-networkd "ifcfg=..."
	  size_t total_length = 0;

	  for (int i = 0; i < argc; i++)
	    {
	      total_length += strlen(argv[i]);
	      total_length++; // for ' ' or '\0'
	    }

	  line = malloc(total_length);
	  if (line == NULL)
	    {
	      fprintf(stderr, "Out of memory!\n");
	      return ENOMEM;
	    }

	  cp = line;

	  for (int i = 0; i < argc; i++)
	    {
	      cp = stpcpy(cp, argv[i]);
	      if (i < argc - 1) // not last argument
		cp = stpcpy(cp, " ");
	    }
	}
      else
	{
	  fp = fopen(CMDLINE_PATH, "r");
	  if (!fp)
	    {
	      r = errno;
	      fprintf(stderr, "Failed to open %s: %s",
		      CMDLINE_PATH, strerror(r));
	      return r;
	    }

	  nread = getline(&line, &line_size, fp);
	  if (nread == -1)
	    {
	      fprintf(stderr, "Failed to read %s: %s",
		      CMDLINE_PATH, strerror(r));
	      return errno;

	      if (nread > 0 && line[nread-1] == '\n')
		line[nread-1] = '\0';
	    }
	}

      if (debug)
	printf("cmdline=%s\n", line);

      // Parse loop handling quotes
      cp = line;
      char *arg_start = cp;
      int in_quote = 0;
      int nr = 1;

      while (*cp)
	{
	  if (*cp == '"')
	    in_quote = !in_quote;

	  if (cp[1] == '\0' || (*cp == ' ' && !in_quote))
	    {
	      if (*cp == ' ')
		*cp = '\0'; // Terminate current arg

	      if (startswith(arg_start, "ifcfg="))
		{
		  char *val = arg_start + 6;

		  // Strip quotes surround the value part
		  if (val[0] == '"')
		    {
		      val++;
		      size_t l = strlen(val);
		      if (l > 0 && val[l-1] == '"')
			val[l-1] = '\0';
		    }
		  r = parse_ifcfg_arg(output_dir, nr++, val);
		  // quit if out of memory, else ignore entry
		  if (r != 0)
		    {
		      if (r == -ENOMEM)
			exit(ENOMEM);
		      else
			fprintf(stderr, "Skip '%s' due to errors\n", val);
		    }
		}
	      else if (parse_all)
		{
		  ip_t cfg = {0};

		  // this options are normally handled by systemd-network-generator
		  if (startswith(arg_start, "ip="))
		    r = parse_ip_arg(nr++, arg_start+3, &cfg);
		  else if (startswith(arg_start, "nameserver="))
		    r = parse_nameserver_arg(nr++, arg_start+11, &cfg);
		  else if (startswith(arg_start, "rd.peerdns="))
		    r = parse_rd_peerdns_arg(nr++, arg_start+11, &cfg);
		  else if (startswith(arg_start, "rd.route="))
		    r = parse_rd_route_arg(nr++, arg_start+9, &cfg);
		  else if (startswith(line, "vlan="))
		    r = parse_vlan_arg(nr++, arg_start+5, &cfg);
		  else
		    r = 255;

		  if (r < 0)
		    return -r;

		  if (r == 0)
		    merge_configs(&cfg);
		}
	      arg_start = cp + 1;
	    }
	  cp++;
	}
    }

  for (int i = 0; i < used_configs; i++)
    write_network_config(output_dir, i+1, &configs[i]);

  if (nr_vlanids > 0)
    {
      r = write_netdev_config(output_dir);
      if (r < 0)
	{
	  fprintf(stderr, "Error writing .netdev files: %s\n",
		  strerror(-r));
	  return -r;
	}
    }

  return 0;
}
