#ifndef L2_THREAD_H
#define L2_THREAD_H
/*
 * tiny portable threading layer (win32 + pthreads), header only.
 * used by the game server for: tick thread, sender threads and the
 * job pool (parallel_for).
 */
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK            l2_mutex;
typedef CONDITION_VARIABLE l2_cond;
typedef HANDLE             l2_thread;
typedef DWORD (WINAPI l2_thread_fn)(LPVOID);
#define L2_THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
#define L2_THREAD_RETURN return 0

static inline void l2_mutex_init(l2_mutex *m)    { InitializeSRWLock(m); }
static inline void l2_mutex_destroy(l2_mutex *m) { (void) m; }
static inline void l2_mutex_lock(l2_mutex *m)    { AcquireSRWLockExclusive(m); }
static inline void l2_mutex_unlock(l2_mutex *m)  { ReleaseSRWLockExclusive(m); }
static inline void l2_cond_init(l2_cond *c)      { InitializeConditionVariable(c); }
static inline void l2_cond_destroy(l2_cond *c)   { (void) c; }
static inline void l2_cond_signal(l2_cond *c)    { WakeConditionVariable(c); }
static inline void l2_cond_broadcast(l2_cond *c) { WakeAllConditionVariable(c); }
static inline void l2_cond_wait(l2_cond *c, l2_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static inline void l2_cond_timedwait_ms(l2_cond *c, l2_mutex *m, uint32_t ms) { SleepConditionVariableSRW(c, m, ms, 0); }
static inline int  l2_thread_start(l2_thread *t, l2_thread_fn *fn, void *arg)
{
    *t = CreateThread(0, 0, fn, arg, 0, 0);
    return *t != 0;
}
static inline void l2_thread_join(l2_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static inline void l2_sleep_ms(uint32_t ms) { Sleep(ms); }
static inline uint64_t l2_now_ms(void) { return (uint64_t) GetTickCount64(); }
static inline int l2_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int) si.dwNumberOfProcessors;
}
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
typedef pthread_mutex_t l2_mutex;
typedef pthread_cond_t  l2_cond;
typedef pthread_t       l2_thread;
typedef void *(l2_thread_fn)(void *);
#define L2_THREAD_FN(name) static void *name(void *arg)
#define L2_THREAD_RETURN return 0

static inline void l2_mutex_init(l2_mutex *m)    { pthread_mutex_init(m, 0); }
static inline void l2_mutex_destroy(l2_mutex *m) { pthread_mutex_destroy(m); }
static inline void l2_mutex_lock(l2_mutex *m)    { pthread_mutex_lock(m); }
static inline void l2_mutex_unlock(l2_mutex *m)  { pthread_mutex_unlock(m); }
static inline void l2_cond_init(l2_cond *c)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
}
static inline void l2_cond_destroy(l2_cond *c)   { pthread_cond_destroy(c); }
static inline void l2_cond_signal(l2_cond *c)    { pthread_cond_signal(c); }
static inline void l2_cond_broadcast(l2_cond *c) { pthread_cond_broadcast(c); }
static inline void l2_cond_wait(l2_cond *c, l2_mutex *m) { pthread_cond_wait(c, m); }
static inline void l2_cond_timedwait_ms(l2_cond *c, l2_mutex *m, uint32_t ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(c, m, &ts);
}
static inline int  l2_thread_start(l2_thread *t, l2_thread_fn *fn, void *arg) { return pthread_create(t, 0, fn, arg) == 0; }
static inline void l2_thread_join(l2_thread t) { pthread_join(t, 0); }
static inline void l2_sleep_ms(uint32_t ms)
{
    struct timespec ts = { (time_t) (ms / 1000), (long) (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
}
static inline uint64_t l2_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}
static inline int l2_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int) n : 1;
}
#endif

#endif
