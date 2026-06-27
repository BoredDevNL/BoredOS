#ifndef CMSIS_OS2_SHIM_H
#define CMSIS_OS2_SHIM_H

#include <stdint.h>
#include <stddef.h>

#define osWaitForever ((uint32_t)0xFFFFFFFFU)

typedef enum { osOK = 0, osError = 1, osErrorTimeout = 2 } osStatus_t;

typedef void* osMessageQueueId_t;
typedef void* osSemaphoreId_t;
typedef void* osMutexId_t;
typedef uint32_t osThreadId_t;

typedef int osPriority_t;

typedef struct {
    const char *name;
    size_t stack_size;
    osPriority_t priority;
} osThreadAttr_t;

#ifdef __cplusplus
extern "C" {
#endif

// Message queue
osMessageQueueId_t osMessageQueueNew(uint32_t max_messages, uint32_t message_size, void *attr);
uint32_t osMessageQueueGetCount(osMessageQueueId_t id);
void osMessageQueueDelete(osMessageQueueId_t id);
osStatus_t osMessageQueuePut(osMessageQueueId_t id, const void *msg_ptr, uint8_t msg_prio, uint32_t timeout);
osStatus_t osMessageQueueGet(osMessageQueueId_t id, void *msg_ptr, uint8_t *msg_prio, uint32_t timeout);

// Semaphore
osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count, void *attr);
osStatus_t osSemaphoreAcquire(osSemaphoreId_t id, uint32_t timeout);
void osSemaphoreRelease(osSemaphoreId_t id);
void osSemaphoreDelete(osSemaphoreId_t id);

// Mutex (wraps semaphore)
osMutexId_t osMutexNew(void *attr);
void osMutexDelete(osMutexId_t id);
void osMutexAcquire(osMutexId_t id, uint32_t timeout);
void osMutexRelease(osMutexId_t id);

// Thread
osThreadId_t osThreadNew(void (*func)(void *), void *arg, const osThreadAttr_t *attr);

// Kernel tick
uint32_t osKernelGetTickCount(void);

#ifdef __cplusplus
}
#endif

#endif
