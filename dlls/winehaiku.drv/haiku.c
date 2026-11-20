/*
 * Unixlib for the winehaiku audio driver: plays through BSoundPlayer (see haikushim.cpp).
 *
 * Modelled on the winecoreaudio driver, which is also a driver that is called back for its data by the system. Playback goes
 * through BSoundPlayer, capture through BMediaRecorder; every input of the sound hardware (Mic in, Line in ...) is a capture
 * endpoint of its own.
 *
 * Copyright 2011 Andrew Eikum for CodeWeavers
 * Copyright 2021 Huw Davies
 * Copyright 2026 the Wine Haiku port
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"
#include "devpkey.h"
#include "mmsystem.h"
#include "mmddk.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(haiku);

#define DEVICE_ID "haiku-default"
#define INPUT_ID_PREFIX "haiku-in-"
#define MAX_INPUTS 16
#define MIX_RATE 48000
#define MAX_CHANNELS 8
#define PERIOD_FRAMES 480      /* 10 ms */

/* what BSoundPlayer is asked for (see haikushim.cpp) */
enum { HS_UINT8, HS_INT16, HS_INT32, HS_FLOAT };

static struct
{
    void *handle;
    void *(*open)(int rate, int channels, int format, int frames_per_buffer, void (*fill)(void *, void *, size_t), void *cookie);
    int (*start)(void *player);
    void (*stop)(void *player);
    void (*close)(void *player);
    long long (*latency)(void *player);
    /* capture; may be missing */
    int (*list_inputs)(char names[][64], int *values, int max, int *current);
    void *(*record_open)(int channels, int format, int input_value, void (*record)(void *, const void *, size_t),
                         void *cookie, int *rate);
    int (*record_start)(void *recorder);
    void (*record_stop)(void *recorder);
    void (*record_close)(void *recorder);
    /* MIDI; may be missing */
    int (*midi_list)(int sources, char names[][64], int *ids, int max);
    void *(*midi_in_open)(int producer_id, const char *name,
                          void (*func)(void *, const unsigned char *, size_t, long long), void *cookie);
    void (*midi_in_close)(void *handle);
    void *(*midi_out_open)(int consumer_id, const char *name);
    void (*midi_send)(void *handle, const unsigned char *data, size_t length);
    void (*midi_out_close)(void *handle);
} hs;

struct haiku_stream
{
    pthread_mutex_t lock;
    void *player;
    void *recorder;
    BOOL capture;
    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    HANDLE timer_thread;
    int sample_kind;

    BOOL playing, please_quit;
    REFERENCE_TIME period;
    UINT32 period_frames;
    UINT32 bufsize_frames;
    UINT32 lcl_offs_frames, held_frames, wri_offs_frames, tmp_buffer_frames;
    UINT64 written_frames;
    INT32 getbuf_last;
    WAVEFORMATEX *fmt;
    BYTE *local_buffer, *tmp_buffer;
    float master_volume;
    float volumes[MAX_CHANNELS];     /* the channel volumes times the session ones */
};

static ULONG_PTR zero_bits = 0;

static NTSTATUS unix_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

static struct haiku_stream *handle_get_stream(stream_handle h)
{
    return (struct haiku_stream *)(UINT_PTR)h;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static BOOL load_shim(void)
{
    Dl_info info;
    char path[1024];
    void *handle = NULL;

    if (hs.handle) return TRUE;

    if (dladdr((void *)load_shim, &info) && info.dli_fname)
    {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash)
        {
            snprintf(path, sizeof(path), "%.*s/libwinehaikushim.so", (int)(slash - info.dli_fname), info.dli_fname);
            handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        }
    }
    if (!handle) handle = dlopen("libwinehaikushim.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        WARN("Failed to load libwinehaikushim.so: %s\n", dlerror());
        return FALSE;
    }

#define LOAD(name) if (!(hs.name = dlsym(handle, "haiku_audio_" #name))) { WARN("Missing haiku_audio_" #name "\n"); dlclose(handle); return FALSE; }
    LOAD(open) LOAD(start) LOAD(stop) LOAD(close) LOAD(latency)
#undef LOAD
#define LOAD(name) hs.name = dlsym(handle, "haiku_audio_" #name);
    LOAD(list_inputs) LOAD(record_open) LOAD(record_start) LOAD(record_stop) LOAD(record_close)
    LOAD(midi_list) LOAD(midi_in_open) LOAD(midi_in_close) LOAD(midi_out_open) LOAD(midi_send) LOAD(midi_out_close)
#undef LOAD
    hs.handle = handle;
    return TRUE;
}

/* the BSoundPlayer sample format for a Windows one, or -1 */
static int sample_kind(const WAVEFORMATEX *fmt)
{
    BOOL is_float;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        if (IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) is_float = FALSE;
        else if (IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) is_float = TRUE;
        else return -1;
    }
    else if (fmt->wFormatTag == WAVE_FORMAT_PCM) is_float = FALSE;
    else if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) is_float = TRUE;
    else return -1;

    if (fmt->nChannels < 1 || fmt->nChannels > 2) return -1;
    if (fmt->wBitsPerSample == 8 && !is_float) return HS_UINT8;
    if (fmt->wBitsPerSample == 16 && !is_float) return HS_INT16;
    if (fmt->wBitsPerSample == 32) return is_float ? HS_FLOAT : HS_INT32;
    return -1;
}

static WAVEFORMATEX *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEX *ret;
    size_t size;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc(size);
    if(!ret)
        return NULL;

    memcpy(ret, fmt, size);

    ret->cbSize = size - sizeof(WAVEFORMATEX);

    return ret;
}

static void silence_buffer(struct haiku_stream *stream, BYTE *buffer, UINT32 frames)
{
    if (stream->sample_kind == HS_UINT8) memset(buffer, 128, frames * stream->fmt->nBlockAlign);
    else memset(buffer, 0, frames * stream->fmt->nBlockAlign);
}

static void apply_volume(struct haiku_stream *stream, BYTE *buffer, UINT32 frames)
{
    UINT32 i, c, channels = stream->fmt->nChannels;
    BOOL unity = stream->master_volume == 1.0f;

    for (c = 0; c < channels && c < MAX_CHANNELS; c++) if (stream->volumes[c] != 1.0f) unity = FALSE;
    if (unity) return;

    for (i = 0; i < frames; i++)
    {
        for (c = 0; c < channels; c++)
        {
            float v = stream->master_volume * stream->volumes[c < MAX_CHANNELS ? c : MAX_CHANNELS - 1];
            switch (stream->sample_kind)
            {
            case HS_FLOAT: ((float *)buffer)[i * channels + c] *= v; break;
            case HS_INT16: ((short *)buffer)[i * channels + c] = (short)(((short *)buffer)[i * channels + c] * v); break;
            case HS_INT32: ((int *)buffer)[i * channels + c] = (int)(((int *)buffer)[i * channels + c] * (double)v); break;
            case HS_UINT8: buffer[i * channels + c] = (BYTE)(128 + (buffer[i * channels + c] - 128) * v); break;
            }
        }
    }
}

/* BSoundPlayer wants data: this runs in a thread that knows nothing of Wine, so it must not call anything of it (no TRACE) */
static void fill_buffer(void *cookie, void *data, size_t bytes)
{
    struct haiku_stream *stream = cookie;
    UINT32 nframes = bytes / stream->fmt->nBlockAlign, to_copy_frames, to_copy_bytes, chunk_bytes, lcl_offs_bytes;

    pthread_mutex_lock(&stream->lock);

    if (stream->playing)
    {
        lcl_offs_bytes = stream->lcl_offs_frames * stream->fmt->nBlockAlign;
        to_copy_frames = min(nframes, stream->held_frames);
        to_copy_bytes = to_copy_frames * stream->fmt->nBlockAlign;

        chunk_bytes = (stream->bufsize_frames - stream->lcl_offs_frames) * stream->fmt->nBlockAlign;

        if (to_copy_bytes > chunk_bytes)
        {
            memcpy(data, stream->local_buffer + lcl_offs_bytes, chunk_bytes);
            memcpy((BYTE *)data + chunk_bytes, stream->local_buffer, to_copy_bytes - chunk_bytes);
        }
        else
            memcpy(data, stream->local_buffer + lcl_offs_bytes, to_copy_bytes);

        stream->lcl_offs_frames += to_copy_frames;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->held_frames -= to_copy_frames;
        apply_volume(stream, data, to_copy_frames);
    }
    else
        to_copy_bytes = to_copy_frames = 0;

    if (nframes > to_copy_frames)
        silence_buffer(stream, (BYTE *)data + to_copy_bytes, nframes - to_copy_frames);

    pthread_mutex_unlock(&stream->lock);
}

static void wrap_buffer(BYTE *dst, UINT32 dst_offs, UINT32 dst_bytes, BYTE *src, UINT32 src_bytes)
{
    UINT32 chunk_bytes = dst_bytes - dst_offs;

    if (chunk_bytes < src_bytes)
    {
        memcpy(dst + dst_offs, src, chunk_bytes);
        memcpy(dst, src + chunk_bytes, src_bytes - chunk_bytes);
    }
    else
        memcpy(dst + dst_offs, src, src_bytes);
}

/* the inputs of the hardware: names and the values that choose them */
static int list_inputs(char names[][64], int *values, int *current)
{
    *current = -1;
    if (!load_shim() || !hs.list_inputs) return 0;
    return hs.list_inputs(names, values, MAX_INPUTS, current);
}

/* the value of the input an endpoint id names, or -1 */
static int input_from_device(const char *device)
{
    if (!device || strncmp(device, INPUT_ID_PREFIX, sizeof(INPUT_ID_PREFIX) - 1)) return -1;
    return atoi(device + sizeof(INPUT_ID_PREFIX) - 1);
}

static NTSTATUS unix_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    static const WCHAR name[] = { 'H', 'a', 'i', 'k', 'u', ' ', 'a', 'u', 'd', 'i', 'o', ' ', 'o', 'u', 't', 'p', 'u', 't', 0 };
    static const WCHAR in_prefix[] = { 'H', 'a', 'i', 'k', 'u', ' ', 0 };
    const SIZE_T name_bytes = sizeof(name), device_bytes = (sizeof(DEVICE_ID) + 1) & ~1;
    unsigned int needed, i;
    struct endpoint *endpoint = params->endpoints;
    char names[MAX_INPUTS][64];
    int values[MAX_INPUTS], current, count;

    params->num = 0;
    params->default_idx = 0;

    if (params->flow == eCapture)
    {
        char device[32];
        char *p;

        count = list_inputs(names, values, &current);
        needed = 0;
        for (i = 0; i < count; i++)
        {
            sprintf(device, INPUT_ID_PREFIX "%d", values[i]);
            needed += sizeof(*endpoint) + sizeof(in_prefix) - sizeof(WCHAR) + (strlen(names[i]) + 1) * sizeof(WCHAR)
                      + ((strlen(device) + 2) & ~1);
        }
        params->num = count;
        if (needed > params->size)
        {
            params->size = needed;
            params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            return STATUS_SUCCESS;
        }
        p = (char *)params->endpoints + count * sizeof(*endpoint);
        for (i = 0; i < count; i++)
        {
            SIZE_T j, len = strlen(names[i]);

            endpoint[i].name = p - (char *)params->endpoints;
            memcpy(p, in_prefix, sizeof(in_prefix) - sizeof(WCHAR));
            p += sizeof(in_prefix) - sizeof(WCHAR);
            for (j = 0; j <= len; j++, p += sizeof(WCHAR)) *(WCHAR *)p = (unsigned char)names[i][j];
            sprintf(device, INPUT_ID_PREFIX "%d", values[i]);
            endpoint[i].device = p - (char *)params->endpoints;
            memcpy(p, device, strlen(device) + 1);
            p += (strlen(device) + 2) & ~1;
            if (values[i] == current) params->default_idx = i;
        }
        params->result = S_OK;
        return STATUS_SUCCESS;
    }
    if (params->flow != eRender)
    {
        params->result = E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    needed = sizeof(*endpoint) + name_bytes + device_bytes;
    params->num = 1;
    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        return STATUS_SUCCESS;
    }

    endpoint->name = sizeof(*endpoint);
    memcpy((char *)params->endpoints + endpoint->name, name, name_bytes);
    endpoint->device = endpoint->name + name_bytes;
    memcpy((char *)params->endpoints + endpoint->device, DEVICE_ID, sizeof(DEVICE_ID));
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* the capture side: data comes from a thread of BMediaRecorder, which must not call anything of Wine */
static void record_buffer(void *cookie, const void *data, size_t frames)
{
    struct haiku_stream *stream = cookie;
    UINT32 block = stream->fmt->nBlockAlign, n = frames, chunk;

    pthread_mutex_lock(&stream->lock);
    if (stream->playing)
    {
        if (n > stream->bufsize_frames) n = stream->bufsize_frames;
        if (stream->held_frames + n > stream->bufsize_frames)    /* overrun: the oldest data goes */
        {
            UINT32 drop = stream->held_frames + n - stream->bufsize_frames;

            stream->lcl_offs_frames = (stream->lcl_offs_frames + drop) % stream->bufsize_frames;
            stream->held_frames -= drop;
        }
        chunk = stream->bufsize_frames - stream->wri_offs_frames;
        if (n > chunk)
        {
            memcpy(stream->local_buffer + stream->wri_offs_frames * block, data, chunk * block);
            memcpy(stream->local_buffer, (const BYTE *)data + chunk * block, (n - chunk) * block);
        }
        else
            memcpy(stream->local_buffer + stream->wri_offs_frames * block, data, n * block);
        stream->wri_offs_frames = (stream->wri_offs_frames + n) % stream->bufsize_frames;
        stream->held_frames += n;
    }
    pthread_mutex_unlock(&stream->lock);
}

static NTSTATUS unix_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct haiku_stream *stream;
    SIZE_T size;
    int kind;

    params->result = S_OK;

    if (params->flow != eRender && params->flow != eCapture)
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if ((kind = sample_kind(params->fmt)) < 0)
    {
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        return STATUS_SUCCESS;
    }
    if (!load_shim())
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (!(stream = calloc(1, sizeof(*stream))))
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    pthread_mutex_init(&stream->lock, NULL);

    stream->fmt = clone_format(params->fmt);
    if (!stream->fmt)
    {
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    stream->sample_kind = kind;
    stream->flags = params->flags;
    stream->share = params->share;
    stream->master_volume = 1.0f;
    for (size = 0; size < MAX_CHANNELS; size++) stream->volumes[size] = 1.0f;

    stream->period_frames = muldiv(params->period, stream->fmt->nSamplesPerSec, 10000000);
    if (stream->period_frames == 0) stream->period_frames = muldiv(PERIOD_FRAMES, stream->fmt->nSamplesPerSec, MIX_RATE);
    stream->period = muldiv(stream->period_frames, 10000000, stream->fmt->nSamplesPerSec);
    stream->bufsize_frames = muldiv(params->duration, stream->fmt->nSamplesPerSec, 10000000);
    if (stream->bufsize_frames < stream->period_frames * 2) stream->bufsize_frames = stream->period_frames * 2;

    size = stream->bufsize_frames * stream->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                                &size, MEM_COMMIT, PAGE_READWRITE))
    {
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    silence_buffer(stream, stream->local_buffer, stream->bufsize_frames);

    if (params->flow == eCapture)
    {
        int rate = 0, input = input_from_device(params->device);

        stream->capture = TRUE;
        if (input < 0 || !hs.record_open)
        {
            params->result = AUDCLNT_E_DEVICE_INVALIDATED;
            goto end;
        }
        if (!(stream->recorder = hs.record_open(stream->fmt->nChannels, kind, input, record_buffer, stream, &rate)))
        {
            WARN("The input could not be opened for recording\n");
            params->result = AUDCLNT_E_DEVICE_INVALIDATED;
            goto end;
        }
        if (rate && rate != stream->fmt->nSamplesPerSec)
        {
            WARN("The input records at %d Hz, not %d\n", rate, (int)stream->fmt->nSamplesPerSec);
            params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
            goto end;
        }
        /* like the playback, record from the start; what comes while the stream is stopped is dropped */
        if (!hs.record_start(stream->recorder))
        {
            WARN("The recording did not start\n");
            params->result = AUDCLNT_E_DEVICE_INVALIDATED;
            goto end;
        }
        params->result = S_OK;
        goto end;
    }

    if (!(stream->player = hs.open(stream->fmt->nSamplesPerSec, stream->fmt->nChannels, kind, stream->period_frames,
                                   fill_buffer, stream)))
    {
        WARN("BSoundPlayer could not be opened\n");
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }
    /* like the CoreAudio driver, play continuously: starting the player can take a while */
    if (!hs.start(stream->player))
    {
        WARN("BSoundPlayer did not start\n");
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }
    params->result = S_OK;

end:
    if (FAILED(params->result))
    {
        if (stream->player) hs.close(stream->player);
        if (stream->recorder) hs.record_close(stream->recorder);
        if (stream->local_buffer)
        {
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE);
        }
        free(stream->fmt);
        free(stream);
    }
    else
    {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;

    if (stream->timer_thread)
    {
        stream->please_quit = TRUE;
        NtWaitForSingleObject(stream->timer_thread, FALSE, NULL);
        NtClose(stream->timer_thread);
    }

    if (stream->player) hs.close(stream->player);    /* waits for the callback to be over */
    if (stream->recorder) hs.record_close(stream->recorder);

    if (stream->local_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE);
    }
    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
    }
    pthread_mutex_destroy(&stream->lock);
    free(stream->fmt);
    free(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if (params->flow != eRender && params->flow != eCapture)
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.nChannels = 2;
    fmt->Format.nSamplesPerSec = MIX_RATE;
    fmt->Format.wBitsPerSample = 32;
    fmt->Format.nBlockAlign = fmt->Format.nChannels * fmt->Format.wBitsPerSample / 8;
    fmt->Format.nAvgBytesPerSec = fmt->Format.nSamplesPerSec * fmt->Format.nBlockAlign;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt->Samples.wValidBitsPerSample = 32;
    fmt->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;

    if (params->flow != eRender && params->flow != eCapture) params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (sample_kind(params->fmt_in) < 0) params->result = S_FALSE;
    else if (params->flow == eCapture && params->fmt_in->nSamplesPerSec != MIX_RATE) params->result = S_FALSE;
    else params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = muldiv(PERIOD_FRAMES, 10000000, MIX_RATE);
    if (params->min_period) *params->min_period = muldiv(PERIOD_FRAMES, 10000000, MIX_RATE);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    *params->frames = stream->bufsize_frames;
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    /* BSoundPlayer's latency is in microseconds; pretend we process audio in Period chunks, so the latency includes one */
    if (stream->capture) *params->latency = stream->period;
    else *params->latency = hs.latency(stream->player) * 10 + stream->period;
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    *params->padding = stream->held_frames;
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static void unix_timer_loop(void *args)
{
    struct haiku_stream *stream = args;
    LARGE_INTEGER delay, next, last;
    int adjust;

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter(&last, NULL);
    next.QuadPart = last.QuadPart + stream->period;

    while (!stream->please_quit)
    {
        if (stream->event) NtSetEvent(stream->event, NULL);
        NtDelayExecution(FALSE, &delay);
        NtQueryPerformanceCounter(&last, NULL);

        adjust = next.QuadPart - last.QuadPart;
        if (adjust > stream->period / 2)
            adjust = stream->period / 2;
        else if (adjust < -stream->period / 2)
            adjust = -stream->period / 2;

        delay.QuadPart = -(stream->period + adjust);
        next.QuadPart += stream->period;
    }
}

static NTSTATUS unix_start(void *args)
{
    struct start_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    static const WCHAR name[] = { 'a', 'u', 'd', 'i', 'o', '_', 'c', 'l', 'i', 'e', 'n', 't', '_', 't', 'i', 'm', 'e', 'r', 0 };

    pthread_mutex_lock(&stream->lock);

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
    else if (stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else
    {
        stream->playing = TRUE;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    if (!stream->timer_thread) create_unix_thread( &stream->timer_thread, name, unix_timer_loop, stream );

    return STATUS_SUCCESS;
}

static NTSTATUS unix_stop(void *args)
{
    struct stop_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if (!stream->playing)
        params->result = S_FALSE;
    else
    {
        stream->playing = FALSE;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_reset(void *args)
{
    struct reset_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if (stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else if (stream->getbuf_last)
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
    else
    {
        stream->written_frames = 0;
        stream->held_frames = 0;
        stream->lcl_offs_frames = 0;
        stream->wri_offs_frames = 0;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;
    UINT32 pad;

    pthread_mutex_lock(&stream->lock);

    pad = stream->held_frames;

    if (stream->getbuf_last)
    {
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }
    if (!params->frames)
    {
        params->result = S_OK;
        goto end;
    }
    if (pad + params->frames > stream->bufsize_frames)
    {
        params->result = AUDCLNT_E_BUFFER_TOO_LARGE;
        goto end;
    }

    if (stream->wri_offs_frames + params->frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < params->frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = params->frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                        &size, MEM_COMMIT, PAGE_READWRITE))
            {
                stream->tmp_buffer_frames = 0;
                params->result = E_OUTOFMEMORY;
                goto end;
            }
            stream->tmp_buffer_frames = params->frames;
        }
        *params->data = stream->tmp_buffer;
        stream->getbuf_last = -params->frames;
    }
    else
    {
        *params->data = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        stream->getbuf_last = params->frames;
    }

    silence_buffer(stream, *params->data, params->frames);
    params->result = S_OK;

end:
    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    BYTE *buffer;

    pthread_mutex_lock(&stream->lock);

    if (!params->written_frames)
    {
        stream->getbuf_last = 0;
        params->result = S_OK;
    }
    else if (!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if (params->written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        params->result = AUDCLNT_E_INVALID_SIZE;
    else
    {
        if (stream->getbuf_last >= 0)
            buffer = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        else
            buffer = stream->tmp_buffer;

        if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
            silence_buffer(stream, buffer, params->written_frames);

        if (stream->getbuf_last < 0)
            wrap_buffer(stream->local_buffer, stream->wri_offs_frames * stream->fmt->nBlockAlign,
                        stream->bufsize_frames * stream->fmt->nBlockAlign,
                        buffer, params->written_frames * stream->fmt->nBlockAlign);

        stream->wri_offs_frames += params->written_frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        stream->held_frames += params->written_frames;
        stream->written_frames += params->written_frames;
        stream->getbuf_last = 0;

        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    UINT32 chunk_bytes, chunk_frames;
    LARGE_INTEGER stamp, freq;
    SIZE_T size;

    pthread_mutex_lock(&stream->lock);

    if (stream->getbuf_last)
    {
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }

    *params->frames = 0;

    if (stream->held_frames < stream->period_frames)
    {
        params->result = AUDCLNT_S_BUFFER_EMPTY;
        goto end;
    }

    *params->flags = 0;
    chunk_frames = stream->bufsize_frames - stream->lcl_offs_frames;
    if (chunk_frames < stream->period_frames)
    {
        chunk_bytes = chunk_frames * stream->fmt->nBlockAlign;
        if (!stream->tmp_buffer)
        {
            size = stream->period_frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                        &size, MEM_COMMIT, PAGE_READWRITE))
            {
                params->result = E_OUTOFMEMORY;
                goto end;
            }
        }
        *params->data = stream->tmp_buffer;
        memcpy(*params->data, stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign, chunk_bytes);
        memcpy(*params->data + chunk_bytes, stream->local_buffer,
               stream->period_frames * stream->fmt->nBlockAlign - chunk_bytes);
    }
    else
        *params->data = stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign;

    stream->getbuf_last = *params->frames = stream->period_frames;

    if (params->devpos)
        *params->devpos = stream->written_frames;
    if (params->qpcpos)
    {
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }
    params->result = S_OK;

end:
    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if (!params->done)
    {
        stream->getbuf_last = 0;
        params->result = S_OK;
    }
    else if (!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if (stream->getbuf_last != params->done)
        params->result = AUDCLNT_E_INVALID_SIZE;
    else
    {
        stream->written_frames += params->done;
        stream->held_frames -= params->done;
        stream->lcl_offs_frames += params->done;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->getbuf_last = 0;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    *params->frames = stream->held_frames >= stream->period_frames ? stream->period_frames : 0;
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_position(void *args)
{
    struct get_position_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    LARGE_INTEGER stamp, freq;

    if (params->device)
    {
        FIXME("Device position reporting not implemented\n");
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    pthread_mutex_lock(&stream->lock);

    if (stream->capture) *params->pos = stream->written_frames + stream->held_frames;    /* what the hardware has recorded */
    else *params->pos = stream->written_frames - stream->held_frames;

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->pos *= stream->fmt->nBlockAlign;

    if (params->qpctime)
    {
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    pthread_mutex_unlock(&stream->lock);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_started(void *args)
{
    struct is_started_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);

    params->result = stream->playing ? S_OK : S_FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_prop_value(void *args)
{
    struct get_prop_value_params *params = args;

    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    UINT32 i;

    pthread_mutex_lock(&stream->lock);
    stream->master_volume = params->master_volume;
    for (i = 0; i < stream->fmt->nChannels && i < MAX_CHANNELS; i++)
        stream->volumes[i] = params->session_volumes[i] * params->volumes[i];
    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct haiku_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;

    pthread_mutex_lock(&stream->lock);
    if (!stream->player && !stream->recorder)
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if (stream->event)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else
        stream->event = params->event;
    pthread_mutex_unlock(&stream->lock);

    params->result = hr;
    return STATUS_SUCCESS;
}

/*
 * MIDI: the endpoints of Haiku's MIDI kit (a keyboard, a soft synth) are the MIDI ports of Windows programs. Modelled on the CoreMIDI
 * driver of Wine. A program that opens a port gets an endpoint of its own in the MIDI kit ("Wine: <port>", with the icon of Wine, see
 * haikushim.cpp), connected to the endpoint of the port; it is gone when the port is closed. The ports are the endpoints there are
 * when the program starts to use MIDI.
 */

#define MAX_MIDI_PORTS 64

struct midi_dest
{
    int endpoint;              /* the id of the endpoint in the MIDI kit */
    void *handle;              /* our producer, while the port is open */

    MIDIOUTCAPSW caps;
    MIDIOPENDESC midiDesc;
    BYTE runningStatus;
    WORD wFlags;
};

struct midi_src
{
    int endpoint;
    void *handle;              /* our consumer, while the port is open */

    WORD wDevID;
    int state; /* 0 is no recording started, 1 in recording, bit 2 set if in sys exclusive recording */
    MIDIINCAPSW caps;
    MIDIOPENDESC midiDesc;
    LPMIDIHDR lpQueueHdr;
    WORD wFlags;
    UINT startTime;
};

static UINT num_dests, num_srcs;
static struct midi_dest *dests;
static struct midi_src *srcs;
static BOOL midi_ready;

static pthread_mutex_t midi_in_mutex = PTHREAD_MUTEX_INITIALIZER;

#define NOTIFY_BUFFER_SIZE 64 + 1 /* + 1 for the sentinel */
static pthread_mutex_t notify_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notify_cond = PTHREAD_COND_INITIALIZER;
static BOOL notify_quit;
static struct notify_context notify_buffer[NOTIFY_BUFFER_SIZE];
static struct notify_context *notify_read, *notify_write;

static void midi_in_lock(BOOL lock)
{
    if (lock) pthread_mutex_lock(&midi_in_mutex);
    else pthread_mutex_unlock(&midi_in_mutex);
}

static void set_in_notify(struct notify_context *notify, struct midi_src *src, WORD dev_id, WORD msg,
                          UINT_PTR param_1, UINT_PTR param_2)
{
    notify->send_notify = TRUE;
    notify->dev_id = dev_id;
    notify->msg = msg;
    notify->param_1 = param_1;
    notify->param_2 = param_2;
    notify->callback = src->midiDesc.dwCallback;
    notify->flags = src->wFlags;
    notify->device = src->midiDesc.hMidi;
    notify->instance = src->midiDesc.dwInstance;
}

/*
 * notify buffer: The notification ring buffer is implemented so that
 * there is always at least one unused sentinel before the current
 * read position in order to allow detection of the full vs empty
 * state.
 */
static struct notify_context *notify_buffer_next(struct notify_context *notify)
{
    if (++notify >= notify_buffer + ARRAY_SIZE(notify_buffer))
        notify = notify_buffer;

    return notify;
}

static void notify_buffer_add(struct notify_context *notify)
{
    struct notify_context *next = notify_buffer_next(notify_write);

    if (next == notify_read) /* buffer is full - we can't issue a WARN() in a non-Win32 thread */
        notify_read = notify_buffer_next(notify_read); /* drop the oldest notification */
    *notify_write = *notify;
    notify_write = next;
}

static BOOL notify_buffer_empty(void)
{
    return notify_read == notify_write;
}

static BOOL notify_buffer_remove(struct notify_context *notify)
{
    if (notify_buffer_empty()) return FALSE;

    *notify = *notify_read;
    notify_read = notify_buffer_next(notify_read);
    return TRUE;
}

static void notify_post(struct notify_context *notify)
{
    pthread_mutex_lock(&notify_mutex);

    if (notify) notify_buffer_add(notify);
    else notify_quit = TRUE;
    pthread_cond_signal(&notify_cond);

    pthread_mutex_unlock(&notify_mutex);
}

/* the thread of the MIDI kit that calls the functions below knows nothing of Wine: no debug channels, no critical sections */
static UINT get_time_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (UINT)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000);
}

static void process_sysex_packet(struct midi_src *src, const unsigned char *data, unsigned int len)
{
    unsigned int pos = 0, copy_len;
    UINT current_time = get_time_ms() - src->startTime;
    struct notify_context notify;

    src->state |= 2;

    midi_in_lock(TRUE);

    while (len)
    {
        MIDIHDR *hdr = src->lpQueueHdr;
        if (!hdr) break;

        copy_len = min(len, hdr->dwBufferLength - hdr->dwBytesRecorded);
        memcpy(hdr->lpData + hdr->dwBytesRecorded, data + pos, copy_len);
        hdr->dwBytesRecorded += copy_len;
        len -= copy_len;
        pos += copy_len;

        if ((hdr->dwBytesRecorded == hdr->dwBufferLength) ||
            (*(BYTE*)(hdr->lpData + hdr->dwBytesRecorded - 1) == 0xf7))
        { /* buffer full or end of sysex message */
            src->lpQueueHdr = hdr->lpNext;
            hdr->dwFlags &= ~MHDR_INQUEUE;
            hdr->dwFlags |= MHDR_DONE;
            set_in_notify(&notify, src, src->wDevID, MIM_LONGDATA, (UINT_PTR)hdr, current_time);
            notify_post(&notify);
            src->state &= ~2;
        }
    }

    midi_in_lock(FALSE);
}

static void process_small_packet(struct midi_src *src, const unsigned char *data, unsigned int length)
{
    UINT current_time = get_time_ms() - src->startTime, value;
    struct notify_context notify;
    unsigned int pos = 0;

    while (pos < length)
    {
        value = 0;
        switch (data[pos] & 0xf0)
        {
        case 0xf0:
            value = data[pos];
            pos++;
            break;
        case 0xc0:
        case 0xd0:
            value = ((pos + 1 < length ? data[pos + 1] : 0) << 8) | data[pos];
            pos += 2;
            break;
        default:
            value = ((pos + 2 < length ? data[pos + 2] : 0) << 16) | ((pos + 1 < length ? data[pos + 1] : 0) << 8) | data[pos];
            pos += 3;
            break;
        }
        set_in_notify(&notify, src, src->wDevID, MIM_DATA, value, current_time);
        notify_post(&notify);
    }
}

/* data from the endpoint of a port that is open (MIDI kit's thread) */
static void midi_in_read_proc(void *cookie, const unsigned char *data, size_t length, long long time)
{
    struct midi_src *src = cookie;

    if (src->state < 1 || !length) /* input not started */
        return;

    if (data[0] == 0xf0 || src->state & 2)
        process_sysex_packet(src, data, length);
    else
        process_small_packet(src, data, length);
}

static void fill_caps_name(WCHAR *dst, unsigned int size, const char *name)
{
    unsigned int i;

    for (i = 0; i + 1 < size && name[i]; i++) dst[i] = (unsigned char)name[i];
    dst[i] = 0;
}

static BOOL name_has_synth(const char *name)
{
    char lower[64];
    unsigned int i;

    for (i = 0; i < sizeof(lower) - 1 && name[i]; i++) lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    lower[i] = 0;
    return strstr(lower, "synth") != NULL;
}

static NTSTATUS unix_midi_init(void *args)
{
    struct midi_init_params *params = args;
    char names[MAX_MIDI_PORTS][64];
    int ids[MAX_MIDI_PORTS];
    UINT i;
    int count;

    pthread_mutex_lock(&notify_mutex);
    notify_quit = FALSE;
    notify_read = notify_write = notify_buffer;
    pthread_mutex_unlock(&notify_mutex);

    /* without the MIDI kit (or without the bridge) the driver is left alone by winmm */
    if (!load_shim() || !hs.midi_list || !hs.midi_in_open || !hs.midi_out_open)
    {
        *params->err = DRV_FAILURE;
        return STATUS_SUCCESS;
    }

    count = hs.midi_list(1, names, ids, MAX_MIDI_PORTS);
    num_srcs = count > 0 ? count : 0;
    srcs = calloc(num_srcs ? num_srcs : 1, sizeof(*srcs));
    for (i = 0; i < num_srcs; i++)
    {
        srcs[i].wDevID = i;
        srcs[i].endpoint = ids[i];
        fill_caps_name(srcs[i].caps.szPname, ARRAY_SIZE(srcs[i].caps.szPname), names[i]);
        srcs[i].state = 0;
        srcs[i].caps.wMid = 0x00FF;     /* Manufac ID */
        srcs[i].caps.wPid = 0x0001;     /* Product ID */
        srcs[i].caps.vDriverVersion = 0x0001;
        srcs[i].caps.dwSupport = 0;
    }

    count = hs.midi_list(0, names, ids, MAX_MIDI_PORTS);
    num_dests = count > 0 ? count : 0;
    dests = calloc(num_dests ? num_dests : 1, sizeof(*dests));
    for (i = 0; i < num_dests; i++)
    {
        dests[i].endpoint = ids[i];
        fill_caps_name(dests[i].caps.szPname, ARRAY_SIZE(dests[i].caps.szPname), names[i]);
        dests[i].caps.wTechnology = name_has_synth(names[i]) ? MOD_SWSYNTH : MOD_MIDIPORT;
        dests[i].caps.wChannelMask = 0xFFFF;
        dests[i].caps.wMid = 0x00FF;
        dests[i].caps.wPid = 0x0001;
        dests[i].caps.vDriverVersion = 0x0001;
        dests[i].caps.dwSupport = 0;
        dests[i].caps.wVoices = 0;
        dests[i].caps.wNotes = 0;
    }

    TRACE("num_dests %d num_srcs %d\n", num_dests, num_srcs);

    midi_ready = TRUE;
    *params->err = DRV_SUCCESS;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_midi_release(void *args)
{
    UINT i;

    /* stop the notify_wait thread */
    notify_post(NULL);

    if (!midi_ready) return STATUS_SUCCESS;
    midi_ready = FALSE;

    for (i = 0; i < num_srcs; i++)
    {
        if (srcs[i].handle) hs.midi_in_close(srcs[i].handle);
        srcs[i].handle = NULL;
    }
    for (i = 0; i < num_dests; i++)
    {
        if (dests[i].handle) hs.midi_out_close(dests[i].handle);
        dests[i].handle = NULL;
    }
    free(srcs);
    free(dests);
    srcs = NULL;
    dests = NULL;
    num_srcs = num_dests = 0;

    return STATUS_SUCCESS;
}

static void set_out_notify(struct notify_context *notify, struct midi_dest *dest, WORD dev_id, WORD msg,
                           UINT_PTR param_1, UINT_PTR param_2)
{
    notify->send_notify = TRUE;
    notify->dev_id = dev_id;
    notify->msg = msg;
    notify->param_1 = param_1;
    notify->param_2 = param_2;
    notify->callback = dest->midiDesc.dwCallback;
    notify->flags = dest->wFlags;
    notify->device = dest->midiDesc.hMidi;
    notify->instance = dest->midiDesc.dwInstance;
}

static void port_name(char *buffer, size_t size, const WCHAR *name)
{
    size_t i;

    strcpy(buffer, "Wine: ");
    for (i = 0; i + 6 < size - 1 && name[i]; i++) buffer[6 + i] = name[i] < 128 ? (char)name[i] : '?';
    buffer[6 + i] = 0;
}

static UINT midi_out_open(WORD dev_id, MIDIOPENDESC *midi_desc, UINT flags, struct notify_context *notify)
{
    struct midi_dest *dest;
    char name[64];

    TRACE("dev_id = %d desc = %p flags = %08x\n", dev_id, midi_desc, flags);

    if (!midi_desc) return MMSYSERR_INVALPARAM;

    if (dev_id >= num_dests)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    if (dests[dev_id].midiDesc.hMidi != 0)
    {
        WARN("device already open!\n");
        return MMSYSERR_ALLOCATED;
    }
    if ((flags & ~CALLBACK_TYPEMASK) != 0)
    {
        WARN("bad flags\n");
        return MMSYSERR_INVALFLAG;
    }

    dest = dests + dev_id;
    port_name(name, sizeof(name), dest->caps.szPname);
    if (!(dest->handle = hs.midi_out_open(dest->endpoint, name)))
    {
        WARN("The MIDI endpoint could not be opened\n");
        return MMSYSERR_ERROR;
    }
    dest->runningStatus = 0;
    dest->wFlags = HIWORD(flags & CALLBACK_TYPEMASK);
    dest->midiDesc = *midi_desc;

    set_out_notify(notify, dest, dev_id, MOM_OPEN, 0, 0);

    return MMSYSERR_NOERROR;
}

static UINT midi_out_close(WORD dev_id, struct notify_context *notify)
{
    struct midi_dest *dest;

    TRACE("dev_id = %d\n", dev_id);

    if (dev_id >= num_dests)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }

    dest = dests + dev_id;

    if (dest->handle) hs.midi_out_close(dest->handle);
    dest->handle = NULL;

    set_out_notify(notify, dest, dev_id, MOM_CLOSE, 0, 0);

    dest->midiDesc.hMidi = 0;

    return MMSYSERR_NOERROR;
}

static UINT midi_out_data(WORD dev_id, UINT data)
{
    struct midi_dest *dest;
    unsigned char bytes[3];
    size_t count = sizeof(bytes);

    TRACE("dev_id = %d data = %08x\n", dev_id, data);

    if (dev_id >= num_dests)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    dest = dests + dev_id;

    bytes[0] = data & 0xff;
    if (bytes[0] & 0x80)
    {
        bytes[1] = (data >> 8) & 0xff;
        bytes[2] = (data >> 16) & 0xff;
        if (bytes[0] < 0xF0)
            dest->runningStatus = bytes[0];
        else if (bytes[0] <= 0xF7)
            dest->runningStatus = 0;
    }
    else if (dest->runningStatus)
    {
        bytes[0] = dest->runningStatus;
        bytes[1] = data & 0xff;
        bytes[2] = (data >> 8) & 0xff;
    }
    else
    {
        FIXME("ooch %x\n", data);
        return MMSYSERR_NOERROR;
    }

    /* MODM_DATA always includes 3 bytes, but some message types are only 1 or 2 bytes long */
    switch (bytes[0] & 0xF0)
    {
    case 0xC0:  /* Program Change */
    case 0xD0:  /* Channel Pressure (After-touch) */
        count = 2;
        break;
    case 0xF0:  /* system messages: the 1 byte ones (the real-time ones, tune request); song position (2 data bytes), song select (1) */
        if (bytes[0] == 0xF1 || bytes[0] == 0xF3) count = 2;
        else if (bytes[0] != 0xF2) count = 1;
        break;
    }

    if (dest->handle) hs.midi_send(dest->handle, bytes, count);

    return MMSYSERR_NOERROR;
}

static UINT midi_out_long_data(WORD dev_id, MIDIHDR *hdr, UINT hdr_size, struct notify_context *notify)
{
    struct midi_dest *dest;

    TRACE("dev_id = %d midi_hdr = %p hdr_size = %d\n", dev_id, hdr, hdr_size);

    if (dev_id >= num_dests)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    if (!hdr)
    {
        WARN("Invalid Parameter\n");
        return MMSYSERR_INVALPARAM;
    }
    if (!hdr->lpData || !(hdr->dwFlags & MHDR_PREPARED))
        return MIDIERR_UNPREPARED;

    if (hdr->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    hdr->dwFlags &= ~MHDR_DONE;
    hdr->dwFlags |= MHDR_INQUEUE;

    if ((unsigned char)hdr->lpData[0] != 0xf0)
        /* System Exclusive */
        ERR("Add missing 0xf0 marker at the beginning of system exclusive byte stream\n");

    if ((unsigned char)hdr->lpData[hdr->dwBufferLength - 1] != 0xF7)
        /* Send end of System Exclusive */
        ERR("Add missing 0xf7 marker at the end of system exclusive byte stream\n");

    dest = dests + dev_id;

    if (dest->handle) hs.midi_send(dest->handle, (const unsigned char *)hdr->lpData, hdr->dwBufferLength);

    dest->runningStatus = 0;
    hdr->dwFlags &= ~MHDR_INQUEUE;
    hdr->dwFlags |= MHDR_DONE;

    set_out_notify(notify, dest, dev_id, MOM_DONE, (UINT_PTR)hdr, 0);

    return MMSYSERR_NOERROR;
}

static UINT midi_out_prepare(WORD dev_id, MIDIHDR *hdr, UINT hdr_size)
{
    TRACE("dev_id = %d midi_hdr = %p hdr_size = %d\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & MHDR_PREPARED)
        return MMSYSERR_NOERROR;

    hdr->lpNext = 0;
    hdr->dwFlags |= MHDR_PREPARED;
    hdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_unprepare(WORD dev_id, MIDIHDR *hdr, UINT hdr_size)
{
    TRACE("dev_id = %d midi_hdr = %p hdr_size = %d\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED))
        return MMSYSERR_NOERROR;
    if (hdr->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    hdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static UINT midi_out_get_devcaps(WORD dev_id, MIDIOUTCAPSW *caps, UINT size)
{
    TRACE("dev_id = %d caps = %p size = %d\n", dev_id, caps, size);

    if (!caps)
    {
        WARN("Invalid Parameter\n");
        return MMSYSERR_INVALPARAM;
    }

    if (dev_id >= num_dests)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    memcpy(caps, &dests[dev_id].caps, min(size, sizeof(*caps)));
    return MMSYSERR_NOERROR;
}

static UINT midi_out_get_num_devs(void)
{
    TRACE("\n");
    return num_dests;
}

static UINT midi_out_reset(WORD dev_id)
{
    unsigned chn;

    TRACE("%d\n", dev_id);

    if (dev_id >= num_dests)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    if (dests[dev_id].handle)
    {
        for (chn = 0; chn < 16; chn++)
        {
            unsigned char off[3] = { 0xB0 | chn, 0x7B, 0 };    /* all notes off */
            unsigned char reset[3] = { 0xB0 | chn, 0x79, 0 };  /* resets controller settings */

            hs.midi_send(dests[dev_id].handle, off, 3);
            hs.midi_send(dests[dev_id].handle, reset, 3);
        }
    }

    dests[dev_id].runningStatus = 0;

    return MMSYSERR_NOERROR;
}

static UINT midi_in_open(WORD dev_id, MIDIOPENDESC *midi_desc, UINT flags, struct notify_context *notify)
{
    struct midi_src *src;
    char name[64];

    TRACE("dev_id = %d desc = %p flags = %08x\n", dev_id, midi_desc, flags);

    if (!midi_desc)
    {
        WARN("Invalid Parameter\n");
        return MMSYSERR_INVALPARAM;
    }
    if (dev_id >= num_srcs)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    src = srcs + dev_id;

    if (src->midiDesc.hMidi)
    {
        WARN("device already open !\n");
        return MMSYSERR_ALLOCATED;
    }
    if (flags & MIDI_IO_STATUS)
    {
        FIXME("No support for MIDI_IO_STATUS in flags yet, ignoring it\n");
        flags &= ~MIDI_IO_STATUS;
    }
    if (flags & ~CALLBACK_TYPEMASK)
    {
        FIXME("Bad flags\n");
        return MMSYSERR_INVALFLAG;
    }

    src->wFlags = HIWORD(flags & CALLBACK_TYPEMASK);
    src->lpQueueHdr = NULL;
    src->midiDesc = *midi_desc;
    src->startTime = 0;
    src->state = 0;

    port_name(name, sizeof(name), src->caps.szPname);
    if (!(src->handle = hs.midi_in_open(src->endpoint, name, midi_in_read_proc, src)))
    {
        WARN("The MIDI endpoint could not be opened\n");
        src->midiDesc.hMidi = 0;
        return MMSYSERR_ERROR;
    }

    set_in_notify(notify, src, dev_id, MIM_OPEN, 0, 0);

    return MMSYSERR_NOERROR;
}

static UINT midi_in_close(WORD dev_id, struct notify_context *notify)
{
    struct midi_src *src;

    TRACE("dev_id = %d\n", dev_id);

    if (dev_id >= num_srcs)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    src = srcs + dev_id;

    if (!src->midiDesc.hMidi)
    {
        WARN("device not opened !\n");
        return MMSYSERR_ERROR;
    }
    if (src->lpQueueHdr) return MIDIERR_STILLPLAYING;

    src->state = 0;
    if (src->handle) hs.midi_in_close(src->handle);    /* waits for the data function to be over */
    src->handle = NULL;

    set_in_notify(notify, src, dev_id, MIM_CLOSE, 0, 0);
    src->midiDesc.hMidi = 0;

    return MMSYSERR_NOERROR;
}

static UINT midi_in_add_buffer(WORD dev_id, MIDIHDR *hdr, UINT hdr_size)
{
    MIDIHDR **next;

    TRACE("dev_id = %d hdr = %p hdr_size = %d\n", dev_id, hdr, hdr_size);

    if (dev_id >= num_srcs)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    if (!hdr || hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr->dwBufferLength)
    {
        WARN("Invalid Parameter\n");
        return MMSYSERR_INVALPARAM;
    }
    if (hdr->dwFlags & MHDR_INQUEUE)
    {
        WARN("Still playing\n");
        return MIDIERR_STILLPLAYING;
    }
    if (!(hdr->dwFlags & MHDR_PREPARED))
    {
        WARN("Unprepared\n");
        return MIDIERR_UNPREPARED;
    }

    hdr->dwFlags &= ~WHDR_DONE;
    hdr->dwFlags |= MHDR_INQUEUE;
    hdr->dwBytesRecorded = 0;
    hdr->lpNext = NULL;

    midi_in_lock(TRUE);

    next = &srcs[dev_id].lpQueueHdr;
    while (*next) next = &(*next)->lpNext;
    *next = hdr;

    midi_in_lock(FALSE);

    return MMSYSERR_NOERROR;
}

static UINT midi_in_prepare(WORD dev_id, MIDIHDR *hdr, UINT hdr_size)
{
    TRACE("dev_id = %d hdr = %p hdr_size = %d\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & MHDR_PREPARED)
        return MMSYSERR_NOERROR;

    hdr->lpNext = NULL;
    hdr->dwFlags |= MHDR_PREPARED;
    hdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

static UINT midi_in_unprepare(WORD dev_id, MIDIHDR *hdr, UINT hdr_size)
{
    TRACE("dev_id = %d hdr = %p hdr_size = %d\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED))
        return MMSYSERR_NOERROR;
    if (hdr->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    hdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static UINT midi_in_get_devcaps(WORD dev_id, MIDIINCAPSW *caps, UINT size)
{
    TRACE("dev_id = %d caps = %p size = %d\n", dev_id, caps, size);

    if (!caps)
    {
        WARN("Invalid Parameter\n");
        return MMSYSERR_INVALPARAM;
    }
    if (dev_id >= num_srcs)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }

    memcpy(caps, &srcs[dev_id].caps, min(size, sizeof(*caps)));
    return MMSYSERR_NOERROR;
}

static UINT midi_in_get_num_devs(void)
{
    TRACE("\n");
    return num_srcs;
}

static UINT midi_in_start(WORD dev_id)
{
    TRACE("%d\n", dev_id);

    if (dev_id >= num_srcs)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    srcs[dev_id].startTime = get_time_ms();
    srcs[dev_id].state = 1;
    return MMSYSERR_NOERROR;
}

static UINT midi_in_stop(WORD dev_id)
{
    TRACE("%d\n", dev_id);

    if (dev_id >= num_srcs)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    srcs[dev_id].state = 0;
    return MMSYSERR_NOERROR;
}

static UINT midi_in_reset(WORD dev_id, struct notify_context *notify)
{
    UINT cur_time = get_time_ms();
    UINT err = MMSYSERR_NOERROR;
    struct midi_src *src;
    MIDIHDR *hdr;

    TRACE("%d\n", dev_id);

    if (dev_id >= num_srcs)
    {
        WARN("bad device ID : %d\n", dev_id);
        return MMSYSERR_BADDEVICEID;
    }
    src = srcs + dev_id;

    midi_in_lock(TRUE);

    if (src->lpQueueHdr)
    {
        hdr = src->lpQueueHdr;
        src->lpQueueHdr = hdr->lpNext;
        hdr->dwFlags &= ~MHDR_INQUEUE;
        hdr->dwFlags |= MHDR_DONE;
        set_in_notify(notify, src, dev_id, MIM_LONGDATA, (UINT_PTR)hdr, cur_time - src->startTime);
        if (src->lpQueueHdr) err = ERROR_RETRY; /* ask the client to call again */
    }

    midi_in_lock(FALSE);

    return err;
}

static NTSTATUS unix_midi_out_message(void *args)
{
    struct midi_out_message_params *params = args;

    params->notify->send_notify = FALSE;

    if (!midi_ready)
    {
        *params->err = params->msg == MODM_GETNUMDEVS ? 0 : MMSYSERR_NOTSUPPORTED;
        return STATUS_SUCCESS;
    }

    switch (params->msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case MODM_OPEN:
        *params->err = midi_out_open(params->dev_id, (MIDIOPENDESC *)params->param_1, params->param_2, params->notify);
        break;
    case MODM_CLOSE:
        *params->err = midi_out_close(params->dev_id, params->notify);
        break;
    case MODM_DATA:
        *params->err = midi_out_data(params->dev_id, params->param_1);
        break;
    case MODM_LONGDATA:
        *params->err = midi_out_long_data(params->dev_id, (MIDIHDR *)params->param_1, params->param_2, params->notify);
        break;
    case MODM_PREPARE:
        *params->err = midi_out_prepare(params->dev_id, (MIDIHDR *)params->param_1, params->param_2);
        break;
    case MODM_UNPREPARE:
        *params->err = midi_out_unprepare(params->dev_id, (MIDIHDR *)params->param_1, params->param_2);
        break;
    case MODM_GETDEVCAPS:
        *params->err = midi_out_get_devcaps(params->dev_id, (MIDIOUTCAPSW *)params->param_1, params->param_2);
        break;
    case MODM_GETNUMDEVS:
        *params->err = midi_out_get_num_devs();
        break;
    case MODM_GETVOLUME:
    case MODM_SETVOLUME:
        *params->err = MMSYSERR_NOTSUPPORTED;
        break;
    case MODM_RESET:
        *params->err = midi_out_reset(params->dev_id);
        break;
    default:
        TRACE("Unsupported message\n");
        *params->err = MMSYSERR_NOTSUPPORTED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_midi_in_message(void *args)
{
    struct midi_in_message_params *params = args;

    params->notify->send_notify = FALSE;

    if (!midi_ready)
    {
        *params->err = params->msg == MIDM_GETNUMDEVS ? 0 : MMSYSERR_NOTSUPPORTED;
        return STATUS_SUCCESS;
    }

    switch (params->msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case MIDM_OPEN:
        *params->err = midi_in_open(params->dev_id, (MIDIOPENDESC *)params->param_1, params->param_2, params->notify);
        break;
    case MIDM_CLOSE:
        *params->err = midi_in_close(params->dev_id, params->notify);
        break;
    case MIDM_ADDBUFFER:
        *params->err = midi_in_add_buffer(params->dev_id, (MIDIHDR *)params->param_1, params->param_2);
        break;
    case MIDM_PREPARE:
        *params->err = midi_in_prepare(params->dev_id, (MIDIHDR *)params->param_1, params->param_2);
        break;
    case MIDM_UNPREPARE:
        *params->err = midi_in_unprepare(params->dev_id, (MIDIHDR *)params->param_1, params->param_2);
        break;
    case MIDM_GETDEVCAPS:
        *params->err = midi_in_get_devcaps(params->dev_id, (MIDIINCAPSW *)params->param_1, params->param_2);
        break;
    case MIDM_GETNUMDEVS:
        *params->err = midi_in_get_num_devs();
        break;
    case MIDM_START:
        *params->err = midi_in_start(params->dev_id);
        break;
    case MIDM_STOP:
        *params->err = midi_in_stop(params->dev_id);
        break;
    case MIDM_RESET:
        *params->err = midi_in_reset(params->dev_id, params->notify);
        break;
    default:
        TRACE("Unsupported message\n");
        *params->err = MMSYSERR_NOTSUPPORTED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_midi_notify_wait(void *args)
{
    struct midi_notify_wait_params *params = args;

    pthread_mutex_lock(&notify_mutex);

    while (!notify_quit && notify_buffer_empty())
        pthread_cond_wait(&notify_cond, &notify_mutex);

    *params->quit = notify_quit;
    if (!notify_quit) notify_buffer_remove(params->notify);

    pthread_mutex_unlock(&notify_mutex);

    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_get_endpoint_ids,
    unix_create_stream,
    unix_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_get_render_buffer,
    unix_release_render_buffer,
    unix_get_capture_buffer,
    unix_release_capture_buffer,
    unix_is_format_supported,
    unix_not_implemented,
    unix_get_mix_format,
    unix_get_device_period,
    unix_get_buffer_size,
    unix_get_latency,
    unix_get_current_padding,
    unix_get_next_packet_size,
    unix_get_frequency,
    unix_get_position,
    unix_set_volumes,
    unix_set_event_handle,
    unix_not_implemented,
    unix_not_implemented,
    unix_is_started,
    unix_get_prop_value,
    unix_not_implemented,
    unix_midi_init,
    unix_midi_release,
    unix_midi_out_message,
    unix_midi_in_message,
    unix_midi_notify_wait,
    unix_not_implemented,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS unix_wow64_process_attach(void *args)
{
    SYSTEM_BASIC_INFORMATION info;

    NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_midi_init(void *args)
{
    struct { PTR32 err; } *params32 = args;
    struct midi_init_params params = { .err = ULongToPtr(params32->err) };
    return unix_midi_init(&params);
}

struct notify_context32
{
    BOOL send_notify;
    WORD dev_id;
    WORD msg;
    UINT param_1;
    UINT param_2;
    UINT callback;
    UINT flags;
    PTR32 device;
    UINT instance;
};

static void notify_to_notify32(struct notify_context32 *notify32,
                               const struct notify_context *notify)
{
    notify32->send_notify = notify->send_notify;
    notify32->dev_id = notify->dev_id;
    notify32->msg = notify->msg;
    notify32->param_1 = notify->param_1;
    notify32->param_2 = notify->param_2;
    notify32->callback = notify->callback;
    notify32->flags = notify->flags;
    notify32->device = PtrToUlong(notify->device);
    notify32->instance = notify->instance;
}

struct midi_open_desc32
{
    PTR32 hMidi;
    UINT dwCallback;
    UINT dwInstance;
    UINT dnDevNode;
    UINT cIds;
    MIDIOPENSTRMID rgIds;
};

struct midi_hdr32
{
    PTR32 lpData;
    UINT dwBufferLength;
    UINT dwBytesRecorded;
    UINT dwUser;
    UINT dwFlags;
    PTR32 lpNext;
    UINT reserved;
    UINT dwOffset;
    UINT dwReserved[8];
};

static UINT wow64_midi_out_prepare(WORD dev_id, struct midi_hdr32 *hdr, UINT hdr_size)
{
    TRACE("(%04X, %p, %d);\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(struct midi_hdr32, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & MHDR_PREPARED)
        return MMSYSERR_NOERROR;

    hdr->lpNext = 0;
    hdr->dwFlags |= MHDR_PREPARED;
    hdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

static UINT wow64_midi_out_unprepare(WORD dev_id, struct midi_hdr32 *hdr, UINT hdr_size)
{
    TRACE("(%04X, %p, %d);\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(struct midi_hdr32, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED))
        return MMSYSERR_NOERROR;
    if (hdr->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    hdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static NTSTATUS unix_wow64_midi_out_message(void *args)
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
        PTR32 notify;
    } *params32 = args;
    struct notify_context32 *notify32 = ULongToPtr(params32->notify);
    struct midi_open_desc32 *desc32;
    struct midi_hdr32 *hdr32;
    struct notify_context notify;
    MIDIOPENDESC open_desc;
    MIDIHDR hdr;
    struct midi_out_message_params params =
    {
        .dev_id = params32->dev_id,
        .msg = params32->msg,
        .user = params32->user,
        .param_1 = params32->param_1,
        .param_2 = params32->param_2,
        .err = ULongToPtr(params32->err),
        .notify = &notify
    };
    notify32->send_notify = FALSE;

    switch (params32->msg)
    {
    case MODM_OPEN:
        desc32 = ULongToPtr(params32->param_1);

        open_desc.hMidi = ULongToPtr(desc32->hMidi);
        open_desc.dwCallback = desc32->dwCallback;
        open_desc.dwInstance = desc32->dwInstance;
        open_desc.dnDevNode = desc32->dnDevNode;
        open_desc.cIds = desc32->cIds;
        open_desc.rgIds.dwStreamID = desc32->rgIds.dwStreamID;
        open_desc.rgIds.wDeviceID = desc32->rgIds.wDeviceID;

        params.param_1 = (UINT_PTR)&open_desc;
        break;

    case MODM_LONGDATA:
        hdr32 = ULongToPtr(params32->param_1);

        memset(&hdr, 0, sizeof(hdr));
        hdr.lpData = ULongToPtr(hdr32->lpData);
        hdr.dwBufferLength = hdr32->dwBufferLength;
        hdr.dwFlags = hdr32->dwFlags;

        params.param_1 = (UINT_PTR)&hdr;
        params.param_2 = sizeof(hdr);
        break;

    case MODM_PREPARE: /* prepare and unprepare are easier to handle explicitly */
        hdr32 = ULongToPtr(params32->param_1);

        *params.err = wow64_midi_out_prepare(params32->dev_id, hdr32, params32->param_2);
        return STATUS_SUCCESS;

    case MODM_UNPREPARE:
        hdr32 = ULongToPtr(params32->param_1);

        *params.err = wow64_midi_out_unprepare(params32->dev_id, hdr32, params32->param_2);
        return STATUS_SUCCESS;
    }

    unix_midi_out_message(&params);

    switch (params32->msg)
    {
    case MODM_LONGDATA:
        hdr32 = ULongToPtr(params32->param_1);

        hdr32->dwFlags = hdr.dwFlags;
        break;
    }

    if (notify.send_notify)
    {
        notify_to_notify32(notify32, &notify);

        if (notify.msg == MOM_DONE)
            notify32->param_1 = params32->param_1; /* restore the 32-bit hdr */
    }
    return STATUS_SUCCESS;
}

static UINT wow64_midi_in_prepare(WORD dev_id, struct midi_hdr32 *hdr, UINT hdr_size)
{
    TRACE("(%04X, %p, %d);\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(struct midi_hdr32, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & MHDR_PREPARED)
        return MMSYSERR_NOERROR;

    hdr->lpNext = 0;
    hdr->dwFlags |= MHDR_PREPARED;
    hdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);

    return MMSYSERR_NOERROR;
}

static UINT wow64_midi_in_unprepare(WORD dev_id, struct midi_hdr32 *hdr, UINT hdr_size)
{
    TRACE("(%04X, %p, %d);\n", dev_id, hdr, hdr_size);

    if (hdr_size < offsetof(struct midi_hdr32, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED))
        return MMSYSERR_NOERROR;
    if (hdr->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    hdr->dwFlags &= ~MHDR_PREPARED;

    return MMSYSERR_NOERROR;
}

static NTSTATUS unix_wow64_midi_in_message(void *args)
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
        PTR32 notify;
    } *params32 = args;
    struct notify_context32 *notify32 = ULongToPtr(params32->notify);
    struct midi_open_desc32 *desc32;
    struct midi_hdr32 *hdr32;
    struct notify_context notify;
    MIDIOPENDESC open_desc;
    MIDIHDR *hdr = NULL;
    struct midi_in_message_params params =
    {
        .dev_id = params32->dev_id,
        .msg = params32->msg,
        .user = params32->user,
        .param_1 = params32->param_1,
        .param_2 = params32->param_2,
        .err = ULongToPtr(params32->err),
        .notify = &notify
    };
    notify32->send_notify = FALSE;

    switch (params32->msg)
    {
    case MIDM_OPEN:
        desc32 = ULongToPtr(params32->param_1);

        open_desc.hMidi = ULongToPtr(desc32->hMidi);
        open_desc.dwCallback = desc32->dwCallback;
        open_desc.dwInstance = desc32->dwInstance;
        open_desc.dnDevNode = desc32->dnDevNode;
        open_desc.cIds = desc32->cIds;
        open_desc.rgIds.dwStreamID = desc32->rgIds.dwStreamID;
        open_desc.rgIds.wDeviceID = desc32->rgIds.wDeviceID;

        params.param_1 = (UINT_PTR)&open_desc;
        break;

    case MIDM_ADDBUFFER:
        hdr32 = ULongToPtr(params32->param_1);

        hdr = calloc(1, sizeof(*hdr));
        hdr->lpData = ULongToPtr(hdr32->lpData);
        hdr->dwBufferLength = hdr32->dwBufferLength;
        hdr->dwFlags = hdr32->dwFlags;
        hdr->dwReserved[7] = params32->param_1; /* keep hdr32 for MIM_LONGDATA notification */

        params.param_1 = (UINT_PTR)hdr;
        params.param_2 = sizeof(*hdr);
        break;

    case MIDM_PREPARE: /* prepare and unprepare are easier to handle explicitly */
        hdr32 = ULongToPtr(params32->param_1);

        *params.err = wow64_midi_in_prepare(params32->dev_id, hdr32, params32->param_2);
        return STATUS_SUCCESS;

    case MIDM_UNPREPARE:
        hdr32 = ULongToPtr(params32->param_1);

        *params.err = wow64_midi_in_unprepare(params32->dev_id, hdr32, params32->param_2);
        return STATUS_SUCCESS;
    }

    unix_midi_in_message(&params);

    switch (params32->msg)
    {
    case MIDM_ADDBUFFER:
        hdr32 = ULongToPtr(params32->param_1);

        if (!*params.err)
        {
            hdr32->dwFlags = hdr->dwFlags;
            hdr32->dwBytesRecorded = hdr->dwBytesRecorded;
            hdr32->lpNext = 0;
        }
        else
            free(hdr);
        break;
    }

    if (notify.send_notify)
    {
        notify_to_notify32(notify32, &notify);

        if (notify.msg == MIM_LONGDATA)
        {
            hdr = (MIDIHDR *)notify.param_1;
            notify32->param_1 = hdr->dwReserved[7];
            hdr32 = ULongToPtr(notify32->param_1);
            hdr32->dwBytesRecorded = hdr->dwBytesRecorded;
            hdr32->dwFlags = hdr->dwFlags;
            free(hdr);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_midi_notify_wait(void *args)
{
    struct
    {
        PTR32 quit;
        PTR32 notify;
    } *params32 = args;
    struct notify_context32 *notify32 = ULongToPtr(params32->notify);
    struct midi_hdr32 *hdr32;
    struct notify_context notify;
    MIDIHDR *hdr;
    struct midi_notify_wait_params params =
    {
        .quit = ULongToPtr(params32->quit),
        .notify = &notify
    };
    notify32->send_notify = FALSE;

    unix_midi_notify_wait(&params);

    if (!*params.quit && notify.send_notify)
    {
        notify_to_notify32(notify32, &notify);

        if (notify.msg == MIM_LONGDATA)
        {
            hdr = (MIDIHDR *)notify.param_1;
            notify32->param_1 = hdr->dwReserved[7];
            hdr32 = ULongToPtr(notify32->param_1);
            hdr32->dwBytesRecorded = hdr->dwBytesRecorded;
            hdr32->dwFlags = hdr->dwFlags;
            free(hdr);
        }
    }
    return STATUS_SUCCESS;
}


static NTSTATUS unix_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    unix_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    unix_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
    };
    unix_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    unix_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .data = &data,
        .frames = ULongToPtr(params32->frames),
        .flags = ULongToPtr(params32->flags),
        .devpos = ULongToPtr(params32->devpos),
        .qpcpos = ULongToPtr(params32->qpcpos)
    };
    unix_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
};

static NTSTATUS unix_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    unix_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    unix_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    unix_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    unix_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    unix_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    unix_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    unix_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return unix_set_volumes(&params);
}

static NTSTATUS unix_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };
    unix_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer; /* caller allocated buffer to hold value's strings */
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    unix_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
            value32->ptr = params32->buffer;
            break;
        default:
            FIXME("Unhandled vt %04x\n", value.vt);
        }
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    unix_wow64_process_attach,
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_wow64_get_endpoint_ids,
    unix_wow64_create_stream,
    unix_wow64_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_wow64_get_render_buffer,
    unix_release_render_buffer,
    unix_wow64_get_capture_buffer,
    unix_release_capture_buffer,
    unix_wow64_is_format_supported,
    unix_not_implemented,
    unix_wow64_get_mix_format,
    unix_wow64_get_device_period,
    unix_wow64_get_buffer_size,
    unix_wow64_get_latency,
    unix_wow64_get_current_padding,
    unix_wow64_get_next_packet_size,
    unix_wow64_get_frequency,
    unix_wow64_get_position,
    unix_wow64_set_volumes,
    unix_wow64_set_event_handle,
    unix_not_implemented,
    unix_not_implemented,
    unix_is_started,
    unix_wow64_get_prop_value,
    unix_not_implemented,
    unix_wow64_midi_init,
    unix_midi_release,
    unix_wow64_midi_out_message,
    unix_wow64_midi_in_message,
    unix_wow64_midi_notify_wait,
    unix_not_implemented,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
