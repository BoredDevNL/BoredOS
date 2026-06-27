#include "cmsis_os2.h"
#include "wait_queue.h"
#include "process.h"
#include "spinlock.h"
#include "memory_manager.h"
#include "kutils.h"
#include <string.h>
#include <stdlib.h>

// Simple implementations using kernel primitives

typedef struct message_queue {
    void **buf;
    uint32_t max;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    spinlock_t lock;
    wait_queue_head_t waitq;
} message_queue_t;

typedef struct simple_sem {
    int count;
    spinlock_t lock;
    wait_queue_head_t waitq;
} simple_sem_t;

typedef struct simple_mutex { simple_sem_t sem; } simple_mutex_t;

// Thread start registry
typedef struct thread_start {
    uint32_t pid;
    void (*fn)(void *);
    void *arg;
    struct thread_start *next;
} thread_start_t;

static thread_start_t *thread_starts = NULL;
static spinlock_t thread_starts_lock = SPINLOCK_INIT;

// Helper: find by pid
static thread_start_t *find_thread_start(uint32_t pid) {
    uint64_t flags = spinlock_acquire_irqsave(&thread_starts_lock);
    thread_start_t *cur = thread_starts;
    while (cur) {
        if (cur->pid == pid) { spinlock_release_irqrestore(&thread_starts_lock, flags); return cur; }
        cur = cur->next;
    }
    spinlock_release_irqrestore(&thread_starts_lock, flags);
    return NULL;
}

// Wrapper entry for kernel threads: find entry by pid and call it
static void thread_entry_wrapper(void) {
    uint32_t pid = process_get_current_pid();
    thread_start_t *ts = find_thread_start(pid);
    if (!ts) {
        // nothing to do
        return;
    }
    ts->fn(ts->arg);
}

// Message queue
osMessageQueueId_t osMessageQueueNew(uint32_t max_messages, uint32_t message_size, void *attr) {
    (void)message_size; (void)attr;
    message_queue_t *q = (message_queue_t *)kmalloc(sizeof(message_queue_t));
    if (!q) return NULL;
    q->buf = (void **)kmalloc(sizeof(void*) * max_messages);
    if (!q->buf) { kfree(q); return NULL; }
    q->max = max_messages;
    q->head = q->tail = q->count = 0;
    q->lock = SPINLOCK_INIT;
    wait_queue_init(&q->waitq);
    return (osMessageQueueId_t)q;
}

uint32_t osMessageQueueGetCount(osMessageQueueId_t id) {
    message_queue_t *q = (message_queue_t *)id;
    if (!q) return 0;
    uint64_t flags = spinlock_acquire_irqsave(&q->lock);
    uint32_t c = q->count;
    spinlock_release_irqrestore(&q->lock, flags);
    return c;
}

void osMessageQueueDelete(osMessageQueueId_t id) {
    message_queue_t *q = (message_queue_t *)id;
    if (!q) return;
    kfree(q->buf);
    kfree(q);
}

osStatus_t osMessageQueuePut(osMessageQueueId_t id, const void *msg_ptr, uint8_t msg_prio, uint32_t timeout) {
    (void)msg_prio; (void)timeout;
    message_queue_t *q = (message_queue_t *)id;
    if (!q) return osError;
    uint64_t flags = spinlock_acquire_irqsave(&q->lock);
    if (q->count >= q->max) {
        spinlock_release_irqrestore(&q->lock, flags);
        return osError;
    }
    q->buf[q->tail] = (void*)*(const void**)msg_ptr;
    q->tail = (q->tail + 1) % q->max;
    q->count++;
    spinlock_release_irqrestore(&q->lock, flags);
    wait_queue_wake_all(&q->waitq);
    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t id, void *msg_ptr, uint8_t *msg_prio, uint32_t timeout) {
    (void)msg_prio;
    message_queue_t *q = (message_queue_t *)id;
    if (!q) return osError;

    if (timeout != 0) {
        process_t *proc = process_get_current();
        wait_queue_entry_t entry;
        entry.proc = proc;
        entry.next = NULL;
        wait_queue_add(&q->waitq, &entry);

        uint32_t sleep_until = 0;
        if (timeout != osWaitForever) {
            extern uint32_t get_ticks(void);
            uint32_t ticks = timeout / 16; if (ticks == 0) ticks = 1;
            sleep_until = get_ticks() + ticks;
        }

        while (1) {
            uint64_t flags = spinlock_acquire_irqsave(&q->lock);
            if (q->count > 0) {
                void *v = q->buf[q->head];
                q->head = (q->head + 1) % q->max;
                q->count--;
                spinlock_release_irqrestore(&q->lock, flags);
                wait_queue_remove(&q->waitq, &entry);
                if (msg_ptr) *(void**)msg_ptr = v;
                return osOK;
            }

            if (timeout != osWaitForever) {
                extern uint32_t get_ticks(void);
                if (get_ticks() >= sleep_until) {
                    spinlock_release_irqrestore(&q->lock, flags);
                    wait_queue_remove(&q->waitq, &entry);
                    return osErrorTimeout;
                }
                proc->sleep_until = sleep_until;
            } else {
                proc->sleep_until = 0;
            }

            proc->state = PROC_STATE_BLOCKED;
            spinlock_release_irqrestore(&q->lock, flags);

            while (proc->state == PROC_STATE_BLOCKED) {
                asm volatile("hlt");
            }
        }
    } else {
        uint64_t flags = spinlock_acquire_irqsave(&q->lock);
        if (q->count > 0) {
            void *v = q->buf[q->head];
            q->head = (q->head + 1) % q->max;
            q->count--;
            spinlock_release_irqrestore(&q->lock, flags);
            if (msg_ptr) *(void**)msg_ptr = v;
            return osOK;
        }
        spinlock_release_irqrestore(&q->lock, flags);
        return osError;
    }
}

// Semaphore
osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count, void *attr) {
    (void)max_count; (void)attr;
    simple_sem_t *s = (simple_sem_t*)kmalloc(sizeof(simple_sem_t));
    if (!s) return NULL;
    s->count = (int)initial_count;
    s->lock = SPINLOCK_INIT;
    wait_queue_init(&s->waitq);
    return (osSemaphoreId_t)s;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t id, uint32_t timeout) {
    simple_sem_t *s = (simple_sem_t*)id;
    if (!s) return osError;

    if (timeout != 0) {
        process_t *proc = process_get_current();
        wait_queue_entry_t entry;
        entry.proc = proc;
        entry.next = NULL;
        wait_queue_add(&s->waitq, &entry);

        uint32_t sleep_until = 0;
        if (timeout != osWaitForever) {
            extern uint32_t get_ticks(void);
            uint32_t ticks = timeout / 16; if (ticks == 0) ticks = 1;
            sleep_until = get_ticks() + ticks;
        }

        while (1) {
            uint64_t flags = spinlock_acquire_irqsave(&s->lock);
            if (s->count > 0) {
                s->count--;
                spinlock_release_irqrestore(&s->lock, flags);
                wait_queue_remove(&s->waitq, &entry);
                return osOK;
            }

            if (timeout != osWaitForever) {
                extern uint32_t get_ticks(void);
                if (get_ticks() >= sleep_until) {
                    spinlock_release_irqrestore(&s->lock, flags);
                    wait_queue_remove(&s->waitq, &entry);
                    return osErrorTimeout;
                }
                proc->sleep_until = sleep_until;
            } else {
                proc->sleep_until = 0;
            }

            proc->state = PROC_STATE_BLOCKED;
            spinlock_release_irqrestore(&s->lock, flags);

            while (proc->state == PROC_STATE_BLOCKED) {
                asm volatile("hlt");
            }
        }
    } else {
        uint64_t flags = spinlock_acquire_irqsave(&s->lock);
        if (s->count > 0) {
            s->count--;
            spinlock_release_irqrestore(&s->lock, flags);
            return osOK;
        }
        spinlock_release_irqrestore(&s->lock, flags);
        return osError;
    }
}

void osSemaphoreRelease(osSemaphoreId_t id) {
    simple_sem_t *s = (simple_sem_t*)id;
    if (!s) return;
    uint64_t flags = spinlock_acquire_irqsave(&s->lock);
    s->count++;
    spinlock_release_irqrestore(&s->lock, flags);
    wait_queue_wake_all(&s->waitq);
}

void osSemaphoreDelete(osSemaphoreId_t id) { if (id) kfree(id); }

// Mutex
osMutexId_t osMutexNew(void *attr) {
    (void)attr;
    simple_mutex_t *m = (simple_mutex_t*)kmalloc(sizeof(simple_mutex_t));
    if (!m) return NULL;
    m->sem.count = 1;
    m->sem.lock = SPINLOCK_INIT;
    wait_queue_init(&m->sem.waitq);
    return (osMutexId_t)m;
}

void osMutexDelete(osMutexId_t id) { if (id) kfree(id); }
void osMutexAcquire(osMutexId_t id, uint32_t timeout) { (void)timeout; osSemaphoreAcquire((osSemaphoreId_t)id, timeout); }
void osMutexRelease(osMutexId_t id) { osSemaphoreRelease((osSemaphoreId_t)id); }

// Thread creation: use process_create
osThreadId_t osThreadNew(void (*func)(void *), void *arg, const osThreadAttr_t *attr) {
    // register thread start after process created
    process_t *p = process_create(thread_entry_wrapper, false);
    if (!p) return 0;
    thread_start_t *ts = (thread_start_t*)kmalloc(sizeof(thread_start_t));
    if (!ts) return 0;
    ts->pid = p->pid;
    ts->fn = func;
    ts->arg = arg;
    uint64_t flags = spinlock_acquire_irqsave(&thread_starts_lock);
    ts->next = thread_starts;
    thread_starts = ts;
    spinlock_release_irqrestore(&thread_starts_lock, flags);
    (void)attr;
    return (osThreadId_t)p->pid;
}

// Kernel tick
uint32_t osKernelGetTickCount(void) {
    extern uint32_t get_ticks(void);
    return get_ticks();
}

