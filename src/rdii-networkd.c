// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
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
#include "logger.h"

/* Configuration */
#define CMDLINE_PATH "/proc/cmdline"
#define RUN_RDII_CONFIG   "/run/rdi-installer/rdii-config"

#define IP_PREFIX   "66-rdii"
#define NETDEV_PREFIX "62-rdii"

#define MAX_INTERFACES 10

static ip_t configs[MAX_INTERFACES] = {0};
static int used_configs = 0;

static void
init_configs(void)
{
  for (int i = 0; i < MAX_INTERFACES; i++)
    configs[i].netmask = -1;
}

/* VLAN */
typedef struct {
  int id;
  const char *name;
  const char *prefix;
} vlan_t;

#define VLAN_CAPACITY 10
static const int vlan_capacity = VLAN_CAPACITY;
static vlan_t vlans[VLAN_CAPACITY];
static int nr_vlanids = 0;

static int
dup_config(ip_t *cfg, int slot)
{
  if (!isempty(cfg->client_ip))
    configs[slot].client_ip = cfg->client_ip;
  if (!isempty(cfg->peer_ip))
    configs[slot].peer_ip = cfg->peer_ip;
  if (cfg->gateways_count > 0)
    {
      for (int i=0; i<cfg->gateways_count; i++)
	{
	  int r = append_route_settings(cfg->gateways[i], cfg->destinations[i], &(configs[slot]));
	  if (r < 0)
            return r;
	}
    }

  if (cfg->netmask >= 0)
    configs[slot].netmask = cfg->netmask;
  if (!isempty(cfg->hostname))
    configs[slot].hostname = cfg->hostname;
  if (!isempty(cfg->interface))
    configs[slot].interface = cfg->interface;
  if (!isempty(cfg->autoconf))
    {
      /* Keep autoconf and its mapped networkd value in sync: an
	 invalid autoconf value maps to NULL, and must clear any
	 stale mapping left over from a previous merged entry. */
      configs[slot].autoconf = cfg->autoconf;
      configs[slot].autoconf_networkd = cfg->autoconf_networkd;
    }
  if (cfg->use_dns)
    configs[slot].use_dns = cfg->use_dns;
  if (!isempty(cfg->dns1))
    configs[slot].dns1 = cfg->dns1;
  if (!isempty(cfg->dns2))
    configs[slot].dns2 = cfg->dns2;
  if (!isempty(cfg->ntp))
    configs[slot].ntp = cfg->ntp;
  if (!isempty(cfg->mtu))
    configs[slot].mtu = cfg->mtu;
  if (!isempty(cfg->macaddr))
    configs[slot].macaddr = cfg->macaddr;
  if (!isempty(cfg->domains))
    configs[slot].domains = cfg->domains;
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
	  MSG_ERROR("More than 3 VLAN IDs!");
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

  MSG_DEBUG("merge_configs called");

  if (used_configs == MAX_INTERFACES)
    {
      MSG_ERROR("Too many interfaces!");
      return -ENOMEM;
    }

  if (used_configs != 0)
    {
      for (int i = 0; i < used_configs; i++)
	{
	  if (configs[i].interface && cfg->interface &&
	      streq(configs[i].interface, cfg->interface))
            return dup_config(cfg, i);

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
  MSG_ERROR("No valid VLAN found.");
  return -ENOKEY;
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
write_dhcp(FILE *fp, const char *kind, bool rfc2132)
{
  /* Default values are: UseDNS=true, UseNTP=true */
  fprintf(fp, "\n[%s]\n"
          "UseHostname=false\n", kind);
  if (rfc2132)
    fputs("ClientIdentifier=mac\n", fp);

  return 0;
}

/* Writes the systemd-networkd .network file */
int
write_network_config(const char *output_dir, const char *prefix, int line_num,
                     ip_t *cfg, bool rfc2132, bool physical_interfaces_only,  int vlanid)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;
  int r;

  if (asprintf(&filepath, "%s/%s-%02d.network", output_dir, prefix, line_num) < 0)
    return -ENOMEM;

  MSG_DEBUG("Entry %2d: %s config for interface '%s'", line_num, filepath, cfg->interface);

  fp = fopen(filepath, "w");
  if (!fp)
    {
        r = -errno;
        MSG_ERROR("Failed to open network file '%s' for writing: %s", filepath, strerror(-r));
        return r;
    }

  /* ------------------------------ [Match] Section ------------------------------ */
  fputs("[Match]\n", fp);
  if (vlanid > 0)
    {
        fprintf(fp, "Name=vlan%04d\n", vlanid);
        fputs("Type=vlan\n", fp);
    }
  else if (physical_interfaces_only && (isempty(cfg->interface) || streq(cfg->interface, "*")))
    {
        fputs("Kind=!*\nType=!loopback\n", fp);
    }
  else
    {
        /* Heuristic: If the interface contains ':', assume MAC. Otherwise Name (supports globs like eth*). */
        if (strchr(cfg->interface, ':'))
            fprintf(fp, "Name=*\nMACAddress=%s\n", cfg->interface);
        else
            fprintf(fp, "Name=%s\n", cfg->interface);
    }

  /* ------------------------------ [Link] Section ------------------------------- */
  if (!isempty(cfg->mtu) || !isempty(cfg->macaddr))
    {
      fputs("\n[Link]\n", fp);
      if (!isempty(cfg->macaddr))
        fprintf(fp, "MACAddress=%s\n", cfg->macaddr);
      if (!isempty(cfg->mtu))
        fprintf(fp, "MTUBytes=%s\n", cfg->mtu);
    }

  /* ----------------------------- [Network] Section ----------------------------- */
  if (!isempty(cfg->autoconf) || !isempty(cfg->dns1) || !isempty(cfg->dns2) ||
      !isempty(cfg->domains) || !isempty(cfg->ntp) || cfg->vlan1 ||
      !isempty(cfg->client_ip))
    {
      fputs("\n[Network]\n", fp);

      if (!isempty(cfg->client_ip) && cfg->netmask < 0)
        {
          /* Space-separated multi-IP write fallback */
          /* Complexer addresses will be written in the [Address] section */
          r = split_and_write(fp, "Address", cfg->client_ip);
          if (r < 0) return r;
        }

      /* Write single-line Gateway entry if defined via split_and_write */
      if (cfg->gateways_count == 1 &&
          !isempty(cfg->gateways[0]) && isempty(cfg->destinations[0]))
        {
          r = split_and_write(fp, "Gateway", cfg->gateways[0]);
          if (r < 0) return r;
        }

      if (!isempty(cfg->autoconf_networkd))
        {
          fprintf(fp, "DHCP=%s\n", cfg->autoconf_networkd);
          if (streq(cfg->autoconf, "off"))
            {
              fputs("LinkLocalAddressing=no\n"
                    "IPv6AcceptRA=no\n", fp);
            }
        }

      /* Write DNS settings (supports single values or space-separated lists) */
      if (!isempty(cfg->dns1))
        {
          r = split_and_write(fp, "DNS", cfg->dns1);
          if (r < 0) return r;
        }
      if (!isempty(cfg->dns2))
        {
          r = split_and_write(fp, "DNS", cfg->dns2);
          if (r < 0) return r;
        }

      if (!isempty(cfg->domains))
        fprintf(fp, "Domains=%s\n", cfg->domains);

      if (!isempty(cfg->ntp))
        fprintf(fp, "NTP=%s\n", cfg->ntp);

      /* VLAN memberships */
      if (cfg->vlan1 && (r = write_vlan_entry(fp, cfg->vlan1)) != 0)
        return r;
      if (cfg->vlan2 && (r = write_vlan_entry(fp, cfg->vlan2)) != 0)
        return r;
      if (cfg->vlan3 && (r = write_vlan_entry(fp, cfg->vlan3)) != 0)
        return r;
    }

  /* ------------------------------ [DHCP] Section --------------------------------- */
  if (!isempty(cfg->hostname) || cfg->use_dns > 0)
    {
      fputs("\n[DHCP]\n", fp);
      if (cfg->hostname)
        fprintf(fp, "Hostname=%s\n", cfg->hostname);
      if (cfg->use_dns == 1)
        fputs("UseDNS=no\n", fp);
      else if (cfg->use_dns == 2)
        fputs("UseDNS=yes\n", fp);
    }

  if (!isempty(cfg->autoconf_networkd))
   {
     /* ----------------------------- [DHCPv4] Section ------------------------------ */
     if (streq(cfg->autoconf_networkd, "yes") || streq(cfg->autoconf_networkd, "ipv4"))
       write_dhcp(fp, "DHCPv4", rfc2132);

     /* ----------------------------- [DHCPv6] Section ------------------------------ */
     if (streq(cfg->autoconf_networkd, "yes") || streq(cfg->autoconf_networkd, "ipv6"))
       write_dhcp(fp, "DHCPv6", false); /* rfc2132 does not matter here */
   }

  /* ----------------------------- [Address] Section ----------------------------- */
  if (!isempty(cfg->client_ip) && cfg->netmask >= 0)
    {
      fputs("\n[Address]\n", fp);
      /* Primary CIDR assignment */
      fprintf(fp, "Address=%s/%d\n", cfg->client_ip, cfg->netmask);
      if (!isempty(cfg->peer_ip))
        fprintf(fp, "Peer=%s\n", cfg->peer_ip);
    }

  /* ------------------------------ [Route] Section ------------------------------ */
  if (cfg->gateways_count > 1 ||
      (cfg->gateways_count == 1 && !isempty(cfg->destinations[0])))
    {
      for (int i = cfg->gateways_count - 1; i >= 0; i--)
        {
          fputs("\n[Route]\n", fp);
          if (!isempty(cfg->destinations[i]))
            fprintf(fp, "Destination=%s\n", cfg->destinations[i]);
          if (!isempty(cfg->gateways[i]))
            fprintf(fp, "Gateway=%s\n", cfg->gateways[i]);
        }
    }

  /* Close explicitly before invoking external file writers */
  fflush(fp);

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
               output_dir, vlan->prefix, vlan->name) < 0)
    return -ENOMEM;

  MSG_DEBUG("Creating vlan netdev: %s for vlan id '%d'", filepath,
	    vlan->id);

  fp = fopen(filepath, "w");
  if (!fp)
    {
      r = -errno;
      MSG_ERROR("Failed to open network file '%s' for writing: %s",
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


int
register_vlan_netdev(int vlanid, const char *name, const char *prefix)
{
  for (int i = 0; i < nr_vlanids; i++)
    if (vlans[i].id == vlanid)
      {
	if (!streq(vlans[i].prefix, prefix))
	  {
	    MSG_ERROR("VLAN ID %d already configured as '%s' via '%s', cannot "
		      "also configure it as '%s' via '%s': conflicting VLAN configuration",
		      vlanid, vlans[i].name, vlans[i].prefix, name, prefix);
	    return -EEXIST;
	  }
	return 0;
      }

  if ((nr_vlanids+1) == vlan_capacity)
    {
      MSG_ERROR("Too many vlans!");
      return -ENOMEM;
    }

  vlans[nr_vlanids].id = vlanid;
  vlans[nr_vlanids].name = name;
  vlans[nr_vlanids].prefix = prefix;
  nr_vlanids++;
  MSG_DEBUG("Stored VLAN ID: %d (%s)", vlanid, name);

  return 0;
}

int
append_route_settings(const char *gateway, const char *destination, ip_t *cfg)
{
  if (cfg->gateways_count >= MAX_GATEWAYS)
    {
      MSG_ERROR("Too many gateways specified!");
      return -ENOMEM;
    }
  cfg->gateways[cfg->gateways_count] = gateway;
  cfg->destinations[cfg->gateways_count] = destination;
  cfg->gateways_count++;
  return 0;
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
        int r;

	p++;
	l = strtol(p, &ep, 10);
	// valid: 1 <= VLAN ID <= 4095
	if (errno == ERANGE || l < 1 || l > 4095 ||
	    p == ep || *ep != '\0')
	  {
	    MSG_ERROR("Invalid VLAN interface: %s", vlan_name);
	    return -EINVAL;
	  }
	vlanid = l;

        r = register_vlan_netdev(vlanid, vlan_name, NETDEV_PREFIX);
	if (r < 0)
	  return r;

	*ret = vlanid;
	return 0;
      }

  return -EINVAL;
}

int
return_syntax_error(int nr, const char *value, const int ret)
{
  MSG_ERROR("Syntax error in entry %d: '%s'", nr, value);
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
  fprintf(stdout, "rdii-networkd - create networkd config files\n\n");
  print_usage(stdout);

  fputs("  -a, --parse-all      Parse all network options from kernel cmdline\n", stdout);
  fputs("  -c, --config <file>  File with configuration\n", stdout);
  fputs("  -d, --debug          Write config to stdout\n", stdout);
  fputs("  -o, --output         Directory in which to write config\n", stdout);
  fputs("      --verify         Verify input, don't write config\n", stdout);
  fputs("  -h, --help           Give this help list\n", stdout);
  fputs("  -v, --version        Print program version\n", stdout);
}

static void
print_error(void)
{
  MSG_ERROR("Try `rdii-networkd --help' for more information.");
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
  bool verify_only = false;
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
	  {"verify",    no_argument,       NULL, '\254' },
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
          set_max_log_level(LOG_LEVEL_DEBUG);
          break;
	case 'o':
	  output_dir = optarg;
	  break;
	case '\254':
	  verify_only = true;
	  break;
        case 'h':
          print_help();
          return 0;
        case 'v':
          MSG_INFO("rdii-networkd (%s) %s", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return EINVAL;
        }
    }

  argc -= optind;
  argv += optind;

  if (!isempty(cfgfile) && argc > 0)
    {
      MSG_ERROR("Using a configuration file with additional arguments is not possible");
      print_error();
      return EINVAL;
    }

  if (stat(output_dir, &st) == -1)
    {
      if (mkdir_p(output_dir, 0755) == -1 && errno != EEXIST)
	{
	  r = errno;
	  MSG_ERROR("Could not create output directory: %s",
	         strerror(r));
	  return r;
	}
    }

  if (isempty(cfgfile) &&
      access(RUN_RDII_CONFIG, F_OK) == 0)
    cfgfile = RUN_RDII_CONFIG;

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
	  MSG_ERROR("Out of memory!");
	  return ENOMEM;
	}

      char *cp = line;

      for (int i = 0; i < argc; i++)
	{
	  cp = stpcpy(cp, argv[i]);
	  if (i < argc - 1) // not last argument
	    cp = stpcpy(cp, " ");
	}

      parse_all = true;
    }
  else if (!isempty(cfgfile))
    {
      _cleanup_close_ int fd = -EBADF;
      size_t file_size = 0;

      fd = open(cfgfile, O_RDONLY|O_NOCTTY|O_CLOEXEC);
      if (fd == -1)
	{
	  r = errno;
	  MSG_ERROR("Error opening '%s': %s",
                 cfgfile, strerror(r));
	  return r;
	}

      if (fstat(fd, &st) == -1)
	{
	  r = errno;
	  MSG_ERROR("fstat(%s) failed: %s",
                 cfgfile, strerror(r));
	  return r;
	}
      file_size = st.st_size;
      line = malloc(file_size);
      if (line == NULL)
	{
	  MSG_ERROR("Out of memory!");
	  return ENOMEM;
	}

      size_t total_read = 0;
      while (total_read < file_size)
	{
	  ssize_t bytes_read_now = read(fd, line + total_read,
					file_size - total_read);

	  if (bytes_read_now == -1)
	    {
	      r = errno;
	      if (r == EINTR) // signal interrupt, try again
                continue;
	      else
		{
		  MSG_ERROR("Error reading config file: %s",
			 strerror(r));
		  return r;
		}
	    }
	  if (bytes_read_now == 0)
            break;

	  total_read += bytes_read_now;
	}

      line[total_read] = '\0';

      char *ptr = line;
      while (*ptr)
	{
	  if (*ptr == '\n')
            *ptr = ' ';
	  ptr++;
	}

      parse_all = true;
    }
  else
    {
      fp = fopen(CMDLINE_PATH, "r");
      if (!fp)
	{
	  r = errno;
	  MSG_ERROR("Failed to open %s: %s",
		 CMDLINE_PATH, strerror(r));
	  return r;
	}

      nread = getline(&line, &line_size, fp);
      if (nread == -1)
	{
	  r = errno;
	  MSG_ERROR("Failed to read %s: %s",
		 CMDLINE_PATH, strerror(r));
	  return r;
	}
      if (nread > 0 && line[nread-1] == '\n')
	line[nread-1] = '\0';
    }

  MSG_DEBUG("cmdline=%s", line);

  init_configs();

  // Parse loop handling quotes
  char *cp = line;
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
		    MSG_ERROR("Skip '%s' due to errors", val);
		}
	    }
	  else if (parse_all)
	    {
	      ip_t cfg = {0};
	      bool merge = true;

	      cfg.netmask = -1;

	      // this options are normally handled by systemd-network-generator
	      if (startswith(arg_start, "ip="))
		r = parse_ip_arg(nr++, arg_start+3, &cfg);
	      else if (startswith(arg_start, "nameserver="))
		r = parse_nameserver_arg(nr++, arg_start+11, &cfg);
	      else if (startswith(arg_start, "rd.peerdns="))
		r = parse_rd_peerdns_arg(nr++, arg_start+11, &cfg);
	      else if (startswith(arg_start, "rd.route="))
		r = parse_rd_route_arg(nr++, arg_start+9, &cfg);
	      else if (startswith(arg_start, "vlan="))
		r = parse_vlan_arg(nr++, arg_start+5, &cfg);
	      else
		{
		  MSG_DEBUG("skip: '%s'", arg_start);
		  merge = false;
		}

	      if (r < 0)
		return -r;

	      if (merge)
		{
                  r = merge_configs(&cfg);
	          if (r < 0)
		    return -r;
		}
	    }
	  arg_start = cp + 1;
	}
      cp++;
    }

  if (verify_only) // don't write configs
    return 0;

  // write networkd config files
  for (int i = 0; i < used_configs; i++)
    {
      r = write_network_config(output_dir, IP_PREFIX, i+1, &configs[i], false, true, 0);
      if (r < 0)
	{
	  MSG_ERROR("Error writing .network files: %s",
		  strerror(-r));
	  return -r;
	}
    }

  if (nr_vlanids > 0)
    {
      r = write_netdev_config(output_dir);
      if (r < 0)
	{
	  MSG_ERROR("Error writing .netdev files: %s",
		  strerror(-r));
	  return -r;
	}
    }

  return 0;
}
