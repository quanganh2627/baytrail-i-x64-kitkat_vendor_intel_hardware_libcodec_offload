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

#define LOG_TAG "dsp_FX"
//#define LOG_NDEBUG 0
#include "EffectEQ.h"
extern "C" const struct effect_interface_s gDSPEffectInterface;
namespace android {
const effect_descriptor_t gEqualizerDescriptor = {
    {0x0bed4300, 0xddd6, 0x11db, 0x8f34, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}, // type
    {0xf7a247c1, 0x1a7b, 0x11e0, 0xbb0d, {0x2a, 0x30, 0xdf, 0xd7, 0x20, 0x45}}, // uuid
    EFFECT_CONTROL_API_VERSION,
    (EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST |
    EFFECT_FLAG_VOLUME_CTRL | EFFECT_FLAG_HW_ACC_TUNNEL),
    0,
    1,
    "Equalizer",
    "Intel",
};
int32_t DSPinitLib(DSPEffect *pContext);
int32_t DSPinitEffect(DSPEffect *pContext);
int32_t DSPEQSetConfig(DSPEffect *pContext, effect_config_t *pConfig);
void DSPEQGetConfig(DSPEffect *pContext, effect_config_t *pConfig);
int32_t Effect_setState(DSPEffect *pContext, bool);
int32_t DSP_setParam(struct effect*, dsp_param, int16_t, const void*);
int32_t Equalizer_getParameter(DSPEffect     *pContext,
                           void              *pParam,
                           size_t            *pValueSize,
                           void              *pValue);
int32_t EqualizerGetBandFreqRange(DSPEffect *pContext, int32_t band,
                                  uint32_t *pmin,
                                  uint32_t *pmax);
int32_t GetNumPresets();
const char * EqualizerGetPresetName(int32_t preset);
int32_t EqualizerGetBand(DSPEffect *pContext, uint32_t targetFreq);
int32_t Equalizer_setParameter (DSPEffect *pContext, void *pParam, void *pValue);
int32_t EqualizerSetPreset (DSPEffect*, int preset);
int32_t EqualizerSetBandLevel(DSPEffect*, int band, short Gain);

extern "C" int32_t DSPEQCreate(const effect_uuid_t *uuid, int32_t sessionId,
                           int32_t ioId, effect_handle_t *pInterface) {
    int ret;
    size_t i, j;
    if ((pInterface == NULL) || (uuid == NULL)) {
        ALOGV("DSPEQCreate NULL UUID/ITFE");
        return -EINVAL;
    }
    DSPEffect* pContext = new DSPEffect;
    if (pContext == NULL) {
        ALOGW("DSPEQCreate() failed");
        return -EINVAL;
    }
    if (DSPinitLib(pContext)) {
        ALOGV("DSPEQCreate() no DSP lib context");
        delete pContext;
        pContext = NULL;
        return -EINVAL;
    }
    pContext->uuid = *uuid;
    // Do not create DSP effect here as the stream maybe unavailable
    pContext->pEffect = NULL;
    pContext->pItf = &gDSPEffectInterface;
    pContext->state = FALSE;
    pContext->ioId = ioId;
    for (i = 0; i < NUM_BANDS; i++) {
        pContext->bandFreq[i] = bandCFrequencies[i];
        pContext->bandGain[i] = 0;
    }
    pContext->preset = 3;
    *pInterface = (effect_handle_t)pContext;
    return 0;
}

extern "C" int32_t DSPEQRelease(effect_handle_t interface) {
    DSPEffect* pContext = (DSPEffect*)interface;
    ALOGV("DSPEQRelease() start");
    if (pContext == NULL) {
        ALOGV("DSPEQRelease() called with NULL pointer");
        return -EINVAL;
    }
    if ((pContext->pEffect) && effect_destroy(pContext->pEffect)) {
        ALOGV("DSPEQRelease() DSP effect destruction failed");
        return -EINVAL;
    }
    pContext->pEffect = NULL;
    if (pContext->pLibeffect) {
        libeffect_destroy(pContext->pLibeffect);
        pContext->pLibeffect = NULL;
    }
    delete pContext;
    pContext = NULL;
    ALOGV("DSPEQRelease() end");
    return 0;
}

extern "C" int32_t DSPEQGetDescriptor(const effect_uuid_t *uuid,
                                  effect_descriptor_t *pDescriptor) {
    const effect_descriptor_t *desc = NULL;
    ALOGV("DSPEQGetDescriptor() start");
    if (pDescriptor == NULL || uuid == NULL) {
        ALOGV("DSPEQGetDescriptor() called with NULL pointer");
        return -EINVAL;
    }

    if (memcmp(uuid, &gEqualizerDescriptor.uuid, sizeof(effect_uuid_t)) == 0) {
        desc = &gEqualizerDescriptor;
    }

    if (desc == NULL) {
        return  -EINVAL;
    }

    *pDescriptor = *desc;

    ALOGV("DSPEQGetDescriptor() end");
    return 0;
}
// Local functions

int32_t DSPinitLib(DSPEffect *pContext) {
    struct effect_config config;
    config.card_num = -1;
    config.log_level = 0;
    char value[PROPERTY_VALUE_MAX];
    // set the audio.device.name property in the init.<boardname>.rc file
    // or set the property at runtime in adb shell using setprop
    property_get("audio.device.name", value, "0");
    config.card_num = snd_card_get_index(value);
    if (config.card_num < 0) {
        ALOGE("open_device: Invalid card name %s. Set the card name against"
              "audio.device.name property in init.<boardname>.rc file", value);
        return -EINVAL;
    }
    pContext->pLibeffect = libeffect_init(&config);
    if (!pContext->pLibeffect) {
        ALOGV("DSPEQCreate() no DSP lib context");
        return -EINVAL;
    }
    if (!is_libeffect_ready(pContext->pLibeffect)) {
        ALOGV("DSPEQCreate() DSP lib not ready");
        return -EINVAL;
    }
    return 0;
}

int32_t DSPinitEffect(DSPEffect *pContext) {

    char value[PROPERTY_VALUE_MAX];
    property_get("offload.compress.device", value, "0");
    int stream = atoi(value);
    int device = 0; /* 00 for local effect, FF for global */
    int pos = 0;   /* EFFECT_INSERT_ANY */
    pContext->pEffect = effect_create(pContext->pLibeffect, pContext->uuid,
                                      pos, stream, device);
    if (pContext->pEffect == NULL) {
        ALOGV("DSPEQCreate() DSP effect creation failed");
        return -EINVAL;
    }
    // Send the BAnd gains to the newly created DSP effect
    if (pContext->pEffect) {
        ALOGV("Sending the params to DSP effect as it is created recently");
        return DSP_setParam(pContext->pEffect, EQ_BANDGAINS,
                        sizeof(pContext->bandGain), pContext->bandGain);
    }
    return 0;
}

// taken care by below layer components as the decoding happens there
int32_t DSPEQSetConfig(DSPEffect *pContext, effect_config_t *pConfig) {
    ALOGV("DSPEQSetConfig noop");
    return 0;
}

void DSPEQGetConfig(DSPEffect *pContext, effect_config_t *pConfig) {
    //memcpy(pConfig, &pContext->config, sizeof(effect_config_t));
    ALOGV("DSPEQGetConfig noop");
}

int32_t Equalizer_getParameter(DSPEffect     *pContext,
                           void              *pParam,
                           size_t            *pValueSize,
                           void              *pValue){
    int status = 0;
    int bMute = 0;
    int32_t *pParamTemp = (int32_t *)pParam;
    int32_t param = *pParamTemp++;
    int32_t param2;
    char *name;

    ALOGV("Equalizer_getParameter start: param: %d", param);

    switch (param) {
    case EQ_PARAM_NUM_BANDS:
    case EQ_PARAM_CUR_PRESET:
    case EQ_PARAM_GET_NUM_OF_PRESETS:
    case EQ_PARAM_BAND_LEVEL:
    case EQ_PARAM_GET_BAND:
        if (*pValueSize < sizeof(int16_t)) {
            ALOGV("Equalizer_getParameter() invalid pValueSize %d", *pValueSize);
            return -EINVAL;
        }
        *pValueSize = sizeof(int16_t);
        break;

    case EQ_PARAM_LEVEL_RANGE:
        if (*pValueSize < 2 * sizeof(int16_t)) {
            ALOGV("Equalizer_getParameter() invalid pValueSize %d", *pValueSize);
            return -EINVAL;
        }
        *pValueSize = 2 * sizeof(int16_t);
        break;
    case EQ_PARAM_BAND_FREQ_RANGE:
        if (*pValueSize < 2 * sizeof(int32_t)) {
            ALOGV("Equalizer_getParameter() invalid pValueSize %d", *pValueSize);
            return -EINVAL;
        }
        *pValueSize = 2 * sizeof(int32_t);
        break;

    case EQ_PARAM_CENTER_FREQ:
        if (*pValueSize < sizeof(int32_t)) {
            ALOGV("Equalizer_getParameter() invalid pValueSize %d", *pValueSize);
            return -EINVAL;
        }
        *pValueSize = sizeof(int32_t);
        break;

    case EQ_PARAM_GET_PRESET_NAME:
        break;

    case EQ_PARAM_PROPERTIES:
        if (*pValueSize < (2 + NUM_BANDS) * sizeof(uint16_t)) {
            ALOGV("Equalizer_getParameter() invalid pValueSize %d", *pValueSize);
            return -EINVAL;
        }
        *pValueSize = (2 + NUM_BANDS) * sizeof(uint16_t);
        break;

    default:
        ALOGV("Equalizer_getParameter unknown param %d", param);
        return -EINVAL;
    }

    switch (param) {
    case EQ_PARAM_NUM_BANDS:
        *(uint16_t *)pValue = (uint16_t)NUM_BANDS;
        break;

    case EQ_PARAM_LEVEL_RANGE:
        *(int16_t *)pValue = -1500;
        *((int16_t *)pValue + 1) = 1500;
        break;

    case EQ_PARAM_BAND_LEVEL:
        param2 = *pParamTemp;
        if (param2 >= NUM_BANDS || param2 < 0) {
            status = -EINVAL;
            break;
        }
        *(int16_t *)pValue = (int16_t)pContext->bandGain[param2] * 100;
        break;

    case EQ_PARAM_CENTER_FREQ:
        param2 = *pParamTemp;
        if ((param2 >= NUM_BANDS) || (param2 < 0)) {
            status = -EINVAL;
            break;
        }
        *(int32_t *)pValue = pContext->bandFreq[param2];
        break;

    case EQ_PARAM_BAND_FREQ_RANGE:
        param2 = *pParamTemp;
        if (param2 >= NUM_BANDS) {
            status = -EINVAL;
            break;
        }
        EqualizerGetBandFreqRange(pContext, param2, (uint32_t *)pValue,
                                  ((uint32_t *)pValue + 1));
        break;

    case EQ_PARAM_GET_BAND:
        param2 = *pParamTemp;
        *(uint16_t *)pValue = (uint16_t)EqualizerGetBand(pContext, param2);
        break;

    case EQ_PARAM_CUR_PRESET:
        *(uint16_t *)pValue = (uint16_t)pContext->preset;
        break;

    case EQ_PARAM_GET_NUM_OF_PRESETS:
        *(uint16_t *)pValue = (uint16_t)GetNumPresets();
        break;

    case EQ_PARAM_GET_PRESET_NAME:
        param2 = *pParamTemp;
        if (param2 >= GetNumPresets()) {
            status = -EINVAL;
            break;
        }
        name = (char *)pValue;
        strncpy(name, EqualizerGetPresetName(param2), *pValueSize - 1);
        name[*pValueSize - 1] = 0;
        *pValueSize = strlen(name) + 1;
        ALOGV("Equalizer_getParameter() EQ_PARAM_GET_PRESET_NAME preset %d"
              " name %s len %d", param2, gPresets[param2], *pValueSize);
        break;

    case EQ_PARAM_PROPERTIES: {
        int16_t *p = (int16_t *)pValue;
        ALOGV("Equalizer_getParameter() EQ_PARAM_PROPERTIES");
        p[0] = (int16_t)pContext->preset;
        p[1] = (int16_t)NUM_BANDS;
        for (int i = 0; i < NUM_BANDS; i++) {
            p[2 + i] = (int16_t)pContext->bandGain[i] * 100;
        }
    } break;

    default:
        ALOGV("ERROR : Equalizer_getParameter() invalid param %d", param);
        status = -EINVAL;
        break;
    }

    ALOGV("Equalizer_getParameter end\n");
    return status;
} /* end Equalizer_getParameter */

int32_t GetNumPresets() {
    return (sizeof(gPresets) / sizeof(char*));
}

int32_t EqualizerGetBandFreqRange(DSPEffect *pContext, int32_t band,
                                  uint32_t *pmin, uint32_t *pmax) {
    *pmin = bandFreqRange[band][0];
    *pmax  = bandFreqRange[band][1];
    return 0;
}

const char* EqualizerGetPresetName(int32_t preset) {

    ALOGV("EqualizerGetPresetName %d, %s", preset, gPresets[preset]);
    return gPresets[preset];
}

int32_t EqualizerGetBand(DSPEffect *pContext, uint32_t targetFreq) {

    int band = 0;
    if (targetFreq < bandFreqRange[0][0]) {
        return -EINVAL;
    } else if (targetFreq == bandFreqRange[0][0]) {
        return 0;
    }
    for (int i = 0; i < NUM_BANDS; i++) {
        if ((targetFreq > bandFreqRange[i][0]) &&
            (targetFreq <= bandFreqRange[i][1])) {
            band = i;
        }
    }
    return band;
}

int32_t Equalizer_setParameter (DSPEffect *pContext, void *pParam, void *pValue) {
    int status = 0;
    int32_t preset;
    int32_t band;
    int32_t level;
    int32_t *pParamTemp = (int32_t *)pParam;
    int32_t param = *pParamTemp++;

    ALOGV("Equalizer_setParameter start: param: %d",param);
    switch (param) {
    case EQ_PARAM_CUR_PRESET:
    {
        preset = (int32_t)(*(uint16_t *)pValue);
        if ((preset >= GetNumPresets()) || (preset < 0)) {
            status = -EINVAL;
            break;
        }
        status = EqualizerSetPreset(pContext, preset);
        break;
    }
    case EQ_PARAM_BAND_LEVEL:
    {
        band =  *pParamTemp;
        level = (int32_t)(*(int16_t *)pValue);
        if (band >= NUM_BANDS) {
            status = -EINVAL;
            break;
        }
        status = EqualizerSetBandLevel(pContext, band, level);
        break;
    }
    case EQ_PARAM_PROPERTIES:
    {
        int16_t *p = (int16_t *)pValue;
        if ((int)p[0] >= GetNumPresets()) {
            status = -EINVAL;
            break;
        }
        if (p[0] >= 0) {
            status = EqualizerSetPreset(pContext, (int)p[0]);
        } else {
            if ((int)p[1] != NUM_BANDS) {
                status = -EINVAL;
                break;
            }
            for (int i = 0; i < NUM_BANDS; i++) {
                status = EqualizerSetBandLevel(pContext, i, (int)p[2 + i]);
            }
        }
        break;
    }
    default:
        ALOGV("Equalizer_setParameter() invalid param %d", param);
        status = -EINVAL;
        break;
    }
    // if the status is error, return without calling DSP setparam
    if (status)
        return status;
    // if the DSP effect handle is available, return the status of DSP set param
    if (pContext->pEffect)
        return DSP_setParam(pContext->pEffect, EQ_BANDGAINS,
                        sizeof(pContext->bandGain), pContext->bandGain);
    // if the DSP handle is NULL, return 0.
    return 0;
}

int32_t EqualizerSetPreset(DSPEffect* pContext, int preset) {

    ALOGV("EqualizerSetPreset: preset: %d", preset);
    if (pContext == NULL) {
        ALOGV("EqualizerSetPreset NULL pointer");
        return -EINVAL;
    }
    int32_t gain, gainRounded;
    for (int i = 0; i< NUM_BANDS; i++) {
        pContext->bandGain[i] = bandGains[i + preset * NUM_BANDS];
    }
    pContext->preset = preset;
    return 0;
}

int32_t EqualizerSetBandLevel(DSPEffect* pContext, int band, short gain) {

    ALOGV("EqualizerSetBandLevel: band: %d, gain: %d", band, gain);
    if (pContext == NULL) {
        ALOGV("EqualizerSetBandLevel NULL pointer");
        return -EINVAL;
    }

    int32_t gainRounded;
    if (gain > 0)
       gainRounded = (gain + 50) / 100;
    else
       gainRounded = (gain - 50) / 100;
    pContext->bandGain[band] = gainRounded;
    pContext->preset = PRESET_CUSTOM;
    return 0;
}

int32_t Effect_setState(DSPEffect *pContext, bool state) {

     if (pContext == NULL) {
         ALOGV("Effect_setState NULL pointer");
         return -EINVAL;
     }
     pContext->state = state;
     if (pContext->pEffect)
         return DSP_setParam(pContext->pEffect, EQ_ENABLE,
                             sizeof(state), &state);
     return 0;
}

int32_t int16ToBytes(int16_t value, unsigned char *buf) {

    *buf = (unsigned char)(value & 0xff);
    *(buf + 1) = (unsigned char)((value >> 8) & 0xff);
    return (sizeof(int16_t));
}

int32_t int32ToBytes(int32_t value, unsigned char *buf) {

    for (int i = 0; i < 4; i++) {
        *(buf + i) = (unsigned char)((value >> (i * 8)) & 0xff);
        ALOGV("byte %d: %d", i, *(buf));
    }
    return (sizeof(int32_t));
}

int32_t DSP_setParam(struct effect* DSPhandle, dsp_param param, int16_t size,
                 const void* value) {

    if (DSPhandle == NULL) {
        ALOGV("DSP_setParam() NULL DSP pointer");
        return -EINVAL;
    }
    unsigned char* pParam = NULL;
    int index = 0;
    int param_size = sizeof(dsp_param) + sizeof(size) + size;
    switch(param) {
        case EQ_ENABLE:
        {
            if (size < sizeof(bool)) {
                ALOGV("DSP_setParam() insufficient data");
                return -EINVAL;
            }
            uint32_t enable = *(uint8_t*)value;
            pParam = (unsigned char*)malloc(sizeof(dsp_param) +
                                            sizeof(size) + size);
            index+= int16ToBytes(param, pParam); // type
            ALOGV("DSP_setParam type %d %d index %d",
                   *pParam, *(pParam + 1),index);
            index+= int16ToBytes(size, pParam+index);  // length
            ALOGV("DSP_setParam length %d %d index %d",
                   *(pParam + 2), *(pParam + 3),index);
            index = int32ToBytes(enable, pParam + index);
            *(pParam + index) = enable; // value
        break;
        }
        case EQ_BANDGAINS:
        {
            if (size < (sizeof(int32_t) * NUM_BANDS)) {
                ALOGV("DSP_setParam() insufficient data");
                return -EINVAL;
            }
            pParam = (unsigned char*)malloc(sizeof(dsp_param) +
                      sizeof(size) + size);
            index+= int16ToBytes(param, pParam); // type
            ALOGV("DSP_setParam type %d %d index %d",
                   *pParam, *(pParam + 1),index);
            index+= int16ToBytes(size, pParam+index);  // length
            ALOGV("DSP_setParam length %d %d index %d",
                   *(pParam + 2), *(pParam + 3),index);
            for(int i = 0; i < NUM_BANDS; i++) {  // form the value
                index+= int32ToBytes(
                           ((int32_t*)value)[i], pParam + index);
            }
        break;
        }
        default:
            ALOGV("DSP_setParams() unknown param: %d", param);
            return -EINVAL;
        break;
    }
    return effect_set_params(DSPhandle, param_size, (char*)pParam);
}
//Local functions end
} //namespace android
extern "C" {
int32_t DSPEQ_command(effect_handle_t  self,
                              uint32_t            cmdCode,
                              uint32_t            cmdSize,
                              void                *pCmdData,
                              uint32_t            *replySize,
                              void                *pReplyData){
        DSPEffect * pContext = (DSPEffect *) self;
        if (pContext == NULL) {
            ALOGV("DSPEQ_command called with NULL pointer");
            return -EINVAL;
        }
        ALOGV("DSPEQ_command: cmdCode: %d", cmdCode);
        switch (cmdCode) {
        case EFFECT_CMD_INIT:
            if (pReplyData == NULL || *replySize != sizeof(int)){
                ALOGV("EFFECT_CMD_INIT: ERROR");
                return -EINVAL;
            }
            *(int *) pReplyData = 0;
            ALOGV("DSPEQ_command init, noop");
            return 0;
        case EFFECT_CMD_SET_CONFIG:
            if (pCmdData    == NULL||
                cmdSize     != sizeof(effect_config_t)||
                pReplyData  == NULL||
                *replySize  != sizeof(int)){
                ALOGV("DSPEQ_command cmdCode Case: "
                        "EFFECT_CMD_SET_CONFIG: ERROR");
                return -EINVAL;
            }
            *(int *) pReplyData = android::DSPEQSetConfig(pContext,
                                      (effect_config_t *) pCmdData);
            break;
        case EFFECT_CMD_GET_CONFIG:
            if (pReplyData == NULL ||
                *replySize != sizeof(effect_config_t)) {
                ALOGV("DSPEQ_command cmdCode Case: "
                        "EFFECT_CMD_GET_CONFIG: ERROR");
                return -EINVAL;
            }
            android::DSPEQGetConfig(pContext, (effect_config_t *)pReplyData);
            break;

        case EFFECT_CMD_RESET:
            android::DSPEQSetConfig(pContext, NULL);
            break;

        case EFFECT_CMD_GET_PARAM:
        {
            ALOGV("EFFECT_CMD_GET_PARAM");
            if (pCmdData == NULL ||
                    cmdSize < (int)(sizeof(effect_param_t) + sizeof(int32_t)) ||
                    pReplyData == NULL ||
                    *replySize < (int) (sizeof(effect_param_t) +
                                 sizeof(int32_t))) {
                    ALOGV("DSPEQ_command cmdCode Case: "
                            "EFFECT_CMD_GET_PARAM: ERROR");
                    return -EINVAL;
                }
                effect_param_t *p = (effect_param_t *)pCmdData;

                memcpy(pReplyData, pCmdData, sizeof(effect_param_t) + p->psize);

                p = (effect_param_t *)pReplyData;

                int voffset = ((p->psize - 1) /
                                  sizeof(int32_t) + 1) * sizeof(int32_t);

                p->status = android::Equalizer_getParameter(pContext,
                                                            p->data,
                                                            &p->vsize,
                                                            p->data + voffset);

                *replySize = sizeof(effect_param_t) + voffset + p->vsize;
            break;
        }
        case EFFECT_CMD_SET_PARAM:
        {
            int i;
            ALOGV("EFFECT_CMD_SET_PARAM");
            if (pCmdData == NULL || cmdSize <
                    (int)(sizeof(effect_param_t) + sizeof(int32_t)) ||
                pReplyData == NULL || *replySize != sizeof(int32_t)) {
                ALOGV("DSPEQ_command cmdCode Case: "
                        "EFFECT_CMD_SET_PARAM: ERROR");
                return -EINVAL;
            }
            effect_param_t *p = (effect_param_t *) pCmdData;
            *(int *)pReplyData = android::Equalizer_setParameter(pContext,
                                                         (void *)p->data,
                                                         p->data + p->psize);

            break;
        }
        case EFFECT_CMD_ENABLE:
        {
            ALOGV("DSPEQ_command cmdCode Case: EFFECT_CMD_ENABLE start");
            if (pReplyData == NULL || *replySize != sizeof(int)) {
                ALOGV("DSPEQ_command cmdCode Case: EFFECT_CMD_ENABLE: ERROR");
                return -EINVAL;
            }

            *(int *)pReplyData = android::Effect_setState(pContext, TRUE);

            break;
        }
        case EFFECT_CMD_DISABLE:
        {
            ALOGV("DSPEQ_command cmdCode Case: EFFECT_CMD_DISABLE start");
            if (pReplyData == NULL || *replySize != sizeof(int)){
                ALOGV("DSPEQ_command cmdCode Case: EFFECT_CMD_DISABLE: ERROR");
                return -EINVAL;
            }
            *(int *)pReplyData = android::Effect_setState(pContext, FALSE);
            break;
        }
        case EFFECT_CMD_OFFLOAD: {
            ALOGV("DSPEQ_command cmdCode Case: CMD_OFFLOAD");
            effect_offload_param_t* offloadParam =
                 (effect_offload_param_t*) pCmdData;
            // If the current thread is offload, we need to the create the
            // for that offload stream
            if (offloadParam->isOffload) {
                // init the library and create effect if not done
                if (pContext->pLibeffect == NULL) {
                    if (android::DSPinitLib(pContext)) {
                        ALOGV("DSPEQ_command: CMD_OFFLOAD error");
                        return -EINVAL;
                    }
                }
                if (pContext->pEffect == NULL) {
                    if (android::DSPinitEffect(pContext)) {
                        ALOGV("DSPEQ_command: CMD_OFFLOAD Error creating effect");
                        return -EINVAL;
                    }
                }
            } else { //current thread is IA. So destruct the DSP effect
                if ((pContext->pEffect) && effect_destroy(pContext->pEffect)) {
                    ALOGV("DSPEQ_command:CMD_OFFLOAD FX destruction failed");
                    return -EINVAL;
                }
                pContext->pEffect = NULL;
            }
            if (pReplyData)
                *(int *)pReplyData = 0;
        }
        break;
        case EFFECT_CMD_SET_DEVICE:
        case EFFECT_CMD_SET_VOLUME:
        case EFFECT_CMD_SET_AUDIO_MODE:
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

int32_t DSPEQ_process(effect_handle_t     self,
                              audio_buffer_t         *inBuffer,
                              audio_buffer_t         *outBuffer) {
    ALOGV("DSPEQ_process noop");
    return 0;
}

int32_t DSPEQ_getDescriptor(effect_handle_t   self,
                                   effect_descriptor_t *pDescriptor) {
    DSPEffect * pContext = (DSPEffect *) self;
    const effect_descriptor_t *desc;

    if (pContext == NULL || pDescriptor == NULL) {
        ALOGV("DSPEQ_getDescriptor() NULL pointer");
        return -EINVAL;
    }
    desc = &android::gEqualizerDescriptor;
    *pDescriptor = *desc;

    return 0;
}

const struct effect_interface_s gDSPEffectInterface = {
    DSPEQ_process,
    DSPEQ_command,
    DSPEQ_getDescriptor,
    NULL
};
__attribute__ ((visibility ("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    tag : AUDIO_EFFECT_LIBRARY_TAG,
    version : EFFECT_LIBRARY_API_VERSION,
    name : "Intel EQ Library",
    implementor : "Intel",
    create_effect : android::DSPEQCreate,
    release_effect : android::DSPEQRelease,
    get_descriptor : android::DSPEQGetDescriptor,
};
} //extern C

