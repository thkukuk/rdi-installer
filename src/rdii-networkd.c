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

static const char *output_dir = "/run/systemd/network";

/* Configuration */
#define CMDLINE_PATH "/proc/cmdline"

int
return_syntax_error(const char *value, const int ret)
{
  fprintf(stderr, "Syntax error: '%s'\n", value);
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
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;
  const char *cfgfile = NULL;
  struct stat st;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"config",  required_argument, NULL, 'c' },
          {"debug",   no_argument,       NULL, 'd' },
	  {"output",  required_argument, NULL, 'o' },
	  {"help",    no_argument,       NULL, 'h' },
          {"version", no_argument,       NULL, 'v' },
          {NULL,      0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "c:do:hv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
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
	  line_count++;

	  if (isempty(line) || line[0] == '#' || line[0] == '\n')
	    continue;

	  if (line[nread-1] == '\n')
	    line[nread-1] = '\0';

	  if (startswith(line, "ip="))
	    r = parse_ip_arg(output_dir, line_count, line+3);
	  else if (startswith(line, "ifcfg="))
	    r = parse_ifcfg_arg(output_dir, line_count, line+6);
	  else
	    return return_syntax_error(line, -EINVAL);

	  if (r < 0)
	    return -r;
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
	      arg_start = cp + 1;
	    }
	  cp++;
	}
    }

  // XXX for ifcfg, we don't know if necessary
  r = create_netdev_files(output_dir);
  if (r < 0)
    {
      fprintf(stderr, "Error writing .netdev files: %s\n",
	      strerror(-r));
      return -r;
    }

  return 0;
}
