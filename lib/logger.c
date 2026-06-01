// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <systemd/sd-journal.h>
#include "logger.h"

static FILE *log_file = NULL;

static LogLevel current_log_level = LOG_LEVEL_WARNING;

const char* log_level_to_str(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_ERROR:   return "ERROR";
        case LOG_LEVEL_WARNING: return "WARNING";
        case LOG_LEVEL_INFO:    return "INFO";
        case LOG_LEVEL_DEBUG:   return "DEBUG";
        case LOG_LEVEL_TRACE:   return "TRACE";
        default:                return "UNKNOWN";
    }
}

int
log_init(const char *filename)
{
  current_log_level = LOG_LEVEL_WARNING;
  if (log_file)
    {
      set_max_log_level(LOG_LEVEL_WARNING);
      return 0;
    }
  else
    set_max_log_level(LOG_LEVEL_INFO);

  log_file = fopen(filename, "a");
  if (!log_file)
    return -errno;
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

  if (level > current_log_level)
    return;

  va_start(args, fmt);

  if (!log_file)
    {
      // Writing only to TTY if availabel. Otherwise write to journald
      static int is_tty = -1;

      if (level == LOG_LEVEL_TRACE) /* Do not log function parameters */
        return;

      if (is_tty == -1)
        is_tty = isatty(STDOUT_FILENO);

      if (is_tty)
        {
	  if (level <= LOG_ERR)
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
      else
        sd_journal_printv(level, fmt, args);
    } else {
      /* Writing to log file, TRACE included */
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
