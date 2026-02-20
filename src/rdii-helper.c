// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <getopt.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>

#include "basics.h"
#include "efivars.h"
#include "rdii-helper.h"

static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: rdii-helper [command]|[--help]|[--version]\n");
}

void
print_help(void)
{
  fputs("rdii-helper - Helper functions for rdi-installer\n\n", stdout);
  print_usage(stdout);

  fputs("Commands: boot, disk\n\n", stdout);

  fputs ("Options for boot:\n", stdout);
  fputs("  -d, --debug       Print debug information\n", stdout);
  fputs ("\n", stdout);

  fputs ("Options for disk:\n", stdout);
  fputs("  -a, -all          Print all devices, even if not suitable\n", stdout);
  fputs("  -d, --debug       Print debug information\n", stdout);
  fputs ("\n", stdout);

  fputs ("Generic options:\n", stdout);
  fputs("  -h, --help        Give this help list\n", stdout);
  fputs("  -v, --version     Print program version\n", stdout);
}

void
print_error(void)
{
  fputs("Try `rdii-helper --help' for more information.\n", stderr);
}

static int
main_boot(int argc, char **argv)
{
  _cleanup_efivars_ efivars_t *efi = NULL;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"debug",      no_argument,       NULL, 'd' },
          {"help",       no_argument,       NULL, 'h' },
          {"version",    no_argument,       NULL, 'v' },
          {NULL,         0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "dhv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
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
          return EINVAL;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 0)
    {
      fprintf(stderr, "rdii-helper boot: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  r = efi_get_boot_source(&efi);
  if (r < 0)
    {
      fprintf(stderr, "Couldn't get boot source: %s\n", strerror(-r));
      return -r;
    }

  printf("Boot Entry:    %s\n", strna(efi->entry));
  printf("PXE Boot:      %s\n", efi->is_pxe_boot?"yes":"no");
  printf("Loader Device: %s\n", strna(efi->device));
  printf("Loader URL:    %s\n", strna(efi->url));
  printf("Loader Image:  %s\n", strna(efi->image));

  return 0;
}

int
main(int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {NULL, 0, NULL, '\0'}
  };
  int c;

  if (argc == 1)
    {
      fprintf(stderr, "rdii-helper: no commands or options provided.\n");
      print_error();
      return EINVAL;
    }

  if (streq(argv[1], "boot"))
    return main_boot(--argc, ++argv);
  else if (streq(argv[1], "disk"))
    return main_disk(--argc, ++argv);

  while ((c = getopt_long (argc, argv, "hv", longopts, NULL)) != -1)
    {
      switch (c)
        {
	case 'h':
          print_help();
          return 0;
        case 'v':
          printf("rdii-helper (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return EINVAL;
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

  return 0;
}
