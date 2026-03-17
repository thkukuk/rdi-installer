// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <getopt.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include <libudev.h>

#include "basics.h"
#include "efivars.h"
#include "devices.h"
#include "rdii-helper.h"

/* simple helper function, not very robust */
static int
parse_size(const char *str, uint64_t *res)
{
  char *ep;
  uint64_t size;
  unsigned long long ull_size = strtoull(str, &ep, 10);
  if (ull_size == ULLONG_MAX && errno == ERANGE)
    return -ERANGE;

  if (ull_size > UINT64_MAX)
    return -ERANGE;

  size = ull_size;

  if (!isempty(ep))
    {
      uint64_t old_size = size;
      if (toupper(*ep) == 'G')
	size *= 1024ULL * 1024 * 1024;
      else if (toupper(*ep) == 'M')
	size *= 1024ULL * 1024;
      else if (toupper(*ep) == 'T')
	size *= 1024ULL * 1024 * 1024 * 1024;

      /* XXX that's not good enough for Terrabyte... */
      if (size < old_size) // overflow
	return -ERANGE;
    }

  *res = size;
  return 0;
}

static inline const char *strunknown(const char *s) {
        return s ?: "Unknown";
}

int
main_disk(int argc, char **argv)
{
  uint64_t minsize = 10 * 1000ULL * 1000 * 1000; // 10G min disk size
  bool all_devices = false;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"all",        no_argument,       NULL, 'a' },
	  {"debug",      no_argument,       NULL, 'd' },
	  {"minsize",    required_argument, NULL, 's' },
          {"help",       no_argument,       NULL, 'h' },
          {"version",    no_argument,       NULL, 'v' },
          {NULL,         0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "ads:hv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
	case 'a':
	  all_devices = true;
	  break;
	case 'd':
	  _efivars_debug = true;
          break;
	case 's':
	  r = parse_size(optarg, &minsize);
	  if (r < 0)
	    {
	      fprintf(stderr, "Error parsing '%s': %s\n",
		      optarg, strerror(-r));
	      return -r;
	    }
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
      fprintf(stderr, "rdii-helper disk: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  _cleanup_(devices_freep) device_t *disk = NULL;
  int count;
  r = get_devices(&disk, &count);

  for (int i = 0; i < count; i++)
    {
      if (!all_devices)
	{
	  if (!isempty(disk[i].type) && !streq(disk[i].type, "disk"))
	    continue;
	  if (disk[i].size < minsize)
	    continue;
	}
      printf("%s - %s (%s, %.1f GB)", disk[i].device,
	     strunknown(disk[i].model), disk[i].bus, disk[i].size_gb);
      if (disk[i].is_default_device)
	fputs(" [Default]", stdout);
      if (disk[i].is_boot_device)
	fputs(" [Booted]", stdout);
      fputs("\n", stdout);
    }

  return 0;
}
