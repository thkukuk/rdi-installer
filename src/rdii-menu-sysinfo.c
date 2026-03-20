// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "basics.h"
#include "rdii-menu.h"

// XXX errors get ignored as output is only informative

static void
get_file_content(const char *fn, char **ret)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;

  fp = fopen(fn, "r");
  if (!fp)
    return; // ignore error

  nread = getline(&line, &line_size, fp);
  if (nread == -1)
    return; // ignore error

  if (nread > 0 && line[nread-1] == '\n')
    line[nread-1] = '\0';

  if (ret)
    *ret = TAKE_PTR(line);
}

static void
get_cpu_model(char **ret)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;

  fp = fopen("/proc/cpuinfo", "r");
  if (!fp)
    return; // ignore error

  while ((nread = getline(&line, &line_size, fp)) != -1)
    {
      if (nread > 0 && line[nread-1] == '\n')
	line[nread-1] = '\0';

       if (strneq(line, "model name", 10))
	 {
	   char *cp = strchr(line, ':');
	   if (cp)
	     {
	       // Skip the colon and leading space
	       cp += 2;

	       if (ret)
		 *ret = strdup(cp); // ignore ENOMEM
	       return;
	     }
	 }
    }
}

static void
get_meminfo(uint64_t *mem_total, uint64_t *mem_free, uint64_t *mem_available)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;

  fp = fopen("/proc/meminfo", "r");
  if (!fp)
    return; // ignore error

  while ((nread = getline(&line, &line_size, fp)) != -1)
    {
      if (sscanf(line, "MemTotal: %ld kB", mem_total) == 1)
	continue;
      if (sscanf(line, "MemFree: %ld kB", mem_free) == 1)
	continue;
      if (sscanf(line, "MemAvailable: %ld kB", mem_available) == 1)
	continue;
    }
}

int
show_sysinfo(void)
{
  _cleanup_free_ char *line = NULL;
  uint64_t mem_total = 0, mem_free = 0, mem_available = 0;
  int y = 2;

  print_global_header_footer(NULL);
  refresh();

  get_cpu_model(&line);
  if (!isempty(line))
    mvprintw(y++, 2, "CPU Model:        %s", strna(line));
  line = mfree(line);

  get_file_content("/sys/class/dmi/id/bios_vendor", &line);
  if (!isempty(line))
    mvprintw(y++, 2, "Firmware Vendor:  %s", strna(line));
  line = mfree(line);
  get_file_content("/sys/class/dmi/id/bios_version", &line);
  if (!isempty(line))
    mvprintw(y++, 2, "Firmware Version: %s", strna(line));
  line = mfree(line);
  get_file_content("/sys/class/dmi/id/bios_date", &line);
  if (!isempty(line))
    mvprintw(y++, 2, "Firmware Date:    %s", strna(line));
  line = mfree(line);

  get_meminfo(&mem_total, &mem_free, &mem_available);

  mvprintw(y++, 2, "Memory Information:");
  mvprintw(y++, 2, "  Total Memory:     %.2f GB",
	   (double)mem_total / (1024 * 1024));
  mvprintw(y++, 2, "  Free Memory:      %.2f GB",
	   (double)mem_free / (1024 * 1024));
  mvprintw(y++, 2, "  Available Memory: %.2f GB",
	   (double)mem_available / (1024 * 1024));

  refresh();
  getchar();

  return 0;
}
