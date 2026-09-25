#pragma once
using SemaphoreHandle_t = void*;
constexpr int portMAX_DELAY = 0;
inline void* xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline void xSemaphoreTake(void*, int) {}
inline void xSemaphoreGive(void*) {}
