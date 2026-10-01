// RTSky - logging
#pragma once

#include <cstdarg>

namespace rtsky::log {

enum class Level : int { Error = 0, Warning = 1, Info = 2, Debug = 3 };

// Opens (truncates) the log file. Safe to call once, early.
void Init(const wchar_t* path);
void Shutdown();
void SetLevel(Level level);
bool Enabled(Level level);
void Write(Level level, const char* fmt, ...);
void WriteV(Level level, const char* fmt, va_list args);

// Logs a message at most once per call site (for hot paths).
#define RTSKY_LOG_ONCE(level, ...)                         \
    do {                                                   \
        static bool rtskyLoggedOnce_ = false;              \
        if (!rtskyLoggedOnce_) {                           \
            rtskyLoggedOnce_ = true;                       \
            ::rtsky::log::Write(level, __VA_ARGS__);       \
        }                                                  \
    } while (0)

} // namespace rtsky::log

#define LOG_ERROR(...) ::rtsky::log::Write(::rtsky::log::Level::Error, __VA_ARGS__)
#define LOG_WARN(...) ::rtsky::log::Write(::rtsky::log::Level::Warning, __VA_ARGS__)
#define LOG_INFO(...) ::rtsky::log::Write(::rtsky::log::Level::Info, __VA_ARGS__)
#define LOG_DEBUG(...) ::rtsky::log::Write(::rtsky::log::Level::Debug, __VA_ARGS__)
