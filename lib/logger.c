// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include "logger.h"

static FILE *log_file = NULL;

static const char* level_strings[] = {
    "TRACE", "INFO", "WARN", "ERROR"
};

int
log_init(const char *filename)
{
  if (log_file)
    return 0;

  log_file = fopen(filename, "a");
  if (!log_file)
    return -errno;
  return 0;
}

void
log_close(void)
{
  if (log_file)
    {
      fclose(log_file);
      log_file = NULL;
    }
}

void
log_write(LogLevel level, const char *file, int line, const char *func,
	  const char *fmt, ...)
{
  if (!log_file)
    return;

  time_t now;
  time(&now);
  struct tm *tm_info = localtime(&now);
  char time_buffer[26];
  strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);

  fprintf(log_file, "[%s] [%-5s] %s:%d %s() - ",
	  time_buffer, level_strings[level], file, line, func);

  va_list args;
  va_start(args, fmt);
  vfprintf(log_file, fmt, args);
  va_end(args);

  fprintf(log_file, "\n");
  fflush(log_file);
}
