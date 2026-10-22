#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
int xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                unsigned prio, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
