// RTSky - logging
#include "Log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace rtsky::log {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
FILE* g_file = nullptr;
std::atomic<int> g_level{ static_cast<int>(Level::Info) };

const char* LevelName(Level level)
{
    switch (level)
    {
    case Level::Error: return "ERROR";
    case Level::Warning: return "WARN ";
    case Level::Info: return "INFO ";
    case Level::Debug: return "DEBUG";
    }
    return "?????";
}

} // namespace

void Init(const wchar_t* path)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_file == nullptr)
        g_file = _wfopen(path, L"w");
    ReleaseSRWLockExclusive(&g_lock);
}

void Shutdown()
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != nullptr)
    {
        fclose(g_file);
        g_file = nullptr;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void SetLevel(Level level)
{
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

bool Enabled(Level level)
{
    return static_cast<int>(level) <= g_level.load(std::memory_order_relaxed);
}

void WriteV(Level level, const char* fmt, va_list args)
{
    if (!Enabled(level))
        return;

    char message[2048];
    vsnprintf(message, sizeof(message), fmt, args);

    SYSTEMTIME t;
    GetLocalTime(&t);

    AcquireSRWLockExclusive(&g_lock);
    if (g_file != nullptr)
    {
        fprintf(g_file, "%02u:%02u:%02u.%03u [%s] [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                LevelName(level), GetCurrentThreadId(), message);
        fflush(g_file);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void Write(Level level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
    va_end(args);
}

} // namespace rtsky::log
