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
#include "exec_cmd.h"
#include "logger.h"

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

  fputs("Commands: boot, disk, set-default-loader-entry\n\n", stdout);

  fputs("Options for boot:\n", stdout);
  fputs("  -d, --debug       Print debug information\n", stdout);
  fputs("\n", stdout);

  fputs("Options for disk:\n", stdout);
  fputs("  -a, --all         Print all devices, even if not suitable\n", stdout);
  fputs("  -d, --debug       Print debug information\n", stdout);
  fputs("\n", stdout);

  fputs("Options for set-default-loader-entry:\n", stdout);
  fputs("  -d, --debug       Print debug information\n", stdout);
  fputs("  -V, --verbose     Print information about changes\n", stdout);
  fputs("\n", stdout);

  fputs("Generic options:\n", stdout);
  fputs("  -h, --help        Give this help list\n", stdout);
  fputs("  -v, --version     Print program version\n", stdout);
}

void
print_error(void)
{
  MSG_ERROR("Try `rdii-helper --help' for more information.");
}

static int
main_boot(int argc, char **argv)
{
  _cleanup_efivars_ efivars_t *efi = NULL;
  _cleanup_free_ char *defloaderentry = NULL;
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
          MSG_INFO("rdii-helper (%s) %s\n", PACKAGE, VERSION);
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
      MSG_ERROR("rdii-helper boot: Too many arguments.");
      print_error();
      return EINVAL;
    }

  r = efi_get_boot_source(&efi);
  if (r < 0)
    {
      MSG_ERROR("Couldn't get boot source: %s", strerror(-r));
      return -r;
    }

  r = efi_get_default_loader_entry(&defloaderentry);
  if (r < 0)
    {
      MSG_ERROR("Couldn't get default loader entry: %s",
	     strerror(-r));
    }

  MSG_INFO("Boot Entry:            %s", strna(efi->entry));
  MSG_INFO("Default Loader Entry:  %s", strna(defloaderentry));
  MSG_INFO("PXE Boot:              %s", efi->is_pxe_boot?"yes":"no");
  MSG_INFO("Loader Partition:      %s", strna(efi->partition));
  MSG_INFO("Loader URL:            %s", strna(efi->url));
  MSG_INFO("Loader Image:          %s", strna(efi->image));
  MSG_INFO("Default EFI Partition: %s", strna(efi->def_efi_partition));
  return 0;
}

static int
main_set_default_loader_entry(int argc, char **argv)
{
  _cleanup_efivars_ efivars_t *efi = NULL;
  _cleanup_free_ char *defloaderentry = NULL;
  bool verbose = false;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"debug",      no_argument,       NULL, 'd' },
          {"help",       no_argument,       NULL, 'h' },
	  {"verbose",    no_argument,       NULL, 'V' },
          {"version",    no_argument,       NULL, 'v' },
          {NULL,         0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "dhvV",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
	case 'd':
	  _efivars_debug = true;
          break;
	case 'V':
	  verbose = true;
	  break;
	case 'h':
          print_help();
          return 0;
        case 'v':
          MSG_INFO("rdii-helper (%s) %s", PACKAGE, VERSION);
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
      MSG_ERROR("rdii-helper set-default-loader-entry: Too many arguments.");
      print_error();
      return EINVAL;
    }

  r = efi_get_boot_source(&efi);
  if (r < 0)
    {
      MSG_ERROR("Couldn't get boot source: %s", strerror(-r));
      return -r;
    }

  if (isempty(efi->entry))
    {
      MSG_ERROR("LoaderEntrySelected not set");
      return ENOENT;
    }

  r = efi_get_default_loader_entry(&defloaderentry);
  if (r < 0)
    MSG_ERROR("Couldn't get default loader entry: %s",
	   strerror(-r));

  // if booted and default entry are equal, all is fine
  if (streq(strempty(efi->entry), strempty(defloaderentry)))
    {
      if (verbose)
	MSG_INFO("Booted and default entry are equal, no changes done.");
      return 0;
    }

  if (verbose)
    MSG_INFO("Setting LoaderEntryDefault to '%s'", efi->entry);

  r = exec_cmd("sdbootutil", "sdbotutil", "set-default", efi->entry, NULL);
  if (r < 0)
    {
      MSG_ERROR("Failed to run sdbootutil: %s", strerror(-r));
      return -r;
    }
  if (r > 0)
    {
      if (r > 128) // aborted by signal
        {
          int sig = r - 128;
          MSG_ERROR("sdbootutil got terminated by signal %d (%s)",
                 sig, strsignal(sig));
        }
      else
        MSG_ERROR("sdbootutil failed with exit code %i", r);

      return ECHILD;
    }
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
      MSG_ERROR("rdii-helper: no commands or options provided.");
      print_error();
      return EINVAL;
    }

  if (streq(argv[1], "boot"))
    return main_boot(--argc, ++argv);
  else if (streq(argv[1], "disk"))
    return main_disk(--argc, ++argv);
  else if (streq(argv[1], "set-default-loader-entry"))
    return main_set_default_loader_entry(--argc, ++argv);

  while ((c = getopt_long(argc, argv, "hv", longopts, NULL)) != -1)
    {
      switch (c)
        {
	case 'h':
          print_help();
          return 0;
        case 'v':
          MSG_INFO("rdii-helper (%s) %s", PACKAGE, VERSION);
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
      MSG_ERROR("rdii-helper: Too many arguments.");
      print_error();
      return EINVAL;
    }

  return 0;
}
