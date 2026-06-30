// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include "logger.h"

static FILE *log_file = NULL;
static bool log_console = CONSOLE_LOG;

static LogLevel current_log_level = LOG_LEVEL_INFO;

const char* log_level_to_str(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_ERROR:   return "ERROR";
        case LOG_LEVEL_WARNING: return "WARNING";
        case LOG_LEVEL_INFO:    return "INFO";
        case LOG_LEVEL_DEBUG:   return "DEBUG";
        case LOG_LEVEL_TRACE:   return "TRACE";
        case LOG_EFIVARS:       return "EFIVARS";
        default:                return "UNKNOWN";
    }
}

int
log_init(const bool console_log,
         const char *filename)
{
  log_console = console_log;
  if (log_file)
    return 0;

  if (filename)
    {
      log_file = fopen(filename, "a");
      if (!log_file)
	return -errno;
    }

  return 0;
}

void
set_max_log_level(LogLevel level)
{
  current_log_level = level;
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
  va_list args;

  va_start(args, fmt);

  if (log_console || !log_file)
    {
      /* Writing to TTY if availabel. Otherwise redirect to journald automatically. */

      if (level != LOG_EFIVARS && /* LOG_EFIVARS will always be logged here. It is decided by _efivars_debug before */
	  level > current_log_level /* Or log level does not fit */ ) {
	va_end(args);
        return;
      }

      if (level == LOG_LEVEL_ERROR || level == LOG_EFIVARS )
        {
          vfprintf(stderr, fmt, args);
	  fputc('\n', stderr);
	}
      else
	{
          vprintf(fmt, args);
          putchar('\n');
	}
    }

  if (log_file && level != LOG_EFIVARS)
    {
      /* Do not write EFI setting in the log file.*/
      time_t now;
      time(&now);
      struct tm *tm_info = localtime(&now);
      char time_buffer[26];

      strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);

      fprintf(log_file, "[%s] [%-5s] %s:%d %s() - ",
	      time_buffer, log_level_to_str(level), file, line, func);


      vfprintf(log_file, fmt, args);
      fprintf(log_file, "\n");
      fflush(log_file);
    }
  va_end(args);
}
