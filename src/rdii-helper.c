// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <getopt.h>

#include "basics.h"
#include "efivars.h"


static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: rdii-helper [--help]|[--version]|[...]\n");
}

static void
print_help(void)
{
  fprintf(stdout, "rdii-helper - Helper functions for rdi-installer\n\n");
  print_usage(stdout);

  fputs("  -b, --boot        Print boot path\n", stdout);
  fputs("  -d, --debug       Print debug informations\n", stdout);
  fputs("  -h, --help        Give this help list\n", stdout);
  fputs("  -v, --version     Print program version\n", stdout);
}

static void
print_error(void)
{
  fputs("Try `rdii-helper --help' for more information.\n", stderr);
}


int
main(int argc, char **argv)
{
  _cleanup_free_ char *loader_url = NULL;
  _cleanup_free_ char *loader_dev = NULL;
  _cleanup_free_ char *loader_img = NULL;
  bool boot = false;
  int r;

    while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"boot",       no_argument,       NULL, 'b' },
          {"debug",      no_argument,       NULL, 'd' },
          {"help",       no_argument,       NULL, 'h' },
          {"version",    no_argument,       NULL, 'v' },
          {NULL,         0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "bdhv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
	case 'b':
	  boot = true;
	  break;
        case 'd':
          _efivars_debug = true;
          break;
	case 'h':
          print_help();
          return 0;
        case 'v':
          printf("rdii-helper (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return 1;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 0)
    {
      fprintf(stderr, "rdii-helper: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  if (boot)
    {
      _cleanup_efivars_ efivars_t *efi = {0};
      r = efi_get_boot_source(&efi);
      if (r < 0)
	{
	  fprintf(stderr, "Couldn't get boot source: %s\n", strerror(-r));
	  return -r;
	}

      printf("Boot Entry:    %s\n", strna(efi->entry));
      printf("Loader Device: %s\n", strna(efi->device));
      printf("Loader URL:    %s\n", strna(efi->url));
      printf("Loader Image:  %s\n", strna(efi->image));
    }
  else
    {
      print_error();
      return 1;
    }

  return 0;
}
