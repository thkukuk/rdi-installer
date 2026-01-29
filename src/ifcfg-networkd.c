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

static bool debug = false;

static const char *output_dir = "/run/systemd/network";

/* Configuration */
#define CMDLINE_PATH "/proc/cmdline"
#define FILE_PREFIX "60-ifcfg"

/* Helper to trim whitespace */
static char *
trim_whitespace(char *str)
{
  char *end;

  if (!str)
    return NULL;

  while(isspace((unsigned char)*str))
    str++;
  if(*str == 0)
    return str;
  end = str + strlen(str) - 1;

  while(end > str && isspace((unsigned char)*end))
    end--;

  *(end+1) = 0;
  return str;
}

static int
split_and_print(FILE *fp, const char *key, const char *list)
{
  char *token = NULL, *saveptr = NULL;
  _cleanup_free_ char *copy = NULL;

  if (isempty(list))
    return 0;

  copy = strdup(list);
  if (!copy)
    return -ENOMEM;

  token = strtok_r(copy, " ", &saveptr);
  while (token)
    {
      fprintf(fp, "%s=%s\n", key, token);
      token = strtok_r(NULL, " ", &saveptr);
    }
  return 0;
}


/* Writes the systemd-networkd .network file */
static int
write_network_file(int nr, const char *interface, int is_dhcp,
		   int dhcp_v4, int dhcp_v6, int rfc2132, char *ip_list,
		   char *gw_list, char *dns_list, char *domains)
{
  _cleanup_free_ char *filepath = NULL;
  _cleanup_fclose_ FILE *fp = NULL;
  int r;

  if (asprintf(&filepath, "%s/%s-%02d.network",
	       output_dir, FILE_PREFIX, nr) < 0)
    return -ENOMEM;

  printf("Creating config: %s for interface '%s'\n", filepath, interface);

  if (debug)
    fp = stdout;
  else
    {
      fp = fopen(filepath, "w");
      if (!fp)
	{
	  r = -errno;
	  fprintf(stderr, "Failed to open network file '%s' for writing: %s",
		  filepath, strerror(-r));
	  return r;
	}
    }

  /* [Match] Section: */
  fprintf(fp, "[Match]\n");
  /* Heuristic: If the interface contains ':', assume MAC.
     Otherwise Name (supports globs like eth*). */
  if (strchr(interface, ':'))
    fprintf(fp, "Name=*\nMACAddress=%s\n", interface);
  else
    fprintf(fp, "Name=%s\n", interface);

  /* [Network] Section: */
  fprintf(fp, "\n[Network]\n");

  if (is_dhcp)
    {
      if (dhcp_v4 && dhcp_v6)
	fprintf(fp, "DHCP=yes\n");
      else if (dhcp_v4)
	fprintf(fp, "DHCP=ipv4\n");
      else if (dhcp_v6)
	fprintf(fp, "DHCP=ipv6\n");
    }

  /* Static IPs (space separated) */
  r = split_and_print(fp, "Address", ip_list);
  if (r < 0)
    return r;

  r = split_and_print(fp, "Gateway", gw_list);
  if (r < 0)
    return r;

  r = split_and_print(fp, "DNS", dns_list);
  if (r < 0)
    return r;

  if (!isempty(domains))
    fprintf(fp, "Domains=%s\n", domains);

  /* DHCP Specific Options */
  if (is_dhcp)
    {
      if (dhcp_v4)
	{
	  fprintf(fp, "\n[DHCPv4]\n");
	  fprintf(fp, "UseHostname=false\n");
	  fprintf(fp, "UseDNS=true\n");
	  fprintf(fp, "UseNTP=true\n");

	  if (rfc2132)
	    fprintf(fp, "ClientIdentifier=mac\n");
	}
      if (dhcp_v6)
	{
	  fprintf(fp, "\n[DHCPv6]\n");
	  fprintf(fp, "UseHostname=false\n");
	  fprintf(fp, "UseDNS=true\n");
	  fprintf(fp, "UseNTP=true\n");
	}
    }

  if (debug)
    fp = NULL;

  return 0;
}

/* Parses a single ifcfg string */
static int
parse_ifcfg_arg(int nr, char *arg)
{
  char *interface = NULL;
  char *config = NULL;
  /* dhcp */
  int is_dhcp = 0;
  int dhcp_v4 = 1;
  int dhcp_v6 = 1;
  int rfc2132 = 0;

  // Syntax: <interface>=<config>
  interface = arg;
  config = strchr(arg, '=');
  if (!config)
    {
      fprintf(stderr, "Error: Malformed format. Expected 'ifcfg=<iface>=...'\n");
      return -EINVAL;
    }
  *config++ = '\0';

  if (!interface || !config)
    return -ENOENT;

  printf("Configuration Found:\n");
  printf("Interface - Config: '%s' - '%s'\n", interface, config);

  // Format: IP_LIST,GATEWAY_LIST,NAMESERVER_LIST,DOMAINSEARCH_LIST
  char *ip_list = trim_whitespace(strsep(&config, ","));
  char *gw_list = trim_whitespace(strsep(&config, ","));
  char *dns_list = trim_whitespace(strsep(&config, ","));
  char *domains = trim_whitespace( strsep(&config, ","));

  if (strneq(ip_list, "dhcp", 4))
    {
      is_dhcp = 1;
      if (streq(ip_list, "dhcp4"))
	dhcp_v6 = 0;
      else if (streq(ip_list, "dhcp6"))
	dhcp_v4 = 0;
      else if (streq(ip_list, "dhcp"))
	{ /* both */ }

      if (!isempty(gw_list) && streq(gw_list, "rfc2132"))
	rfc2132 = 1;
    }

  write_network_file(nr, interface, is_dhcp, dhcp_v4, dhcp_v6,
		       rfc2132, ip_list, gw_list, dns_list, domains);

    return 0;
}

  static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: ifcfg-networkd [--help]|[--version]|[--debug]\n");
}

static void
print_help(void)
{
  fprintf(stdout, "ifcfg-networkd - wait for key pressed or timeout\n\n");
  print_usage(stdout);

  fputs("  -d, --debug     Write config to stdout\n", stdout);
  fputs("  -h, --help      Give this help list\n", stdout);
  fputs("  -v, --version   Print program version\n", stdout);
}

static void
print_error(void)
{
  fputs("Try `ifcfg-networkd --help' for more information.\n", stderr);
}

/* Reads /proc/cmdline and parses quoted arguments */
int
main(int argc, char *argv[])
{
  _cleanup_free_ char *cmdline = NULL;
  _cleanup_fclose_ FILE *f = NULL;
  struct stat st;
  size_t len = 0;
  char *cp;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
          {"debug",   no_argument, NULL, 'd' },
	  {"help",    no_argument, NULL, 'h' },
          {"version", no_argument, NULL, 'v' },
          {NULL,      0,           NULL, '\0'}
        };

      c = getopt_long (argc, argv, "dhv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
        case 'd':
	  debug = true;
          break;
        case 'h':
          print_help();
          return 0;
        case 'v':
          printf("expiry (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return 1;
        }
    }

  argc -= optind;
  argv += optind;

  if (!debug && stat(output_dir, &st) == -1)
    {
      // XXX use mkdir_p
      if (mkdir(output_dir, 0755) == -1 && errno != EEXIST)
	{
	  r = errno;
	  fprintf(stderr, "Could not create output directory: %s\n",
		  strerror(r));
	  return r;
	}
    }

  // Allow overriding input for testing: ifcfg-networkd "ifcfg=..."
  if (argc > 0)
    {
      size_t total_length = 0;

      for (int i = 0; i < argc; i++)
	{
	  total_length += strlen(argv[i]);
	  total_length++; // for ' ' or '\0'
	}

      cmdline = malloc(total_length);
      if (cmdline == NULL)
	{
	  fprintf(stderr, "Out of memory!\n");
	  return ENOMEM;
	}

      cp = cmdline;

      for (int i = 0; i < argc; i++)
	{
	  cp = stpcpy(cp, argv[i]);
	  if (i < argc - 1) // not last argument
	  cp = stpcpy(cp, " ");
	}
    }
  else
    {
      f = fopen(CMDLINE_PATH, "r");
      if (!f)
	{
	  r = errno;
	  fprintf(stderr, "Failed to open %s: %s",
		  CMDLINE_PATH, strerror(r));
	  return r;
	}

      ssize_t read = getline(&cmdline, &len, f);
      if (read == -1)
	{
	  fprintf(stderr, "Failed to read %s: %s",
		  CMDLINE_PATH, strerror(r));
	  return errno;

	  if (read > 0 && cmdline[read-1] == '\n')
	    cmdline[read-1] = '\0';
	}
    }

  if (debug)
    printf("cmdline=%s\n", cmdline);

  // Parse loop handling quotes
  cp = cmdline;
  char *arg_start = cp;
  int in_quote = 0;
  int nr = 0;

  while (*cp)
    {
      if (*cp == '"')
	in_quote = !in_quote;

      if (cp[1] == '\0' || (*cp == ' ' && !in_quote))
	{
	  if (*cp == ' ')
	    *cp = '\0'; // Terminate current arg

	  if (strneq(arg_start, "ifcfg=", 6))
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
	      parse_ifcfg_arg(nr++, val);
	    }
	  arg_start = cp + 1;
	}
      cp++;
    }

    return 0;
}
