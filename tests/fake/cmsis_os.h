/* Minimal stand-in for CMSIS-RTOS2 so logger.c compiles on a PC for tests/test_logger.c. */
#ifndef FAKE_CMSIS_OS_H
#define FAKE_CMSIS_OS_H

#include <stdint.h>

typedef void *osThreadId_t;
#define osFlagsWaitAny 0u

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout_ms);
uint32_t osThreadFlagsSet(osThreadId_t thread, uint32_t flags);
uint32_t osKernelGetTickCount(void);
int osDelay(uint32_t ms);

#endif
