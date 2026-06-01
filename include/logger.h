#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

// The values are almost the same as in syslog.h defined.
typedef enum {
    LOG_LEVEL_TRACE = -1,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_WARNING = 4,
    LOG_LEVEL_INFO = 6,
    LOG_LEVEL_DEBUG = 7
} LogLevel;

// Initialize and close the logger

// If log_init will be not called or filename is NULL, the logging will be done to console if a TTY
// is available. Otherwise it will be written to journald. The default log level will be LOG_LEVEL_WARNING.
// LOG_LEVEL_TRACE will NOT be logged to console or TTY.
//
// If there is a valid filename, all Logging (TRACE included) will be written to it.
// The default log level will be LOG_LEVEL_INFO in that case.
int log_init(const char *filename);

void set_max_log_level(LogLevel level);
void log_close(void);

// The core logging functions (do not call directly, use macros)
void log_write(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...);
const char* log_level_to_str(LogLevel level);

// Convenience Macros
// -----------------------------------------------------------------------------
// Call this at the start of a function to log its execution and arguments
#define LOG_FUN(...) \
    log_write(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, "CALLED with args: " __VA_ARGS__)

// General purpose logging macros for use anywhere
#define LOG_DEBUG(...)   log_write(LOG_LEVEL_DEBUG,    __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...)    log_write(LOG_LEVEL_INFO,     __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARNING(...) log_write(LOG_LEVEL_WARNING,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...)   log_write(LOG_LEVEL_ERROR,      __FILE__, __LINE__, __func__, __VA_ARGS__)

#endif // LOGGER_H
