/*
 * Minimal threads for the GUI's background conversion: pthreads, or Win32
 * on Windows (kept out of gui.c because windows.h clashes with raylib).
 */
#ifndef EGATOOL_THREAD_H
#define EGATOOL_THREAD_H

#include <stdbool.h>

typedef struct EgtThread EgtThread;

/* A mutex with one condition variable. */
typedef struct EgtSync EgtSync;

/* Starts fn(arg) on a new thread; NULL on failure. */
EgtThread *egt_thread_start(void (*fn)(void *), void *arg);
/* Waits for the thread to finish and frees it. */
void egt_thread_join(EgtThread *t);

EgtSync *egt_sync_create(void);
void egt_sync_destroy(EgtSync *s);
void egt_sync_lock(EgtSync *s);
void egt_sync_unlock(EgtSync *s);
/* Releases the lock, waits for a signal, reacquires it (spurious wakeups
 * possible: wait in a loop on the actual condition). */
void egt_sync_wait(EgtSync *s);
/* Wakes all waiters. */
void egt_sync_signal(EgtSync *s);

#endif
