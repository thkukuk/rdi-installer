#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

// Log levels
typedef enum {
    LOG_LEVEL_TRACE,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

// Initialize and close the logger
int log_init(const char *filename);
void log_close(void);

// The core logging function (do not call directly, use macros)
void log_write(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...);

// Convenience Macros
// -----------------------------------------------------------------------------
// Call this at the start of a function to log its execution and arguments
#define LOG_FUNC(...) \
    log_write(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, "CALLED with args: " __VA_ARGS__)

// General purpose logging macros for use anywhere
#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

#endif // LOGGER_H
