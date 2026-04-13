/*
 * ESP32 QEMU-only PCM playback peripheral.
 *
 * This exposes a small MMIO control block at the legacy I2S0 register page and
 * a separate 128 KiB guest-visible RAM window holding two 64 KiB PCM buffers.
 * The host side plays the queued PCM stream via miniaudio on Windows and falls
 * back to timed silent consumption on other hosts.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32_qemu_pcm.h"
#include "qemu/timer.h"

#ifdef _WIN32
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WINMM
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio/miniaudio.h>
#endif

#define ESP32_QEMU_PCM_MAGIC 0x514d4350u
#define ESP32_QEMU_PCM_VERSION 0x00000001u
#define ESP32_QEMU_PCM_SAMPLE_RATE 48000u
#define ESP32_QEMU_PCM_CHANNELS 2u
#define ESP32_QEMU_PCM_BITS_PER_SAMPLE 16u
#define ESP32_QEMU_PCM_FRAME_SIZE ((ESP32_QEMU_PCM_CHANNELS * ESP32_QEMU_PCM_BITS_PER_SAMPLE) / 8)
#define ESP32_QEMU_PCM_TIMER_PERIOD_MS 10u
#define ESP32_QEMU_PCM_TIMER_CHUNK_BYTES \
    ((ESP32_QEMU_PCM_SAMPLE_RATE * ESP32_QEMU_PCM_FRAME_SIZE * ESP32_QEMU_PCM_TIMER_PERIOD_MS) / 1000)

enum
{
    ESP32_QEMU_PCM_REG_MAGIC = 0x00,
    ESP32_QEMU_PCM_REG_VERSION = 0x04,
    ESP32_QEMU_PCM_REG_BUFFER0_ADDR = 0x08,
    ESP32_QEMU_PCM_REG_BUFFER1_ADDR = 0x0c,
    ESP32_QEMU_PCM_REG_BUFFER_SIZE = 0x10,
    ESP32_QEMU_PCM_REG_SAMPLE_RATE = 0x14,
    ESP32_QEMU_PCM_REG_CHANNELS = 0x18,
    ESP32_QEMU_PCM_REG_BITS_PER_SAMPLE = 0x1c,
    ESP32_QEMU_PCM_REG_CURRENT_BUFFER = 0x20,
    ESP32_QEMU_PCM_REG_QUEUED_MASK = 0x24,
    ESP32_QEMU_PCM_REG_UNDERRUN_COUNT = 0x28,
    ESP32_QEMU_PCM_REG_BUFFER0_LENGTH = 0x2c,
    ESP32_QEMU_PCM_REG_BUFFER1_LENGTH = 0x30,
    ESP32_QEMU_PCM_REG_BUFFER0_SUBMIT = 0x34,
    ESP32_QEMU_PCM_REG_BUFFER1_SUBMIT = 0x38,
    ESP32_QEMU_PCM_REG_CONTROL = 0x3c,
};

enum
{
    ESP32_QEMU_PCM_CONTROL_RESET = BIT(0),
};

typedef struct Esp32QemuPcmHostAudio
{
#ifdef _WIN32
    ma_device device;
    bool initialized;
    bool started;
#endif
} Esp32QemuPcmHostAudio;

typedef struct Esp32QemuPcmRuntime
{
    QemuMutex lock;
    QEMUTimer *play_timer;
    Esp32QemuPcmHostAudio *host_audio;
    uint8_t *buffer_ptr;
    uint32_t lengths[ESP32_QEMU_PCM_BUFFER_COUNT];
    uint32_t positions[ESP32_QEMU_PCM_BUFFER_COUNT];
    uint64_t submit_seq[ESP32_QEMU_PCM_BUFFER_COUNT];
    uint64_t next_submit_seq;
    uint32_t queued_mask;
    uint32_t underrun_count;
    int32_t current_buffer;
    bool host_audio_ready;
    bool buffer_window_initialized;
} Esp32QemuPcmRuntime;

static Esp32QemuPcmRuntime *esp32_qemu_pcm_rt(Esp32QemuPcmState *s)
{
    return s->opaque;
}

static void esp32_qemu_pcm_reschedule_timer_locked(Esp32QemuPcmState *s)
{
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    if (rt->host_audio_ready || rt->play_timer == NULL)
    {
        return;
    }

    if (rt->current_buffer >= 0 || rt->queued_mask != 0)
    {
        timer_mod(rt->play_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + ESP32_QEMU_PCM_TIMER_PERIOD_MS);
    }
    else
    {
        timer_del(rt->play_timer);
    }
}

static void esp32_qemu_pcm_clear_buffer_locked(Esp32QemuPcmRuntime *rt, unsigned index)
{
    rt->queued_mask &= ~BIT(index);
    rt->lengths[index] = 0;
    rt->positions[index] = 0;
    rt->submit_seq[index] = 0;
    if (rt->current_buffer == (int32_t)index)
    {
        rt->current_buffer = -1;
    }
}

static void esp32_qemu_pcm_select_next_locked(Esp32QemuPcmRuntime *rt)
{
    uint64_t best_seq = UINT64_MAX;
    int best_index = -1;

    if (rt->current_buffer >= 0)
    {
        return;
    }

    for (unsigned i = 0; i < ESP32_QEMU_PCM_BUFFER_COUNT; ++i)
    {
        if ((rt->queued_mask & BIT(i)) == 0)
        {
            continue;
        }
        if (rt->lengths[i] == 0)
        {
            rt->queued_mask &= ~BIT(i);
            continue;
        }
        if (rt->submit_seq[i] < best_seq)
        {
            best_seq = rt->submit_seq[i];
            best_index = (int)i;
        }
    }

    rt->current_buffer = best_index;
}

static void esp32_qemu_pcm_reset_locked(Esp32QemuPcmState *s)
{
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    rt->queued_mask = 0;
    rt->underrun_count = 0;
    rt->current_buffer = -1;
    rt->next_submit_seq = 1;
    memset(rt->lengths, 0, sizeof(rt->lengths));
    memset(rt->positions, 0, sizeof(rt->positions));
    memset(rt->submit_seq, 0, sizeof(rt->submit_seq));
    if (rt->buffer_ptr != NULL)
    {
        memset(rt->buffer_ptr, 0, ESP32_QEMU_PCM_WINDOW_SIZE);
    }
    if (rt->play_timer != NULL)
    {
        timer_del(rt->play_timer);
    }
}

static void esp32_qemu_pcm_submit_locked(Esp32QemuPcmState *s, unsigned index)
{
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);
    uint32_t length;

    if (index >= ESP32_QEMU_PCM_BUFFER_COUNT)
    {
        return;
    }

    if (rt->current_buffer == (int32_t)index && rt->positions[index] < rt->lengths[index])
    {
        return;
    }

    length = MIN(rt->lengths[index], (uint32_t)ESP32_QEMU_PCM_BUFFER_SIZE);
    length &= ~(ESP32_QEMU_PCM_FRAME_SIZE - 1);
    rt->lengths[index] = length;
    if (length == 0)
    {
        rt->queued_mask &= ~BIT(index);
        return;
    }

    rt->positions[index] = 0;
    rt->submit_seq[index] = rt->next_submit_seq++;
    rt->queued_mask |= BIT(index);

    if (rt->current_buffer < 0)
    {
        esp32_qemu_pcm_select_next_locked(rt);
    }

    esp32_qemu_pcm_reschedule_timer_locked(s);
}

static void esp32_qemu_pcm_pump_locked(Esp32QemuPcmState *s, uint8_t *output, size_t bytes)
{
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);
    size_t remaining = bytes;
    bool underrun = false;

    while (remaining > 0)
    {
        uint32_t available;
        uint32_t chunk;
        uint8_t *src;
        int current;

        if (rt->current_buffer < 0)
        {
            esp32_qemu_pcm_select_next_locked(rt);
        }

        current = rt->current_buffer;
        if (current < 0)
        {
            underrun = true;
            break;
        }

        available = rt->lengths[current] - rt->positions[current];
        if (available == 0)
        {
            esp32_qemu_pcm_clear_buffer_locked(rt, current);
            continue;
        }

        chunk = MIN((size_t)available, remaining);
        src = rt->buffer_ptr + (current * ESP32_QEMU_PCM_BUFFER_SIZE) + rt->positions[current];
        if (output != NULL)
        {
            memcpy(output, src, chunk);
            output += chunk;
        }
        rt->positions[current] += chunk;
        remaining -= chunk;

        if (rt->positions[current] >= rt->lengths[current])
        {
            esp32_qemu_pcm_clear_buffer_locked(rt, current);
        }
    }

    if (underrun && bytes != 0)
    {
        rt->underrun_count++;
    }

    esp32_qemu_pcm_reschedule_timer_locked(s);
}

static void esp32_qemu_pcm_timer_cb(void *opaque)
{
    Esp32QemuPcmState *s = ESP32_QEMU_PCM(opaque);
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    qemu_mutex_lock(&rt->lock);
    esp32_qemu_pcm_pump_locked(s, NULL, ESP32_QEMU_PCM_TIMER_CHUNK_BYTES);
    qemu_mutex_unlock(&rt->lock);
}

#ifdef _WIN32
static void esp32_qemu_pcm_miniaudio_cb(ma_device *device,
                                        void *output,
                                        const void *input,
                                        ma_uint32 frame_count)
{
    Esp32QemuPcmState *s = device->pUserData;
    Esp32QemuPcmRuntime *rt;
    size_t bytes = (size_t)frame_count * ESP32_QEMU_PCM_FRAME_SIZE;

    (void)input;

    memset(output, 0, bytes);

    if (s == NULL)
    {
        return;
    }

    rt = esp32_qemu_pcm_rt(s);
    if (rt == NULL)
    {
        return;
    }

    qemu_mutex_lock(&rt->lock);
    esp32_qemu_pcm_pump_locked(s, output, bytes);
    qemu_mutex_unlock(&rt->lock);
}

static bool esp32_qemu_pcm_init_host_audio(Esp32QemuPcmState *s)
{
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);
    Esp32QemuPcmHostAudio *host = g_new0(Esp32QemuPcmHostAudio, 1);
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    ma_result result;

    config.playback.format = ma_format_s16;
    config.playback.channels = ESP32_QEMU_PCM_CHANNELS;
    config.sampleRate = ESP32_QEMU_PCM_SAMPLE_RATE;
    config.dataCallback = esp32_qemu_pcm_miniaudio_cb;
    config.pUserData = s;

    result = ma_device_init(NULL, &config, &host->device);
    if (result != MA_SUCCESS)
    {
        warn_report("esp32 qemu pcm: miniaudio init failed, using silent timer fallback (%d)",
                    (int)result);
        g_free(host);
        return false;
    }

    host->initialized = true;

    result = ma_device_start(&host->device);
    if (result != MA_SUCCESS)
    {
        warn_report("esp32 qemu pcm: miniaudio start failed, using silent timer fallback (%d)",
                    (int)result);
        ma_device_uninit(&host->device);
        g_free(host);
        return false;
    }

    host->started = true;
    rt->host_audio = host;
    rt->host_audio_ready = true;
    return true;
}

static void esp32_qemu_pcm_shutdown_host_audio(Esp32QemuPcmRuntime *rt)
{
    Esp32QemuPcmHostAudio *host = rt->host_audio;

    if (host == NULL)
    {
        return;
    }

    if (host->started)
    {
        ma_device_stop(&host->device);
    }
    if (host->initialized)
    {
        ma_device_uninit(&host->device);
    }

    g_free(host);
    rt->host_audio = NULL;
    rt->host_audio_ready = false;
}
#else
static bool esp32_qemu_pcm_init_host_audio(Esp32QemuPcmState *s)
{
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    rt->host_audio_ready = false;
    return false;
}

static void esp32_qemu_pcm_shutdown_host_audio(Esp32QemuPcmRuntime *rt)
{
    rt->host_audio_ready = false;
}
#endif

static uint64_t esp32_qemu_pcm_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32QemuPcmState *s = ESP32_QEMU_PCM(opaque);
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);
    uint64_t value = 0;

    if (size != sizeof(uint32_t))
    {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32 qemu pcm: invalid read size %u at 0x%" HWADDR_PRIx "\n",
                      size, addr);
        return 0;
    }

    qemu_mutex_lock(&rt->lock);

    switch (addr)
    {
    case ESP32_QEMU_PCM_REG_MAGIC:
        value = ESP32_QEMU_PCM_MAGIC;
        break;
    case ESP32_QEMU_PCM_REG_VERSION:
        value = ESP32_QEMU_PCM_VERSION;
        break;
    case ESP32_QEMU_PCM_REG_BUFFER0_ADDR:
        value = ESP32_QEMU_PCM_BUFFER_BASE;
        break;
    case ESP32_QEMU_PCM_REG_BUFFER1_ADDR:
        value = ESP32_QEMU_PCM_BUFFER_BASE + ESP32_QEMU_PCM_BUFFER_SIZE;
        break;
    case ESP32_QEMU_PCM_REG_BUFFER_SIZE:
        value = ESP32_QEMU_PCM_BUFFER_SIZE;
        break;
    case ESP32_QEMU_PCM_REG_SAMPLE_RATE:
        value = ESP32_QEMU_PCM_SAMPLE_RATE;
        break;
    case ESP32_QEMU_PCM_REG_CHANNELS:
        value = ESP32_QEMU_PCM_CHANNELS;
        break;
    case ESP32_QEMU_PCM_REG_BITS_PER_SAMPLE:
        value = ESP32_QEMU_PCM_BITS_PER_SAMPLE;
        break;
    case ESP32_QEMU_PCM_REG_CURRENT_BUFFER:
        value = (rt->current_buffer < 0) ? UINT32_MAX : (uint32_t)rt->current_buffer;
        break;
    case ESP32_QEMU_PCM_REG_QUEUED_MASK:
        value = rt->queued_mask;
        break;
    case ESP32_QEMU_PCM_REG_UNDERRUN_COUNT:
        value = rt->underrun_count;
        break;
    case ESP32_QEMU_PCM_REG_BUFFER0_LENGTH:
        value = rt->lengths[0];
        break;
    case ESP32_QEMU_PCM_REG_BUFFER1_LENGTH:
        value = rt->lengths[1];
        break;
    case ESP32_QEMU_PCM_REG_BUFFER0_SUBMIT:
    case ESP32_QEMU_PCM_REG_BUFFER1_SUBMIT:
    case ESP32_QEMU_PCM_REG_CONTROL:
        value = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32 qemu pcm: invalid read at 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }

    qemu_mutex_unlock(&rt->lock);
    return value;
}

static void esp32_qemu_pcm_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size)
{
    Esp32QemuPcmState *s = ESP32_QEMU_PCM(opaque);
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    if (size != sizeof(uint32_t))
    {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32 qemu pcm: invalid write size %u at 0x%" HWADDR_PRIx "\n",
                      size, addr);
        return;
    }

    qemu_mutex_lock(&rt->lock);

    switch (addr)
    {
    case ESP32_QEMU_PCM_REG_BUFFER0_LENGTH:
        rt->lengths[0] = MIN((uint32_t)value, (uint32_t)ESP32_QEMU_PCM_BUFFER_SIZE);
        rt->lengths[0] &= ~(ESP32_QEMU_PCM_FRAME_SIZE - 1);
        break;
    case ESP32_QEMU_PCM_REG_BUFFER1_LENGTH:
        rt->lengths[1] = MIN((uint32_t)value, (uint32_t)ESP32_QEMU_PCM_BUFFER_SIZE);
        rt->lengths[1] &= ~(ESP32_QEMU_PCM_FRAME_SIZE - 1);
        break;
    case ESP32_QEMU_PCM_REG_BUFFER0_SUBMIT:
        if (value != 0)
        {
            esp32_qemu_pcm_submit_locked(s, 0);
        }
        break;
    case ESP32_QEMU_PCM_REG_BUFFER1_SUBMIT:
        if (value != 0)
        {
            esp32_qemu_pcm_submit_locked(s, 1);
        }
        break;
    case ESP32_QEMU_PCM_REG_CONTROL:
        if (value & ESP32_QEMU_PCM_CONTROL_RESET)
        {
            esp32_qemu_pcm_reset_locked(s);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32 qemu pcm: invalid write 0x%08" PRIx64 " at 0x%" HWADDR_PRIx "\n",
                      value, addr);
        break;
    }

    qemu_mutex_unlock(&rt->lock);
}

static const MemoryRegionOps esp32_qemu_pcm_ops = {
    .read = esp32_qemu_pcm_read,
    .write = esp32_qemu_pcm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void esp32_qemu_pcm_reset(DeviceState *dev)
{
    Esp32QemuPcmState *s = ESP32_QEMU_PCM(dev);
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    qemu_mutex_lock(&rt->lock);
    esp32_qemu_pcm_reset_locked(s);
    qemu_mutex_unlock(&rt->lock);
}

static void esp32_qemu_pcm_realize(DeviceState *dev, Error **errp)
{
    Esp32QemuPcmState *s = ESP32_QEMU_PCM(dev);
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    if (!memory_region_init_ram(&s->buffer_window, OBJECT(dev),
                                TYPE_ESP32_QEMU_PCM ".buffers",
                                ESP32_QEMU_PCM_WINDOW_SIZE, errp))
    {
        return;
    }

    rt->buffer_window_initialized = true;
    rt->buffer_ptr = memory_region_get_ram_ptr(&s->buffer_window);
    rt->play_timer = timer_new_ms(QEMU_CLOCK_REALTIME, esp32_qemu_pcm_timer_cb, s);
    esp32_qemu_pcm_init_host_audio(s);

    qemu_mutex_lock(&rt->lock);
    esp32_qemu_pcm_reset_locked(s);
    qemu_mutex_unlock(&rt->lock);
}

static void esp32_qemu_pcm_init(Object *obj)
{
    Esp32QemuPcmState *s = ESP32_QEMU_PCM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Esp32QemuPcmRuntime *rt = g_new0(Esp32QemuPcmRuntime, 1);

    s->opaque = rt;
    qemu_mutex_init(&rt->lock);
    rt->current_buffer = -1;
    rt->next_submit_seq = 1;

    memory_region_init_io(&s->iomem, obj, &esp32_qemu_pcm_ops, s,
                          TYPE_ESP32_QEMU_PCM, ESP32_QEMU_PCM_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void esp32_qemu_pcm_finalize(Object *obj)
{
    Esp32QemuPcmState *s = ESP32_QEMU_PCM(obj);
    Esp32QemuPcmRuntime *rt = esp32_qemu_pcm_rt(s);

    if (rt == NULL)
    {
        return;
    }

    esp32_qemu_pcm_shutdown_host_audio(rt);

    if (rt->play_timer != NULL)
    {
        timer_free(rt->play_timer);
    }

    qemu_mutex_destroy(&rt->lock);
    g_free(rt);
    s->opaque = NULL;
}

static void esp32_qemu_pcm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = esp32_qemu_pcm_realize;
    dc->reset = esp32_qemu_pcm_reset;
}

static const TypeInfo esp32_qemu_pcm_info = {
    .name = TYPE_ESP32_QEMU_PCM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32QemuPcmState),
    .instance_init = esp32_qemu_pcm_init,
    .instance_finalize = esp32_qemu_pcm_finalize,
    .class_init = esp32_qemu_pcm_class_init,
};

static void esp32_qemu_pcm_register_types(void)
{
    type_register_static(&esp32_qemu_pcm_info);
}

type_init(esp32_qemu_pcm_register_types)