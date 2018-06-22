#pragma once
#include "platform.h"

#include <memory>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string>

namespace securefs
{
enum LoggingLevel
{
    kLogTrace = 0,
    kLogVerbose = 1,
    kLogInfo = 2,
    kLogWarning = 3,
    kLogError = 4
};

inline const char* stringify(LoggingLevel lvl)
{
    switch (lvl)
    {
    case kLogTrace:
        return "Trace";
    case kLogVerbose:
        return "Verbose";
    case kLogInfo:
        return "Info";
    case kLogWarning:
        return "Warning";
    case kLogError:
        return "Error";
    }
    return "UNKNOWN";
}

class Logger
{
    DISABLE_COPY_MOVE(Logger)

private:
    LoggingLevel m_level;
    FILE* m_fp;
    std::unique_ptr<ConsoleColourSetter> m_console_color;
    bool m_close_on_exit;

    explicit Logger(FILE* fp, bool close_on_exit);

public:
    static Logger* create_stderr_logger();
    static Logger* create_file_logger(const std::string& path);

    void vlog(LoggingLevel level, const char* format, va_list args) noexcept;
    void log(LoggingLevel level, const char* format, ...) noexcept
#ifndef _MSC_VER
        __attribute__((format(printf, 3, 4)))
#endif
        ;

    LoggingLevel get_level() const noexcept { return m_level; }
    void set_level(LoggingLevel lvl) noexcept { m_level = lvl; }

    ~Logger();
};

extern Logger* global_logger;

#define GENERIC_LOG(log_level, ...)                                                                \
    do                                                                                             \
    {                                                                                              \
        using securefs::global_logger;                                                             \
        if (global_logger && global_logger->get_level() <= log_level)                              \
        {                                                                                          \
            global_logger->log(log_level, __VA_ARGS__);                                            \
        }                                                                                          \
    } while (0)
#define TRACE_LOG(...) GENERIC_LOG(securefs::kLogTrace, __VA_ARGS__)
#define VERBOSE_LOG(...) GENERIC_LOG(securefs::kLogVerbose, __VA_ARGS__)
#define INFO_LOG(...) GENERIC_LOG(securefs::kLogInfo, __VA_ARGS__)
#define WARN_LOG(...) GENERIC_LOG(securefs::kLogWarning, __VA_ARGS__)
#define ERROR_LOG(...) GENERIC_LOG(securefs::kLogError, __VA_ARGS__)


 class OperationLogger
{
public:
    OperationLogger(const std::string& text);

    ~OperationLogger();

private:
    std::string m_text;
    LARGE_INTEGER m_begin_time;
    LARGE_INTEGER m_end_time;
    LARGE_INTEGER m_freq;
};
}    // namespace securefs
