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
#include "logger.h"

/* Parse a size with an optional K/M/G/T suffix (binary units). */
static int
parse_size(const char *str, uint64_t *res)
{
  const char *p = str;
  uint64_t size, multiplier = 1;
  char *ep;

  if (isempty(str))
    return -EINVAL;

  while (isspace((unsigned char)*p))
    p++;
  if (*p == '-') /* strtoull() would silently wrap this around */
    return -ERANGE;

  errno = 0;
  size = strtoull(p, &ep, 10);
  if (errno == ERANGE)
    return -ERANGE;
  if (ep == p) /* no digits at all */
    return -EINVAL;

  switch (toupper((unsigned char)*ep))
    {
    case '\0':
      break;
    case 'K':
      multiplier = 1024ULL;
      ep++;
      break;
    case 'M':
      multiplier = 1024ULL * 1024;
      ep++;
      break;
    case 'G':
      multiplier = 1024ULL * 1024 * 1024;
      ep++;
      break;
    case 'T':
      multiplier = 1024ULL * 1024 * 1024 * 1024;
      ep++;
      break;
    default:
      return -EINVAL;
    }

  if (!isempty(ep)) /* trailing garbage */
    return -EINVAL;

  if (size > UINT64_MAX / multiplier)
    return -ERANGE;

  *res = size * multiplier;
  return 0;
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
	      MSG_ERROR("Error parsing '%s': %s",
		     optarg, strerror(-r));
	      return -r;
	    }
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
      MSG_ERROR("rdii-helper disk: Too many arguments.");
      print_error();
      return EINVAL;
    }

  _cleanup_(devices_freep) device_t *disk = NULL;
  int count;
  r = get_devices(&disk, &count);
  if (r < 0)
    {
      MSG_ERROR("Getting list of devices failed: %s", strerror(-r));
      return -r;
    }

  for (int i = 0; i < count; i++)
    {
      if (!all_devices)
	{
	  if (!isempty(disk[i].type) && !streq(disk[i].type, "disk"))
	    continue;
	  if (disk[i].size < minsize)
	    continue;
	}

      char *kind = "";
      if (disk[i].is_default_device)
        kind = " [Default]";
      if (disk[i].is_boot_device)
        kind = " [Booted]";
      MSG_INFO("%s - %s (%s, %.1f GB) %s", disk[i].device,
	       strunknown(disk[i].model), disk[i].bus, disk[i].size_gb, kind);
    }

  return 0;
}
