#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32_reg.h"

#define TYPE_ESP32_QEMU_PCM "misc.esp32.qemu-pcm"
#define ESP32_QEMU_PCM(obj) OBJECT_CHECK(Esp32QemuPcmState, (obj), TYPE_ESP32_QEMU_PCM)

#define ESP32_QEMU_PCM_REG_BASE DR_REG_I2S_BASE
#define ESP32_QEMU_PCM_REGS_SIZE 0x1000
#define ESP32_QEMU_PCM_BUFFER_SIZE 0x10000
#define ESP32_QEMU_PCM_BUFFER_COUNT 2
#define ESP32_QEMU_PCM_WINDOW_SIZE (ESP32_QEMU_PCM_BUFFER_SIZE * ESP32_QEMU_PCM_BUFFER_COUNT)
#define ESP32_QEMU_PCM_BUFFER_BASE 0x70000000

typedef struct Esp32QemuPcmState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion buffer_window;
    void *opaque;
} Esp32QemuPcmState;