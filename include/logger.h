#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

// Log levels
// The values are almost the same as in syslog.h defined.
typedef enum {
    LOG_EFIVARS = -2,
    LOG_LEVEL_TRACE = -1,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_WARNING = 4,
    LOG_LEVEL_INFO = 6,
    LOG_LEVEL_DEBUG = 7
} LogLevel;

#define CONSOLE_LOG true
#define NO_CONSOLE_LOG false

// Initialize the logger

// console_log: The logs can be written to console.
//              If the console is not available it will be written to journald.
// filename:    If it is not NULL, the logging will written into this file too.
//
// If log_init is not called, the default values are : console_log = CONSOLE_LOG; filename = NULL
int log_init(const bool console_log, const char *filename);

// The default log level is LOG_LEVEL_WARNING
void set_max_log_level(LogLevel level);

// Close the logger
void log_close(void);

// The core logging functions (do not call directly, use macros)
void log_write(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...);
const char* log_level_to_str(LogLevel level);

// Convenience Macros
// -----------------------------------------------------------------------------
// Call this at the start of a function to log its execution and arguments
#define MSG_FUNC(...) \
    log_write(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, "CALLED with args: " __VA_ARGS__)

/* Output with more concret information about EFI settings */
/* This will be done by the -d option in the command line interfaces */
#define MSG_EFIVARS(...) log_write(LOG_EFIVARS, __FILE__, __LINE__, __func__, __VA_ARGS__)

// General purpose logging macros for use anywhere
#define MSG_DEBUG(...) log_write(LOG_LEVEL_DEBUG,   __FILE__, __LINE__, __func__, __VA_ARGS__)
#define MSG_INFO(...)  log_write(LOG_LEVEL_INFO,    __FILE__, __LINE__, __func__, __VA_ARGS__)
#define MSG_WARN(...)  log_write(LOG_LEVEL_WARNING, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define MSG_ERROR(...) log_write(LOG_LEVEL_ERROR,   __FILE__, __LINE__, __func__, __VA_ARGS__)

#endif // LOGGER_H
