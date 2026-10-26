#include "thread.h"

#include <stdlib.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct EgtThread {
    HANDLE handle;
    void (*fn)(void *);
    void *arg;
};

struct EgtSync {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cond;
};

static DWORD WINAPI trampoline(LPVOID p)
{
    EgtThread *t = p;
    t->fn(t->arg);
    return 0;
}

EgtThread *egt_thread_start(void (*fn)(void *), void *arg)
{
    EgtThread *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!t->handle) {
        free(t);
        return NULL;
    }
    return t;
}

void egt_thread_join(EgtThread *t)
{
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

EgtSync *egt_sync_create(void)
{
    EgtSync *s = malloc(sizeof *s);
    if (!s)
        return NULL;
    InitializeCriticalSection(&s->lock);
    InitializeConditionVariable(&s->cond);
    return s;
}

void egt_sync_destroy(EgtSync *s)
{
    DeleteCriticalSection(&s->lock);
    free(s);
}

void egt_sync_lock(EgtSync *s) { EnterCriticalSection(&s->lock); }
void egt_sync_unlock(EgtSync *s) { LeaveCriticalSection(&s->lock); }
void egt_sync_wait(EgtSync *s) { SleepConditionVariableCS(&s->cond, &s->lock, INFINITE); }
void egt_sync_signal(EgtSync *s) { WakeAllConditionVariable(&s->cond); }

#else

#include <pthread.h>

struct EgtThread {
    pthread_t id;
    void (*fn)(void *);
    void *arg;
};

struct EgtSync {
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

static void *trampoline(void *p)
{
    EgtThread *t = p;
    t->fn(t->arg);
    return NULL;
}

EgtThread *egt_thread_start(void (*fn)(void *), void *arg)
{
    EgtThread *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->id, NULL, trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void egt_thread_join(EgtThread *t)
{
    pthread_join(t->id, NULL);
    free(t);
}

EgtSync *egt_sync_create(void)
{
    EgtSync *s = malloc(sizeof *s);
    if (!s)
        return NULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    return s;
}

void egt_sync_destroy(EgtSync *s)
{
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void egt_sync_lock(EgtSync *s) { pthread_mutex_lock(&s->lock); }
void egt_sync_unlock(EgtSync *s) { pthread_mutex_unlock(&s->lock); }
void egt_sync_wait(EgtSync *s) { pthread_cond_wait(&s->cond, &s->lock); }
void egt_sync_signal(EgtSync *s) { pthread_cond_broadcast(&s->cond); }

#endif
