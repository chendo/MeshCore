#pragma once
#include <freertos/FreeRTOS.h>
typedef void* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t)1; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, int) { return pdTRUE; }
inline void xSemaphoreGive(SemaphoreHandle_t) {}
inline void vTaskDelay(int ms) { g_fake_millis += ms; }
