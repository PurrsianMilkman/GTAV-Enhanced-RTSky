// Linux shim used only by the host analyzer tests (tests/AnalyzerTests.cpp): DirectX-Headers' WSL
// adapter for COM types plus the few Win32 functions the tracker / analyzer use.
#pragma once
#include <wsl/winadapter.h>
#include <pthread.h>
#include <stdio.h>
#include <wchar.h>

struct SRWLOCK
{
    pthread_rwlock_t l;
};
#define SRWLOCK_INIT { PTHREAD_RWLOCK_INITIALIZER }
inline void AcquireSRWLockExclusive(SRWLOCK* s) { pthread_rwlock_wrlock(&s->l); }
inline void ReleaseSRWLockExclusive(SRWLOCK* s) { pthread_rwlock_unlock(&s->l); }
inline void AcquireSRWLockShared(SRWLOCK* s) { pthread_rwlock_rdlock(&s->l); }
inline void ReleaseSRWLockShared(SRWLOCK* s) { pthread_rwlock_unlock(&s->l); }

extern unsigned long long g_testTick; // advanced by the tests
inline unsigned long long GetTickCount64() { return g_testTick; }
inline FILE* _wfopen(const wchar_t*, const wchar_t*) { return nullptr; }
