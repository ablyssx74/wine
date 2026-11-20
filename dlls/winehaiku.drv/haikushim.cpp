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
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
#include <SoundPlayer.h>
#include <string.h>
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

/* ---- capture: the inputs of the sound hardware (Mic in, Line in ...) and BMediaRecorder ---- */

typedef void (*hs_record_func)(void *cookie, const void *data, size_t frames);

struct hs_recorder
{
    BMediaRecorder *recorder;
    hs_record_func record;
    void *cookie;
    int channels, format;    /* what the caller wants: format as in haiku_audio_open */
    int running;
    int old_input;           /* the input that was chosen before, given back when the recording is over */
    media_node node;         /* where the recording comes from: the system's default audio input (hrecord's input hub when it shares the input), or the hardware */
    media_node hardware;     /* the sound hardware: its input selector chooses Mic in, Line in ... */
    bool starts_hardware;    /* the node is the hardware itself, so this recorder starts it and stops it again */
};

/* the physical input node of the sound hardware (not a hub that shares it: hrecord's input hub is a physical input node too) */
static bool find_input_node(BMediaRoster *roster, media_node *node)
{
    live_node_info infos[32];
    int32 count = 32;
    bool found = false;

    if (roster->GetLiveNodes(infos, &count) != B_OK) return false;
    for (int32 i = 0; i < count; i++)
    {
        if (!(infos[i].node.kind & B_PHYSICAL_INPUT) || !(infos[i].node.kind & B_BUFFER_PRODUCER)) continue;
        if (!strncmp(infos[i].name, "hrecord ", 8)) continue;
        *node = infos[i].node;
        return true;
    }
    /* nothing else: whatever physical input there is */
    for (int32 i = 0; i < count && !found; i++)
        if ((infos[i].node.kind & B_PHYSICAL_INPUT) && (infos[i].node.kind & B_BUFFER_PRODUCER)) { *node = infos[i].node; found = true; }
    return found;
}

/* hrecord's mix hub (its "Offer the mix to programs that record" option): a node that hands out what is heard, the mix of everything that
 * plays, as an input of its own. It is offered as one more input, with this value (the hardware's own are small numbers). */
static const int kMixInputValue = 1000;
static const char *const kMixNodeName = "hrecord Mix";
static const char *const kMixInputName = "What you hear (hrecord mix)";

static bool find_mix_node(BMediaRoster *roster, media_node *node)
{
    live_node_info infos[32];
    int32 count = 32;

    bool found = false;

    if (roster->GetLiveNodes(infos, &count) != B_OK) return false;
    /* the newest one: a hub that was switched off can stay as a node as long as a program holds it, and a new one is made when it is
     * switched on again */
    for (int32 i = 0; i < count; i++)
    {
        if (!(infos[i].node.kind & B_BUFFER_PRODUCER) || strcmp(infos[i].name, kMixNodeName)) continue;
        if (!found || infos[i].node.node > node->node) *node = infos[i].node;
        found = true;
    }
    return found;
}

/* the parameters of the node that choose the input */
static int select_parameters(BMediaRoster *roster, const media_node &node, BParameterWeb **web, BDiscreteParameter **list, int max)
{
    int n = 0;

    *web = NULL;
    if (roster->GetParameterWebFor(node, web) != B_OK || !*web) return 0;
    for (int32 i = 0; i < (*web)->CountParameters() && n < max; i++)
    {
        BParameter *p = (*web)->ParameterAt(i);
        if (p->Type() == BParameter::B_DISCRETE_PARAMETER && p->Kind() && !strcmp(p->Kind(), B_INPUT_MUX))
            list[n++] = (BDiscreteParameter *)p;
    }
    return n;
}

/* choose an input (-1: keep what is chosen); returns the value that was chosen before, or -1 */
static int choose_input(BMediaRoster *roster, const media_node &node, int value)
{
    BParameterWeb *web;
    BDiscreteParameter *sel[4];
    int n = select_parameters(roster, node, &web, sel, 4), old = -1;

    for (int i = 0; i < n; i++)
    {
        int32 v;
        size_t size = sizeof(v);
        bigtime_t when;

        if (i == 0 && sel[i]->GetValue(&v, &size, &when) == B_OK && size >= sizeof(v)) old = v;
        if (value >= 0) { v = value; sel[i]->SetValue(&v, sizeof(v), 0); }
    }
    delete web;
    return old;
}

/* the inputs: names and the values that choose them; *current is the value the first selector has now (or -1) */
int haiku_audio_list_inputs(char names[][64], int *values, int max, int *current)
{
    BMediaRoster *roster;
    media_node node;
    BParameterWeb *web;
    BDiscreteParameter *sel[4];
    int n = 0;

    *current = -1;
    if (!ensure_application()) return 0;
    if (!(roster = BMediaRoster::Roster())) return 0;
    web = NULL;
    if (find_input_node(roster, &node) && select_parameters(roster, node, &web, sel, 4) > 0)
    {
        BDiscreteParameter *p = sel[0];
        int32 value;
        size_t size = sizeof(value);
        bigtime_t when;

        for (int32 i = 0; i < p->CountItems() && n < max; i++)
        {
            strncpy(names[n], p->ItemNameAt(i), 63);
            names[n][63] = 0;
            values[n++] = p->ItemValueAt(i);
        }
        if (p->GetValue(&value, &size, &when) == B_OK && size >= sizeof(value)) *current = value;
    }
    delete web;
    if (n < max && find_mix_node(roster, &node))
    {
        strncpy(names[n], kMixInputName, 63);
        names[n][63] = 0;
        values[n++] = kMixInputValue;
    }
    return n;
}

static float sample_float(const char *p, uint32 format)
{
    switch (format)
    {
    case media_raw_audio_format::B_AUDIO_FLOAT: return *(const float *)p;
    case media_raw_audio_format::B_AUDIO_INT: return *(const int32 *)p / 2147483648.0f;
    case media_raw_audio_format::B_AUDIO_SHORT: return *(const int16 *)p / 32768.0f;
    case media_raw_audio_format::B_AUDIO_UCHAR: return (*(const uint8 *)p - 128) / 128.0f;
    case media_raw_audio_format::B_AUDIO_CHAR: return *(const int8 *)p / 128.0f;
    }
    return 0;
}

static void record_buffer(void *cookie, bigtime_t, void *data, size_t size, const media_format &format)
{
    hs_recorder *r = (hs_recorder *)cookie;
    const media_raw_audio_format &in = format.u.raw_audio;
    uint32 in_size = in.format & 0xf, in_channels = in.channel_count;
    static const size_t out_sizes[] = { 1, 2, 4, 4 };
    size_t out_size = out_sizes[r->format];
    char out[4096];
    size_t frames, done = 0;

    if (format.type != B_MEDIA_RAW_AUDIO || !in_size || !in_channels) return;
    frames = size / (in_size * in_channels);
    while (done < frames)
    {
        size_t chunk = frames - done;
        if (chunk > sizeof(out) / (out_size * r->channels)) chunk = sizeof(out) / (out_size * r->channels);
        for (size_t i = 0; i < chunk; i++)
        {
            const char *src = (const char *)data + (done + i) * in_size * in_channels;
            for (int c = 0; c < r->channels; c++)
            {
                float v;
                if (r->channels == 1 && in_channels > 1)    /* mono wanted: the mean of the channels */
                {
                    v = 0;
                    for (uint32 k = 0; k < in_channels; k++) v += sample_float(src + k * in_size, in.format);
                    v /= in_channels;
                }
                else v = sample_float(src + (c < (int)in_channels ? c : in_channels - 1) * in_size, in.format);
                if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                char *dst = out + (i * r->channels + c) * out_size;
                switch (r->format)
                {
                case 0: *(uint8 *)dst = (uint8)(128 + v * 127.0f); break;
                case 1: *(int16 *)dst = (int16)(v * 32767.0f); break;
                case 2: *(int32 *)dst = (int32)(v * 2147483647.0); break;
                default: *(float *)dst = v; break;
                }
            }
        }
        r->record(r->cookie, out, chunk);
        done += chunk;
    }
}

/* starts to record the input with the given value; *rate is the frame rate of the hardware (0 if it is not known) */
void *haiku_audio_record_open(int channels, int format, int input_value, hs_record_func record, void *cookie, int *rate)
{
    BMediaRoster *roster;
    hs_recorder *r;
    media_format fmt;
    media_output outputs[8];
    int32 output_count = 8;

    if (!ensure_application() || format < 0 || format > 3) return NULL;
    if (!(roster = BMediaRoster::Roster())) return NULL;
    if (!(r = (hs_recorder *)calloc(1, sizeof(*r)))) return NULL;
    r->record = record;
    r->cookie = cookie;
    r->channels = channels;
    r->format = format;
    r->old_input = -1;

    if (input_value == kMixInputValue)
    {
        /* what is heard: hrecord's mix hub, not the hardware (its input selector stays as it is; the hub is started by hrecord) */
        media_node mix;

        if (!find_mix_node(roster, &mix) || roster->GetNodeFor(mix.node, &r->node) != B_OK) { free(r); return NULL; }
        r->hardware = r->node;
        r->starts_hardware = false;
    }
    else
    {
        if (!find_input_node(roster, &r->hardware)) { free(r); return NULL; }

        /* the system's default audio input is where programs get their input from: with hrecord's input hub on it is the hub, which has
         * a free output for every program (the hardware has one only, and hrecord holds it). Without a usable default, the hardware. */
        {
            media_node def;
            live_node_info info;

            if (roster->GetAudioInput(&def) == B_OK && roster->GetLiveNodeInfo(def, &info) == B_OK)
                r->node = def;
            else if (roster->GetNodeFor(r->hardware.node, &r->node) != B_OK)
            {
                free(r);
                return NULL;
            }
        }
        r->starts_hardware = r->node.node == r->hardware.node;

        r->old_input = choose_input(roster, r->hardware, input_value);
    }

    fmt.type = B_MEDIA_RAW_AUDIO;
    fmt.u.raw_audio = media_raw_audio_format::wildcard;
    r->recorder = new BMediaRecorder("Wine", B_MEDIA_RAW_AUDIO);
    if (r->recorder->InitCheck() != B_OK) goto fail;
    r->recorder->SetHooks(record_buffer, NULL, r);
    /* an output that is free; if none is, the first one (the connection then fails: the input is in use) */
    if (roster->GetFreeOutputsFor(r->node, outputs, 8, &output_count, B_MEDIA_RAW_AUDIO) != B_OK || output_count < 1)
    {
        output_count = 8;
        if (roster->GetAllOutputsFor(r->node, outputs, 8, &output_count) != B_OK || output_count < 1) goto fail;
    }
    if (r->recorder->Connect(r->node, &outputs[0], &fmt) != B_OK) goto fail;
    *rate = (int)r->recorder->Format().u.raw_audio.frame_rate;
    return r;

fail:
    delete r->recorder;
    if (r->old_input >= 0) choose_input(roster, r->hardware, r->old_input);
    roster->ReleaseNode(r->node);
    free(r);
    return NULL;
}

int haiku_audio_record_start(void *handle)
{
    hs_recorder *r = (hs_recorder *)handle;
    if (r->recorder->Start() != B_OK) return 0;
    /* the hardware does not send anything until its own node is started (a hub is started by whoever made it) */
    if (r->starts_hardware) BMediaRoster::Roster()->StartNode(r->node, 0);
    r->running = 1;
    return 1;
}

void haiku_audio_record_stop(void *handle)
{
    hs_recorder *r = (hs_recorder *)handle;
    if (r->running)
    {
        if (r->starts_hardware) BMediaRoster::Roster()->StopNode(r->node, 0, true);
        r->recorder->Stop();
    }
    r->running = 0;
}

void haiku_audio_record_close(void *handle)
{
    hs_recorder *r = (hs_recorder *)handle;
    haiku_audio_record_stop(handle);
    r->recorder->Disconnect();
    delete r->recorder;
    if (r->old_input >= 0) choose_input(BMediaRoster::Roster(), r->hardware, r->old_input);
    BMediaRoster::Roster()->ReleaseNode(r->node);
    free(r);
}

}
