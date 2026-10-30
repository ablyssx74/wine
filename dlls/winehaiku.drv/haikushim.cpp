/*
 * The bridge between the Haiku audio driver of Wine (haiku.c, a C library) and BSoundPlayer, which is C++.
 * It is built on Haiku with build.sh into libwinehaikushim.so, which haiku.c loads from the directory of winehaiku.so.
 *
 * BSoundPlayer pulls: it calls the function given here from a thread of its own, with the buffer to fill. That thread knows
 * nothing about Wine, so the function must not call anything of it.
 *
 * Copyright 2026 the Wine Haiku port
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <Application.h>
#include <OS.h>
#include <SoundPlayer.h>
#include <pthread.h>
#include <stdlib.h>

typedef void (*hs_fill_func)(void *cookie, void *buffer, size_t bytes);

struct hs_player
{
    BSoundPlayer *player;
    hs_fill_func fill;
    void *cookie;
};

static pthread_mutex_t app_lock = PTHREAD_MUTEX_INITIALIZER;
static thread_id app_thread = -1;
static sem_id app_ready = -1;

static int32 app_main(void *)
{
    BApplication *app = new BApplication("application/x-vnd.wine-haikushim");
    release_sem(app_ready);
    app->Run();
    delete app;
    return 0;
}

/* make sure the process has a BApplication (the media kit wants one) */
static int ensure_application(void)
{
    int ok = 1;

    pthread_mutex_lock(&app_lock);
    if (be_app == NULL && app_thread < 0)
    {
        app_ready = create_sem(0, "haiku audio app ready");
        app_thread = spawn_thread(app_main, "haiku audio app", B_NORMAL_PRIORITY, NULL);
        if (app_thread < 0 || resume_thread(app_thread) != B_OK) ok = 0;
        else acquire_sem_etc(app_ready, 1, B_RELATIVE_TIMEOUT, 5000000);
        delete_sem(app_ready);
    }
    if (be_app == NULL) ok = 0;
    pthread_mutex_unlock(&app_lock);
    return ok;
}

static void play_buffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &)
{
    hs_player *p = (hs_player *)cookie;
    p->fill(p->cookie, buffer, size);
}

extern "C" {

/* sample formats: 0 unsigned 8 bit, 1 signed 16 bit, 2 signed 32 bit, 3 32 bit float */
void *haiku_audio_open(int rate, int channels, int format, int frames_per_buffer, hs_fill_func fill, void *cookie)
{
    media_raw_audio_format fmt = media_raw_audio_format::wildcard;
    size_t sample_size;
    hs_player *p;

    if (!ensure_application()) return NULL;

    switch (format)
    {
    case 0: fmt.format = media_raw_audio_format::B_AUDIO_UCHAR; sample_size = 1; break;
    case 1: fmt.format = media_raw_audio_format::B_AUDIO_SHORT; sample_size = 2; break;
    case 2: fmt.format = media_raw_audio_format::B_AUDIO_INT; sample_size = 4; break;
    case 3: fmt.format = media_raw_audio_format::B_AUDIO_FLOAT; sample_size = 4; break;
    default: return NULL;
    }
    fmt.frame_rate = rate;
    fmt.channel_count = channels;
    fmt.byte_order = B_MEDIA_HOST_ENDIAN;
    fmt.buffer_size = frames_per_buffer > 0 ? frames_per_buffer * channels * sample_size : 0;

    if (!(p = (hs_player *)calloc(1, sizeof(*p)))) return NULL;
    p->fill = fill;
    p->cookie = cookie;
    p->player = new BSoundPlayer(&fmt, "Wine", play_buffer, NULL, p);
    if (p->player == NULL || p->player->InitCheck() != B_OK)
    {
        delete p->player;
        free(p);
        return NULL;
    }
    return p;
}

int haiku_audio_start(void *handle)
{
    hs_player *p = (hs_player *)handle;

    if (p->player->Start() != B_OK) return 0;
    p->player->SetHasData(true);
    return 1;
}

void haiku_audio_stop(void *handle)
{
    hs_player *p = (hs_player *)handle;
    p->player->Stop();
}

void haiku_audio_close(void *handle)
{
    hs_player *p = (hs_player *)handle;
    p->player->Stop();
    delete p->player;
    free(p);
}

/* microseconds between what the program writes and what is heard */
long long haiku_audio_latency(void *handle)
{
    hs_player *p = (hs_player *)handle;
    return p->player->Latency();
}

void haiku_audio_set_volume(void *handle, float volume)
{
    hs_player *p = (hs_player *)handle;
    p->player->SetVolume(volume);
}

}
