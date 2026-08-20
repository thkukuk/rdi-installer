// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "basics.h"
#include "nc-dialogs.h"
#include "rdii-menu.h"
#include "logger.h"

static bool
get_file_content(const char *fn, char **ret)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;

  fp = fopen(fn, "r");
  if (!fp)
    {
      MSG_ERROR("Failed to open file '%s': %s",
		fn, strerror(errno));
      return false;
    }

  nread = getline(&line, &line_size, fp);
  if (nread == -1)
    {
      MSG_ERROR("Failed to read file '%s'.",
		fn);
      return false;
    }

  if (nread > 0 && line[nread-1] == '\n')
    line[nread-1] = '\0';

  if (ret)
    *ret = TAKE_PTR(line);
  return true;
}

static bool
get_cpu_model(char **ret)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;

  fp = fopen("/proc/cpuinfo", "r");
  if (!fp)
    {
      MSG_ERROR("Failed to open /proc/cpuinfo : %s",
		strerror(errno));
      return false;
    }

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
		 *ret = strdup(cp);
	       if (!*ret)
                 {
                   MSG_ERROR("Out of memory");
	           return false;
		 }
	       return true;
	     }
	 }
    }
  return false;
}

static bool
get_meminfo(uint64_t *mem_total, uint64_t *mem_free, uint64_t *mem_available)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;

  fp = fopen("/proc/meminfo", "r");
  if (!fp)
    {
      MSG_ERROR("Failed to open /proc/meminfo : %s",
		strerror(errno));
      return false;
    }

  while ((nread = getline(&line, &line_size, fp)) != -1)
    {
      if (sscanf(line, "MemTotal: %ld kB", mem_total) == 1)
	continue;
      if (sscanf(line, "MemFree: %ld kB", mem_free) == 1)
	continue;
      if (sscanf(line, "MemAvailable: %ld kB", mem_available) == 1)
	continue;
    }
  return true;
}

int
show_sysinfo(void)
{
  _cleanup_free_ char *line = NULL;
  uint64_t mem_total = 0, mem_free = 0, mem_available = 0;
  int y = 2;

  print_global_header_footer(NULL);
  refresh();

  if (get_cpu_model(&line) && !isempty(line))
    mvprintw(y++, 2, "CPU Model:        %s", strna(line));
  else
    mvprintw(y++, 2, "CPU Model:        no information");
  line = mfree(line);

  if (get_file_content("/sys/class/dmi/id/bios_vendor", &line) && !isempty(line))
    mvprintw(y++, 2, "Firmware Vendor:  %s", strna(line));
  else
    mvprintw(y++, 2, "Firmware Vendor:  no information");
  line = mfree(line);
  if (get_file_content("/sys/class/dmi/id/bios_version", &line) && !isempty(line))
    mvprintw(y++, 2, "Firmware Version: %s", strna(line));
  else
    mvprintw(y++, 2, "Firmware Version: no information");
  line = mfree(line);
  if (get_file_content("/sys/class/dmi/id/bios_date", &line) && !isempty(line))
    mvprintw(y++, 2, "Firmware Date:    %s", strna(line));
  else
    mvprintw(y++, 2, "Firmware Date:    no information");
  line = mfree(line);

  if (get_meminfo(&mem_total, &mem_free, &mem_available))
    {
      mvprintw(y++, 2, "Memory Information:");
      mvprintw(y++, 2, "  Total Memory:     %.2f GB",
	       (double)mem_total / (1024 * 1024));
      mvprintw(y++, 2, "  Free Memory:      %.2f GB",
	       (double)mem_free / (1024 * 1024));
      mvprintw(y++, 2, "  Available Memory: %.2f GB",
	       (double)mem_available / (1024 * 1024));
    }
  else
    mvprintw(y++, 2, "Memory Information: no information");

  mvprintw(y++, 2, "Terminal Information:");
  mvprintw(y++, 2, "  TERM: %s", getenv("TERM"));
  mvprintw(y++, 2, "  Colors: %i", COLORS);

  refresh();
  getchar();

  return 0;
}
