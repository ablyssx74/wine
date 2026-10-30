/*
 * Unixlib for the winehaiku audio driver: plays through BSoundPlayer (see haikushim.cpp).
 *
 * Modelled on the winecoreaudio driver, which is also a driver that is called back for its data by the system. Only playback
 * is implemented so far: there is no capture device.
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
} hs;

struct haiku_stream
{
    pthread_mutex_t lock;
    void *player;
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

static NTSTATUS unix_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    static const WCHAR name[] = { 'H', 'a', 'i', 'k', 'u', ' ', 'a', 'u', 'd', 'i', 'o', ' ', 'o', 'u', 't', 'p', 'u', 't', 0 };
    const SIZE_T name_bytes = sizeof(name), device_bytes = (sizeof(DEVICE_ID) + 1) & ~1;
    unsigned int needed;
    struct endpoint *endpoint = params->endpoints;

    params->num = 0;
    params->default_idx = 0;

    if (params->flow == eCapture)    /* no capture yet */
    {
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

static NTSTATUS unix_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct haiku_stream *stream;
    SIZE_T size;
    int kind;

    params->result = S_OK;

    if (params->flow != eRender)
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

    if (params->flow != eRender)
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

    if (params->flow != eRender) params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (sample_kind(params->fmt_in) < 0) params->result = S_FALSE;
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
    *params->latency = hs.latency(stream->player) * 10 + stream->period;
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
    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    *params->frames = 0;
    params->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
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

    *params->pos = stream->written_frames - stream->held_frames;

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
    if (!stream->player)
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

/* no MIDI: failing to load makes the driver be left alone by winmm */
static NTSTATUS unix_midi_init(void *args)
{
    struct midi_init_params *params = args;
    *params->err = DRV_FAILURE;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_midi_notify_wait(void *args)
{
    struct midi_notify_wait_params *params = args;
    *params->quit = TRUE;
    params->notify->send_notify = FALSE;
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
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
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
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
