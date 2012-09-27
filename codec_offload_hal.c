/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
* Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "codec_offload_hw"
#define LOG_NDEBUG 0

#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>
#include <linux/types.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/poll.h>
#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <utils/threads.h>
#include <intel_sst_ioctl.h>
#include <compress_params.h>
#include <tinycompress.h>

#define _POSIX_SOURCE
#include <alsa/asoundlib.h>

#define CODEC_OFFLOAD_BUFSIZE       (64*1024) /* Default buffer size in bytes */
#define CODEC_OFFLOAD_LATENCY       10      /* Default latency in mSec  */
#define CODEC_OFFLOAD_SAMPLINGRATE  48000     /* Default sampling rate in Hz */
#define CODEC_OFFLOAD_BITRATE       128000    /* Default bitrate in bps  */
#define CODEC_OFFLOAD_PCM_WORD_SIZE 16        /* Default PCM wordsize in bits */
#define CODEC_OFFLOAD_CHANNEL_COUNT 2         /* Default channel count */
#define OFFLOAD_TRANSFER_INTERVAL   8         /* Default intervel in sec */
#define OFFLOAD_MIN_ALLOWED_BUFSIZE (2*1024)   /*  bytes */
#define OFFLOAD_MAX_ALLOWED_BUFSIZE (128*1024) /*  bytes */

#define OFFLOAD_STREAM_DEFAULT_OUTPUT   2      /* Speaker */

#define AUDIO_DEVICE_NAME  "cloverviewaudio"  /* change this according to HW */
#define SST_VOLUME_MUTE 0xA0
#define SST_VOLUME_TYPE 0x602
#define SST_VOLUME_SIZE 1
#define SST_PPP_VOL_STR_ID  0x03
#define CODEC_OFFLOAD_INPUT_BUFFERSIZE 320

static snd_pcm_t *pHandle;

/* stream states */
typedef enum {
    STREAM_CLOSED    = 0, /* Device not opened yet  */
    STREAM_READY     = 1, /* Device opened, SET_STREAM_PARAMS done */
    STREAM_RUNNING   = 2, /* STREAM_START done and write calls going  */
    STREAM_PAUSING   = 3, /* STREAM_PAUSE to call and stream is pausing */
    STREAM_RESUMING  = 4  /* STREAM_RESUME to call and is OK for writes */
}sst_stream_states;

/* The data structure used for passing the codec specific information to the
* HAL offload hal for configuring the playback
*/
typedef struct{
        int32_t format;
        int32_t numChannels;
        int32_t sampleRate;
        int32_t bitsPerSample;
        int32_t avgBitRate;
    /* WMA -9 Specific */
        int32_t streamNumber;
        int32_t encryptedContentFlag;
        int32_t codecID;
        int32_t blockAlign;
        int32_t encodeOption;
    /* AAC Specific */
        int32_t downSampling;

}CodecInformation;

static CodecInformation mCodec;

struct offload_audio_device {
    struct audio_hw_device device;
    bool offload_init;
    uint32_t buffer_size;
    pthread_mutex_t lock;
    struct offload_stream_out *out;
    int  offload_out_ref_count;
};

struct offload_stream_out {
    audio_stream_out_t stream;
    sst_stream_states   state;
    struct compress     *compress;
    int                 fd;
    float               volume;
    bool                volume_change_requested;
    bool                muted;
    int32_t             format;
    uint32_t            sample_rate;
    uint32_t            buffer_size;
    uint32_t            channels;
    uint32_t            latency;
    uint32_t            adjusted_render_offset;
    uint32_t            paused_duration;
    struct snd_sst_params      params;
    int                 device_output;
    timer_t             paused_timer_id;
    pthread_mutex_t               lock;
};

/* The parameter structure used for getting and setting the volume
 */
struct offload_vol_algo_param {
    __u32 type;
    __u32 size;
    __u8  params;
}__attribute__((packed));


static bool is_offload_device_available(
               struct offload_audio_device *offload_dev,
               audio_format_t format, uint32_t channels, uint32_t sample_rate)
{
    if (!offload_dev->offload_init) {
        ALOGW("is_offload_device_available: Offload device not initialized");
        return false;
    }

    ALOGV("is_offload_device_available format = %x", format);

    switch (format){
        case AUDIO_FORMAT_MP3:
        //case AUDIO_FORMAT_WMA9:
        case AUDIO_FORMAT_AAC:
            break;
        default:
            ALOGW("is_offload_device_available: Offload not possible for"
                       "format = %x", format);
            return false;
    }

    return true;
}

static int out_pause(struct audio_stream *stream)
{
     ALOGV("out_pause");
     struct offload_stream_out *out = (struct offload_stream_out *)stream;
     struct audio_stream_out *aout = (struct audio_stream_out *)stream;
     if(out->state != STREAM_RUNNING){
        ALOGV("out_pause ignored: the state = %d", out->state);
        return 0;
     }
     ALOGV("out_pause: out->state = %d", out->state );
     int ret = out->stream.get_render_position(aout, &out->paused_duration);

     pthread_mutex_lock(&out->lock);
     if(compress_pause(out->compress) < 0 ) {
         ALOGE("out_pause : failed in the compress pause");
         pthread_mutex_unlock(&out->lock);
         return -ENOSYS;
     }
     out->state = STREAM_PAUSING;
     pthread_mutex_unlock(&out->lock);
     return 0;
}

static int out_resume( struct audio_stream *stream)
{
    ALOGV("out_resume");
    struct offload_stream_out *out = (struct offload_stream_out *)stream;
    ALOGV("out_resume: the state = %d", out->state);
    pthread_mutex_lock(&out->lock);
    if( compress_resume(out->compress) < 0) {
        ALOGE("failed in the compress resume");
        pthread_mutex_unlock(&out->lock);
        return -ENOSYS;
    }
    out->state = STREAM_RUNNING;
    pthread_mutex_unlock(&out->lock);
    return 0;
}

static int close_device(struct audio_stream_out *stream)
{
    struct offload_stream_out *out = (struct offload_stream_out *)stream;
    pthread_mutex_lock(&out->lock);
    ALOGV("close_device");
    if (out->compress) {
        ALOGV("close_device: compress_close");
        compress_close(out->compress);
    }
    if (out->fd) {
        close(out->fd);
        ALOGV("close_device: intel-sst- fd closed");
    }
    out->fd = 0;
    pthread_mutex_unlock(&out->lock);
    out->state = STREAM_CLOSED;
    return 0;
}

static int open_device(struct offload_stream_out *out)
{
    int card  = snd_card_get_index(AUDIO_DEVICE_NAME);
    int err;
    struct compr_config config;
    struct compress *compress;
    struct snd_codec codec;

    if (out->state != STREAM_CLOSED) {
        ALOGE("open[%d] Error with stream state", out->state);
        return -EINVAL;
    }
    char device_v[128];
    sprintf(device_v, "hw:%d,%d", card, 2);
    ALOGV("device_v = %s", device_v);
    if ((err = snd_pcm_open(&pHandle, device_v, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
       ALOGV("snd_pcm_open err(&pHandle, device_v, SND_PCM_STREAM_PLAYBACK, 0) %d", err);
    }
    if ((err = snd_pcm_set_params(pHandle,
                    SND_PCM_FORMAT_S16_LE,
                    SND_PCM_ACCESS_RW_INTERLEAVED,
                    CODEC_OFFLOAD_CHANNEL_COUNT,
                    CODEC_OFFLOAD_SAMPLINGRATE, 1, 500000)) < 0) {
        ALOGE("open_device: SND_PCM set_params failure setting %s\n", snd_strerror(err));
        close_device(&out->stream);
        return -EINVAL;
    }
    // update the configuration structure for given type of stream
    if (out->format == AUDIO_FORMAT_MP3) {
        codec.id = SND_AUDIOCODEC_MP3;
        /* the channel maks is the one that come to hal. Converting the mask to channel number */
        codec.ch_in = (out->channels == 3) ? 2: 1;
        codec.ch_out = (out->channels == 3) ? 2:1;
        codec.sample_rate =  out->sample_rate;
        codec.bit_rate = mCodec.avgBitRate ? mCodec.avgBitRate : CODEC_OFFLOAD_BITRATE;
        codec.rate_control = 0;
        codec.profile = 0;
        codec.level = 0;
        codec.ch_mode = 0;
        codec.format = 0;
        ALOGV("open_device: MP3 params: codec.id = %d,codec.ch_in = %d,codec.ch_out =%d,"
             "codec.sample_rate=%d, codec.bit_rate=%d,codec.rate_control=%d,"
             "codec.profile=%d,codec.level=%d,codec.ch_mode=%d,codec.format=%d",
             codec.id, codec.ch_in,codec.ch_out,codec.sample_rate,
             codec.bit_rate, codec.rate_control, codec.profile,
             codec.level,codec.ch_mode, codec.format);

    } else if (out->format == AUDIO_FORMAT_AAC) {

        /* AAC codec parameters  */
        codec.id = SND_AUDIOCODEC_AAC;
        /* Converting the mask to channel number */
        codec.ch_in = (out->channels == 3) ? 2: 1;
        codec.ch_out = (out->channels == 3) ? 2:1;
        codec.sample_rate =  out->sample_rate;
        codec.bit_rate = mCodec.avgBitRate ? mCodec.avgBitRate : CODEC_OFFLOAD_BITRATE;
        codec.rate_control = 0;
        codec.profile = SND_AUDIOPROFILE_AAC;
        codec.level = 0;
        codec.ch_mode = 0;
        codec.format = SND_AUDIOSTREAMFORMAT_RAW;
    }
    config.fragment_size = out->buffer_size;
    config.fragments = 2;
    config.codec = &codec;

    out->compress = compress_open(3, 0, COMPRESS_IN, &config);

    if (!out->compress || !is_compress_ready(out->compress)) {
        ALOGE("open_device: compress_open Error  %s\n",
                                  compress_get_error(out->compress));
        ALOGE("open_device:Unable to open Compress device %d:%d\n",
                                  card, out->device_output);
        return -EINVAL;
    }
    ALOGV("open_device: Compress device opened sucessfully");
    out->fd = open("/dev/intel_sst_ctrl", O_RDWR);
    if (out->fd < 0) {
        ALOGE("error opening LPE device, error = %d",out->fd);
        close_device(&out->stream);
        //pthread_mutex_unlock(&out->lock);
        return -EIO;
    }
    ALOGV("open_device: intel_sst_ctrl opened sucessuflly with fd=%d", out->fd);
    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    ALOGV("out_get_sample_rate:");
    struct offload_stream_out *out = (struct offload_stream_out *)stream;
    return out->sample_rate ?: CODEC_OFFLOAD_SAMPLINGRATE;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct offload_stream_out *out = (struct offload_stream_out *)stream;
    ALOGV("out_get_buffer_size: out->buffer_size %d", out->buffer_size);
    return out->buffer_size;
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    ALOGV("out_get_channels:");
    struct offload_stream_out *out = (struct offload_stream_out *)stream;
    return out->channels;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    struct offload_stream_out *out = (struct offload_stream_out *)stream;
    return out->format;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -1;
}

static int out_standby(struct audio_stream *stream)
{
    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct str_parms *param;

    ALOGV("out_set_parameters kvpairs = %s", kvpairs);

    struct offload_stream_out *out = (struct offload_stream_out*)stream;

    param = str_parms_create_str(kvpairs);
    int value=0;

    // Bits per sample - for WMA
    if (str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_BIT_PER_SAMPLE, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_BIT_PER_SAMPLE);
        mCodec.bitsPerSample = value;
    }
    // Avg bitrate in bps - for WMA/AAC/MP3
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_AVG_BIT_RATE, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_AVG_BIT_RATE);
        mCodec.avgBitRate = value;
        ALOGV("average bit rate set to %d", mCodec.avgBitRate);
    }
    // Number of channels present (for AAC)
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_NUM_CHANNEL, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_NUM_CHANNEL);
        mCodec.numChannels = value;
    }
    // Codec ID tag - Represents AudioObjectType (AAC) and FormatTag (WMA)
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_ID, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_ID);
        mCodec.codecID = value;
    }
    // Block Align - for WMA
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_BLOCK_ALIGN, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_BLOCK_ALIGN);
        mCodec.blockAlign = value;
    }
    // Sample rate - for WMA/AAC direct from parser
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_SAMPLE_RATE, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_SAMPLE_RATE);
        mCodec.sampleRate = value;
    }
    // Encode Option - for WMA
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_ENCODE_OPTION, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_ENCODE_OPTION);
        mCodec.encodeOption = value;
    }
    return 0;
}

static char* out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    const char *temp;
    char * value;
    struct str_parms *param;
    struct offload_stream_out *out = (struct offload_stream_out *)stream;
    param = str_parms_create_str(keys);
    if (str_parms_get_str(param, AUDIO_PARAMETER_STREAM_ROUTING, value,
                                strlen(AUDIO_PARAMETER_STREAM_ROUTING)) >= 0) {
        str_parms_add_int(param, AUDIO_PARAMETER_STREAM_ROUTING,
                                            out->device_output);
    }
    ALOGV("getParameters: %s", str_parms_to_str(param));
    temp = str_parms_to_str(param);
    return (char*)temp;
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    return CODEC_OFFLOAD_LATENCY;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    ALOGV("out_set_volume right vol= %f, left vol = %f", right, left);
    struct offload_stream_out *out = (struct offload_stream_out *)stream ;

    // Check the boundary conditions and apply volume to LPE
    if (left < 0.0f || left > 1.0f) {
        ALOGE("setVolume: Invalid data as vol=%f mOffloadDevice=%d", left, out->fd);
        return -EINVAL;
    }

    // Device could be in standby state. Once active, set new volume
    if (!out->fd){
        out->volume = left;
        out->volume_change_requested = true;
        ALOGV("setVolume: Requested for %2f, but yet to service when active", left);
        return 0;
    }

    pthread_mutex_lock(&out->lock);

    struct offload_vol_algo_param  sst_vol;

    ALOGV("setVolume Requested for %2f", left);
    if (left == 0) {
        /* Set the mute value for the FW i.e -96dB */
        sst_vol.params = SST_VOLUME_MUTE; //2s compliment of -96 dB
    } else {
        sst_vol.params =  (uint8_t)(20*log10(left));
    }
    sst_vol.type = SST_VOLUME_TYPE;
    sst_vol.size = SST_VOLUME_SIZE;


    out->volume  = left;
    out->volume_change_requested = false;
    ALOGV("setVolume:  volume=%x in 2s compliment", sst_vol.params);

   // Incase if device is already set with same volume, we can ignore this request
    struct offload_vol_algo_param  sst_get_vol;
    sst_get_vol.type = SST_VOLUME_TYPE; // 0x602;
    sst_get_vol.size = SST_VOLUME_SIZE; // 1;

    struct snd_ppp_params  sst_ppp_get_vol;
    sst_ppp_get_vol.algo_id = SST_CODEC_VOLUME_CONTROL; // 0x67
    sst_ppp_get_vol.str_id = SST_PPP_VOL_STR_ID; // 0x03; //out->params.stream_id;
    sst_ppp_get_vol.enable = 1;
    sst_ppp_get_vol.reserved = 1;
    sst_ppp_get_vol.size =  sizeof(struct offload_vol_algo_param);
    sst_ppp_get_vol.params = &sst_get_vol;
    ALOGV("calling get volume");
    if (ioctl(out->fd, SNDRV_SST_GET_ALGO, &sst_ppp_get_vol) >=0) {
        ALOGV("setVolume:  The volume read by getvolume stream_id =%d, volume = %d dB",
                            sst_ppp_get_vol.str_id, sst_get_vol.params);
        if (sst_get_vol.params == sst_vol.params) {
            pthread_mutex_unlock(&out->lock);
            ALOGV("setVolume: No update since volume requested matches to one in the system.");
            return 0;
        }
    }
    ALOGV("setVolume:  The volume read by getvolume stream_id =%d, volume = %d dB",
                            sst_ppp_get_vol.str_id, sst_get_vol.params);
    struct snd_ppp_params  sst_ppp_vol;
    sst_ppp_vol.algo_id = SST_CODEC_VOLUME_CONTROL;
    sst_ppp_vol.str_id = SST_PPP_VOL_STR_ID; // 0x03; //out->params.stream_id;
    sst_ppp_vol.enable = 1;
    sst_ppp_vol.reserved = 0;
    sst_ppp_vol.size =  sizeof(struct offload_vol_algo_param);
    sst_ppp_vol.params = &sst_vol;

    // Proceed doing the new setVolume to the LPE device
    int retval = ioctl(out->fd, SNDRV_SST_SET_ALGO, &sst_ppp_vol);
    if (retval <0) {
        ALOGE("setVolume: Error setting the ioctl with dB=%x", sst_vol.params);
                pthread_mutex_unlock(&out->lock);
        return retval;
    }
    ALOGV("setVolume: Successful in set volume=%2f (%x dB)", left, sst_vol.params);
    pthread_mutex_unlock(&out->lock);
    return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    struct offload_stream_out *out = (struct offload_stream_out *)stream;

    if (!bytes) {
        if (out->compress && out->state==STREAM_RUNNING)
            compress_drain(out->compress);
        return 0;
    }

    int sent = 0;
    int retval = 0;

    ALOGV("out_write: state = %d", out->state);
    switch (out->state) {
        case STREAM_CLOSED:
            // Due to standby the device could be closed (power-saving mode)
            if (open_device(out)) {
                ALOGE("out_write[%d]: Device open error, retry!", out->state);
                close_device(stream);
                return retval;
            }
        case STREAM_READY:
            if (out->volume_change_requested) {
                out->stream.set_volume(&out->stream,out->volume, out->volume);
            }
            ALOGV("out_write: state = %d: writting %d bytes", out->state, bytes);
            sent = compress_write(out->compress, buffer, bytes);
            if (sent < 0) {
                ALOGE("Error: %s\n", compress_get_error(out->compress));
            }
            ALOGV("out_write: state = %d: writting Done with %d bytes", out->state, sent);
            if (compress_start(out->compress) < 0) {
                ALOGE("write: Failed in the compress_start");
                return retval;
            }
            LOGI("out_write[%d]: writen  compress_start in state ", out->state);
            out->state = STREAM_RUNNING;
            break;
        case STREAM_RUNNING:
            if (out->volume_change_requested) {
                out->stream.set_volume(&out->stream,out->volume, out->volume);
            }
            ALOGV("out_write:[%d] Writing to compress write with %d bytes..", out->state, bytes);
            sent = compress_write(out->compress, buffer, bytes);
            if (sent < 0) {
                ALOGE("out_write:[%d] compress_write: Fatal error %s", out->state, strerror(errno));
                sent = 0;
            }
            LOGI("out_write:[%d] written %d bytes now", out->state, (int) sent);
            break;

        default:
            ALOGW("out_write[%d]: Ignored", out->state);
            return retval;
    }

    return sent;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    unsigned int avail;
    struct timespec tstamp;
    uint32_t calTimeMs;
    struct offload_stream_out *out = (struct offload_stream_out *)stream;

    *dsp_frames = out->adjusted_render_offset;
    if (!out->compress)  {
        ALOGV("out_get_render_position: Retuning without calling compress API");
        return 0;
    }

    pthread_mutex_lock(&out->lock);
    switch (out->state) {
        case STREAM_RUNNING:
        case STREAM_READY:
        case STREAM_PAUSING:
            if (compress_get_hpointer(out->compress, &avail,&tstamp) < 0) {
                ALOGW("out_get_render_position: compress_get_hposition Failed");
                pthread_mutex_unlock(&out->lock);
                return -EINVAL;
            }

          calTimeMs = (tstamp.tv_sec * 1000) + (tstamp.tv_nsec /1000000);
          *dsp_frames +=calTimeMs;
          ALOGV("out_get_render_position : time in millisec returned = %ld", *dsp_frames);
        break;
        default:
            pthread_mutex_unlock(&out->lock);
            return -EINVAL;
    }
    pthread_mutex_unlock(&out->lock);
    return 0;
}

/* The drain function implementation. This will send drain to driver */
static int out_drain(struct audio_stream_out *stream)
{
    ALOGV("out_drain");
    struct offload_stream_out *out = (struct offload_stream_out *)stream ;
    if (compress_drain(out->compress) < 0 ) {
        ALOGE("out_drain: Failed in the compress_drain ");
        return -ENOSYS;
    }
    return 0;
}

static int out_flush (struct audio_stream *stream)
{
   struct offload_stream_out *out = (struct offload_stream_out *)stream;
   switch (out->state) {
        case STREAM_RUNNING:
            break;
        case STREAM_PAUSING:
            out->adjusted_render_offset = 0;
            break;
        default :
            ALOGV("out_flush: ignored");
            out->adjusted_render_offset = 0;
            return 0;
    }
    ALOGV("out_flush:[%d] calling Compress Stop", out->state);
    if (compress_stop(out->compress) < 0) {
        ALOGE("out_flush: failed in the compress_flush");
        out->state = STREAM_READY;
        return -ENOSYS;
    }

    out->state = STREAM_READY;
    return 0;
}

static int offload_dev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    ALOGV("offload_dev_open_output_stream:  flag = 0x%x", flags);

    if (!(flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)) {
        ALOGV("offload_dev_open_output_stream: Not for Offload. Returning");
        return -ENOSYS;
    }
    struct offload_audio_device *loffload_dev = (struct offload_audio_device *)dev;
    struct offload_stream_out *out;
    int ret;
    if(loffload_dev->offload_out_ref_count == 1) {
        ALOGE("offload_dev_open_output_stream: Already device open");
        return -EINVAL;
    }

    out = (struct offload_stream_out *)calloc(1, sizeof(struct offload_stream_out));
    if (!out) {
        ALOGV("offload_dev_open_output_stream NO_MEMORY");
        return -ENOMEM;
    }

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.pause = out_pause;
    out->stream.resume = out_resume;
    out->stream.drain = out_drain;
    out->stream.flush = out_flush;

    *stream_out = &out->stream;

    // initialize stream parameters
    out->format = config->format;
    out->sample_rate = config->sample_rate;
    out->channels = config->channel_mask;
    out->buffer_size = loffload_dev->buffer_size ? loffload_dev->buffer_size : CODEC_OFFLOAD_BUFSIZE;

    //Default route is done for offload and let primary HAL do the routing
    out->device_output = OFFLOAD_STREAM_DEFAULT_OUTPUT;

    ret = open_device(out);
    if (ret != 0) {
        ALOGE("offload_dev_open_output_stream: open_device error");
        goto err_open;
    }

    loffload_dev->offload_out_ref_count += 1;
    loffload_dev->out = out;

    ALOGV("offload_dev_open_output_stream: offload device opened");
    out->state = STREAM_READY;
    return 0;

err_open:
    ALOGE("offload_dev_open_output_stream -> err_open:");
    free(out);
    *stream_out = NULL;
    return ret;
}

static void offload_dev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct offload_audio_device *loffload_dev = (struct offload_audio_device *)dev;
    ALOGV("offload_dev_close_output_stream");
    close_device(stream);
    loffload_dev->offload_out_ref_count = 0;
    free(stream);
}

static int offload_dev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct str_parms *param;

    ALOGV("offload_dev_set_parameters kvpairs = %s", kvpairs);

    param = str_parms_create_str(kvpairs);
    int value=0;

    // Avg bitrate in bps - for WMA/AAC/MP3
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_AVG_BIT_RATE, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_AVG_BIT_RATE);
        mCodec.avgBitRate = value;
        ALOGV("offload_dev_set_parameters: average bit rate %d", mCodec.avgBitRate);
    }
    // Sample rate - for WMA/AAC direct from parser
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_SAMPLE_RATE, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_SAMPLE_RATE);
        mCodec.sampleRate = value;
        ALOGV("offload_dev_set_parameters: sample rate %d", mCodec.sampleRate);
    }
    // Number of channels present (for AAC)
    if ( str_parms_get_int(param, AUDIO_OFFLOAD_CODEC_NUM_CHANNEL, &value) >= 0) {
        str_parms_del(param, AUDIO_OFFLOAD_CODEC_NUM_CHANNEL);
        mCodec.numChannels = value;
        ALOGV("offload_dev_set_parameters: num of channels %d", mCodec.numChannels);
    }
    return 0;
}

static char * offload_dev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return NULL;
}
// TBD - Do we need to open the compress device do init check ???
static int offload_dev_init_check(const struct audio_hw_device *dev)
{
    struct offload_audio_device *loffload_dev = (struct offload_audio_device *)dev;

    ALOGV("offload_dev_init_check");
    loffload_dev->offload_init = true;
    ALOGV("Proxy: initCheck successful");
    return 0;
}

static int offload_dev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int offload_dev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int offload_dev_set_mode(struct audio_hw_device *dev, int mode)
{
    return 0;
}

static size_t offload_dev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         uint32_t sample_rate, audio_format_t format,
                                         int channel_count)
{
    return CODEC_OFFLOAD_INPUT_BUFFERSIZE;
}

static size_t offload_dev_get_offload_buffer_size(const struct audio_hw_device *dev,
                                           uint32_t bitRate, uint32_t samplingRate,
                                           uint32_t channel)
{
    // Goal is to compute an optimal bufferSize that shall be used by
    // Multimedia framework in transferring the encoded stream to LPE firmware
    // in duration of OFFLOAD_TRANSFER_INTERVAL defined
    struct offload_audio_device *loffload_dev = (struct offload_audio_device *)dev;

    //set bit rate, sample rate and channel
    mCodec.avgBitRate = bitRate;
    mCodec.sampleRate = samplingRate;
    mCodec.numChannels = channel;

    size_t bufSize = 0;
    if (bitRate >= 12000) {
        bufSize = (OFFLOAD_TRANSFER_INTERVAL*bitRate)/8; /* in bytes */
    }
    else {
        // Though we could not take the decision based on exact bit-rate,
        // select optimal bufferSize based on samplingRate & Channel of the stream
        if (samplingRate<=8000)
            bufSize = 2*1024; // Voice data in Mono/Stereo
        else if (channel == AUDIO_CHANNEL_OUT_MONO)
            bufSize = 4*1024; // Mono music
        else if (samplingRate<=32000)
            bufSize = 16*1024; // Stereo low quality music
        else if (samplingRate<=48000)
            bufSize = 32*1024; // Stereo high quality music
        else
            bufSize = 64*1024; // HiFi stereo music
    }

    if (bufSize < OFFLOAD_MIN_ALLOWED_BUFSIZE)
        bufSize = OFFLOAD_MIN_ALLOWED_BUFSIZE;
    if (bufSize > OFFLOAD_MAX_ALLOWED_BUFSIZE)
        bufSize = OFFLOAD_MAX_ALLOWED_BUFSIZE;

    // Make the bufferSize to be of 2^n bytes
    for (size_t i = 1; (bufSize & ~i) != 0; i<<=1)
         bufSize &= ~i;

    loffload_dev->buffer_size = bufSize;

    ALOGV("getOffloadBufferSize: BR=%d SR=%d CC=%x bufSize=%d",
                             bitRate, samplingRate, channel, bufSize);
    return bufSize;

}

static int offload_dev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int offload_dev_close(hw_device_t *device)
{
    free(device);
    return 0;
}

static uint32_t offload_dev_get_supported_devices(const struct audio_hw_device *dev)
{
    return (/* OUT */
            AUDIO_DEVICE_OUT_SPEAKER |
            AUDIO_DEVICE_OUT_WIRED_HEADSET |
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
}

static int offload_dev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    ALOGV("offload_dev_open");
    struct offload_audio_device *offload_dev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        ALOGV("offload_dev_open: name != AUDIO_HARDWARE_INTERFACE");
        return -EINVAL;
    }

    offload_dev = (struct offload_audio_device *)calloc(1, sizeof(struct offload_audio_device));
    if (!offload_dev) {
        ALOGV("offload_dev_open: !offload_dev");
        return -ENOMEM;
    }

    offload_dev->device.common.tag = HARDWARE_DEVICE_TAG;
    offload_dev->device.common.version = AUDIO_DEVICE_API_VERSION_1_0;
    offload_dev->device.common.module = (struct hw_module_t *) module;
    offload_dev->device.common.close = offload_dev_close;

    offload_dev->device.get_supported_devices = offload_dev_get_supported_devices;
    offload_dev->device.init_check = offload_dev_init_check;
    offload_dev->device.set_voice_volume = offload_dev_set_voice_volume;
    offload_dev->device.set_master_volume = offload_dev_set_master_volume;
    offload_dev->device.set_mode = offload_dev_set_mode;
    offload_dev->device.set_parameters = offload_dev_set_parameters;
    offload_dev->device.get_parameters = offload_dev_get_parameters;
    offload_dev->device.get_input_buffer_size = offload_dev_get_input_buffer_size;
    offload_dev->device.get_offload_buffer_size = offload_dev_get_offload_buffer_size;
    offload_dev->device.open_output_stream = offload_dev_open_output_stream;
    offload_dev->device.close_output_stream = offload_dev_close_output_stream;
    offload_dev->device.dump = offload_dev_dump;

    *device = &offload_dev->device.common;
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    open:  offload_dev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    common : {
        tag : HARDWARE_MODULE_TAG,
        module_api_version: AUDIO_DEVICE_API_VERSION_1_0,
        hal_api_version: HARDWARE_HAL_API_VERSION,
        id : AUDIO_HARDWARE_MODULE_ID,
        name : "CODEC Offload HAL",
        author : "The Android Open Source Project",
        methods : &hal_module_methods,
        dso : NULL,
        reserved : {0},
    },
};
