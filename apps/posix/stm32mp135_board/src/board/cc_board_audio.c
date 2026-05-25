#include "cc_board_tools_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CC_BOARD_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

static void write_wav_header(FILE *f, int sample_rate, int channels, int bits, uint32_t data_size)
{
    uint32_t byte_rate = (uint32_t)(sample_rate * channels * bits / 8);
    uint16_t block_align = (uint16_t)(channels * bits / 8);
    uint32_t riff_size = 36 + data_size;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

static int record_wav(
    const char *device,
    const char *path,
    int seconds,
    int sample_rate,
    int channels,
    size_t *out_bytes
)
{
#ifndef CC_BOARD_HAS_ALSA
    (void)device;
    (void)path;
    (void)seconds;
    (void)sample_rate;
    (void)channels;
    (void)out_bytes;
    return -2;
#else
    snd_pcm_t *pcm = NULL;
    int rc = snd_pcm_open(&pcm, device ? device : CC_BOARD_DEFAULT_AUDIO_DEVICE,
        SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) return -1;
    rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned int)channels,
        (unsigned int)sample_rate, 1, 500000);
    if (rc < 0) {
        snd_pcm_close(pcm);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        snd_pcm_close(pcm);
        return -1;
    }
    write_wav_header(f, sample_rate, channels, 16, 0);
    int frames_total = sample_rate * seconds;
    int frames_left = frames_total;
    int chunk_frames = 1024;
    int bytes_per_frame = channels * 2;
    unsigned char *buffer = malloc((size_t)chunk_frames * bytes_per_frame);
    if (!buffer) {
        fclose(f);
        snd_pcm_close(pcm);
        return -1;
    }
    size_t data_size = 0;
    while (frames_left > 0) {
        int want = frames_left < chunk_frames ? frames_left : chunk_frames;
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buffer, (snd_pcm_uframes_t)want);
        if (got == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (got < 0) break;
        size_t bytes = (size_t)got * bytes_per_frame;
        if (fwrite(buffer, 1, bytes, f) != bytes) break;
        data_size += bytes;
        frames_left -= (int)got;
    }
    free(buffer);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    fseek(f, 0, SEEK_SET);
    write_wav_header(f, sample_rate, channels, 16, (uint32_t)data_size);
    fclose(f);
    if (out_bytes) *out_bytes = 44 + data_size;
    return data_size > 0 ? 0 : -1;
#endif
}

static cc_result_t board_audio_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)self;
    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json && *args_json ? args_json : "{}", &args);
    if (rc.code != CC_OK) {
        cc_board_set_error(out_result, "Invalid audio arguments JSON");
        return cc_result_ok();
    }
    const char *device = cc_board_json_string_or(args, "device", CC_BOARD_DEFAULT_AUDIO_DEVICE);
    int seconds = cc_board_json_int_or(args, "seconds", 3);
    int sample_rate = cc_board_json_int_or(args, "sample_rate", 44100);
    int channels = cc_board_json_int_or(args, "channels", 2);
    int embed_base64 = cc_board_json_bool_or(args, "embed_base64", 1);
    if (seconds <= 0 || seconds > 60 || channels <= 0 || channels > 2 || sample_rate <= 0) {
        cc_json_destroy(args);
        cc_board_set_error(out_result, "Invalid audio parameters");
        return cc_result_ok();
    }
    char *path = cc_board_output_path(ctx, args, "output_path", "audio", "wav");
    if (!path) {
        cc_json_destroy(args);
        cc_board_set_error(out_result, "Failed to create board audio output path");
        return cc_result_ok();
    }
    size_t bytes = 0;
    int rec = record_wav(device, path, seconds, sample_rate, channels, &bytes);
    if (rec == -2) {
        free(path);
        cc_json_destroy(args);
        cc_board_set_error(out_result, "ALSA support was not found at build time");
        return cc_result_ok();
    }
    if (rec != 0) {
        free(path);
        cc_json_destroy(args);
        cc_board_set_error(out_result, "Failed to record WAV audio from ALSA device");
        return cc_result_ok();
    }

    char *b64 = cc_board_file_base64_if_requested(path, embed_base64, &bytes);
    char *id = cc_board_now_id("audio");
    char *artifacts = cc_board_artifact_json(id, "audio", "audio/wav", path, b64, bytes, 0, 0, seconds * 1000);

    cc_json_value_t *content = cc_json_create_object();
    cc_json_object_set(content, "ok", cc_json_create_bool(1));
    cc_json_object_set(content, "path", cc_json_create_string(path));
    cc_json_object_set(content, "mime", cc_json_create_string("audio/wav"));
    cc_json_object_set(content, "bytes", cc_json_create_number((double)bytes));
    cc_json_object_set(content, "duration_ms", cc_json_create_number(seconds * 1000));
    cc_json_object_set(content, "sample_rate", cc_json_create_number(sample_rate));
    cc_json_object_set(content, "channels", cc_json_create_number(channels));
    char *content_json = cc_json_stringify_unformatted(content);
    cc_json_destroy(content);
    cc_board_set_success_json(out_result, content_json);
    out_result->artifacts_json = artifacts;

    free(id);
    free(b64);
    free(path);
    cc_json_destroy(args);
    return cc_result_ok();
}

const cc_board_tool_ops_t cc_board_audio_tool_ops = {
    "board.audio",
    "Record WAV audio from the STM32MP135 ALSA capture device and return an audio artifact.",
    "{\"type\":\"object\",\"properties\":{\"device\":{\"type\":\"string\"},\"seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":60},\"sample_rate\":{\"type\":\"integer\"},\"channels\":{\"type\":\"integer\"},\"output_path\":{\"type\":\"string\"},\"embed_base64\":{\"type\":\"boolean\"}}}",
    board_audio_call,
    NULL
};
