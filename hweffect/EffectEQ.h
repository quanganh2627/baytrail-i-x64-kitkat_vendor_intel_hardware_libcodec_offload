/*
 * Copyright (C) 2010 The Android Open Source Project
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


#include <cutils/log.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <utils/String8.h>
#include <media/AudioSystem.h>
#include <audio_effects/effect_equalizer.h>
#include <hardware/audio_effect.h>
#include <hardware/audio.h>
#include <cutils/properties.h>
#include "eq.h"

#define _POSIX_SOURCE
#include <alsa/asoundlib.h>
#if __cplusplus
extern "C" {
#endif
#include <libeffect/libeffect.h>
#define TRUE 1
#define FALSE 0

enum dsp_param {
    EQ_ENABLE = 1,
    EQ_BANDGAINS = 5,
    EQ_CFREQ
};
struct DSPEffect {
    const struct effect_interface_s* pItf; // Holds itfe of the effect lib
    bool                  state; // enabled state
    int32_t               preset; // current preset
    int32_t               bandGain[NUM_BANDS]; // band gains
    uint16_t              bandFreq[NUM_BANDS]; // centre freq of the bands
    struct libeffect*     pLibeffect; // pointer to libeffect lib
    struct effect*        pEffect; // pointer to the effect handle
    audio_io_handle_t     ioId; // the thread id where effect is created
    effect_uuid_t         uuid; // uuid of the effect
};
#if __cplusplus
} // extern C
#endif
