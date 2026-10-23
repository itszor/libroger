/* Host audio: the sound system's converted DMA buffers (sound.c; 16-bit
 * stereo at the DMA rate, 1e6/period Hz) go to an SDL audio stream - pushed
 * from the emulation thread, not pulled: no callback, SDL resamples to the
 * device rate - and/or to a --wav file. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"

static SDL_AudioStream *stream;
static int stream_rate;
static FILE *wav;
static uint32_t wav_frames;
static int wav_rate;

int audio_init_device(void)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { SDL_Log("audio: %s", SDL_GetError()); return 0; }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 41666};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) { SDL_Log("audio: %s", SDL_GetError()); return 0; }
    stream_rate = spec.freq;
    SDL_ResumeAudioStreamDevice(stream);
    return 1;
}

static void wav_header(void)
{
    uint32_t data = wav_frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + data; memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16; *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = wav_rate; *(uint32_t *)(h + 28) = wav_rate * 4; *(uint16_t *)(h + 32) = 4;
    *(uint16_t *)(h + 34) = 16; memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = data;
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

int audio_init_wav(const char *path)
{
    wav = fopen(path, "wb");
    if (!wav) return 0;
    wav_rate = 41666;
    wav_header();
    return 1;
}

/* One emulated DMA buffer: `frames` stereo frames at `rate` Hz. */
void roger_audio_out(const int16_t *lr, int frames, int rate)
{
    if (wav) {
        /* A WAV file has one rate: the one sound is first heard at
         * (programs normally pick their rate once, though a module may
         * reconfigure after the default's silent buffers: QTM) */
        static int heard;
        if (!heard) {
            wav_rate = rate;
            for (int k = 0; k < 2 * frames && !heard; k++) heard = lr[k] != 0;
        }
        fwrite(lr, 4, frames, wav);
        wav_frames += frames;
    }
    if (stream) {
        if (rate != stream_rate) {
            SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, rate};
            SDL_SetAudioStreamFormat(stream, &spec, NULL);
            stream_rate = rate;
        }
        /* Fills are paced by the wall-clock centisecond tick, so the queue
         * should hover around a few buffers; if the device has fallen
         * behind (e.g. after a stall), drop rather than build up latency. */
        int queued = SDL_GetAudioStreamQueued(stream);
        if (queued == 0) {                       /* start, or ran dry: re-establish a cushion */
            static int16_t zero[2 * 4096];
            int pad = rate / 25;                 /* 40ms */
            SDL_PutAudioStreamData(stream, zero, (pad < 4096 ? pad : 4096) * 4);
        }
        if (queued < rate * 4 / 4)
            SDL_PutAudioStreamData(stream, lr, frames * 4);
    }
}

void audio_close(void)
{
    if (wav) { wav_header(); fclose(wav); wav = 0; }
    if (stream) { SDL_DestroyAudioStream(stream); stream = 0; }
}
