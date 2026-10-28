// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <ctype.h>

#include "basics.h"
#include "parse-size.h"

int
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

  /* Accept an optional trailing "B" (e.g. "GB" as well as "G"). */
  if (multiplier != 1 && toupper((unsigned char)*ep) == 'B')
    ep++;

  if (!isempty(ep)) /* trailing garbage */
    return -EINVAL;

  if (size > UINT64_MAX / multiplier)
    return -ERANGE;

  *res = size * multiplier;
  return 0;
}
