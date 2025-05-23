/*
 * Copyright (C) 2024 The Android Open Source Project
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

/**
 * @addtogroup Media
 * @{
 */

/**
 * @file NdkMediaCodecStore.h
 */

/*
 * This file defines an NDK API.
 * Do not remove methods.
 * Do not change method signatures.
 * Do not change the value of constants.
 * Do not change the size of any of the classes defined in here.
 * Do not reference types that are not part of the NDK.
 * Do not #include files that aren't part of the NDK.
 */

#ifndef _NDK_MEDIA_CODEC_STORE_H
#define _NDK_MEDIA_CODEC_STORE_H

#include <stdint.h>

#include "NdkMediaCodecInfo.h"
#include "NdkMediaError.h"
#include "NdkMediaFormat.h"

__BEGIN_DECLS

/**
 * The media type definition with bitfeids indicating if it is
 * supported by decoders/ encoders/ both.
 */
typedef struct AMediaCodecSupportedMediaType {
    enum Mode : uint32_t {
        FLAG_DECODER = 1 << 0,
        FLAG_ENCODER = 1 << 1,
    };

    // The media type.
    const char *mMediaType;
    // bitfields for modes.
    uint32_t mMode;
} AMediaCodecSupportedMediaType;

/**
 * Get an array of all the supported media types of a device.
 *
 * @param outMediaTypes The pointer to the output AMediaCodecSupportedMediaType array.
 *                      It is owned by the fraework and has an infinite lifetime.
 *
 * @param outCount size of the out array.
 *
 * Return AMEDIA_OK if successfully made the copy.
 * Return AMEDIA_ERROR_INVALID_PARAMETER if the @param outMediaTypes is invalid.
 */
media_status_t AMediaCodecStore_getSupportedMediaTypes(
        const AMediaCodecSupportedMediaType **outMediaTypes, size_t *outCount) __INTRODUCED_IN(36);

/**
 * Get the next decoder info that supports the format.
 *
 * @param outCodecInfo  should be set as NULL to start the iteration.
 *                      Keep the last codecInfo you got from a previous call to get the next one.
 *                      *outCodecInfo will be set to NULL if reached the end.
 *                      It is owned by the framework and has an infinite lifetime.
 *
 * @param format        If set as NULL, this API will iterate through all available decoders.
 *                      If NOT NULL, it MUST contain key "mime" implying the media type.
 *
 * Return AMEDIA_OK if successfully got the info.
 * Return AMEDIA_ERROR_INVALID_PARAMETER if @param outCodecInfo or @param format is invalid.
 * Return AMEDIA_ERROR_UNSUPPORTED if no more decoder supporting the format.
 *
 * It is undefined behavior to call this API with a NON NULL @param outCodecInfo
 * and a different @param format during an iteration.
 */
media_status_t AMediaCodecStore_findNextDecoderForFormat(
        const AMediaFormat *format, const AMediaCodecInfo **outCodecInfo) __INTRODUCED_IN(36);

/**
 * Get the next encoder info that supports the format.
 *
 * @param outCodecInfo  should be set as NULL to start the iteration.
 *                      Keep the last codecInfo you got from a previous call to get the next one.
 *                      *outCodecInfo will be set to NULL if reached the end.
 *                      It is owned by the framework and has an infinite lifetime.
 *
 * @param format        If set as NULL, this API will iterate through all available encoders.
 *                      If NOT NULL, it MUST contain key "mime" implying the media type.
 *
 * Return AMEDIA_OK if successfully got the info.
 * Return AMEDIA_ERROR_INVALID_PARAMETER if @param outCodecInfo is invalid.
 * Return AMEDIA_ERROR_UNSUPPORTED if no more encoder supporting the format.
 *
 * It is undefined behavior to call this API with a NON NULL @param outCodecInfo
 * and a different @param format during an iteration.
 *
 * No secure encoder will show in the output.
 */
media_status_t AMediaCodecStore_findNextEncoderForFormat(
        const AMediaFormat* format, const AMediaCodecInfo **outCodecInfo) __INTRODUCED_IN(36);

/**
 * Get the codecInfo corresponding to a given codec name.
 *
 * @param name          Media codec name.
 *                      Users can get valid codec names from the AMediaCodecInfo structures
 *                      returned from findNextDecoder|EncoderForFormat methods.
 *                      Note that this name may not correspond to the name the same codec used
 *                      by the SDK API, but will always do for codec names starting with "c2.".
 *
 * @param outCodecInfo  Output parameter for the corresponding AMeidaCodecInfo structure.
 *                      It is owned by the framework and has an infinite lifetime.
 *
 * Return AMEDIA_OK if got the codecInfo successfully.
 * Return AMEDIA_ERROR_UNSUPPORTED if no corresponding codec found.
 * Return AMEDIA_ERROR_INVALID_PARAMETER if @param outCodecInfo or @param name is invalid.
 */
media_status_t AMediaCodecStore_getCodecInfo(
        const char *name, const AMediaCodecInfo **outCodecInfo) __INTRODUCED_IN(36);

__END_DECLS

#endif //_NDK_MEDIA_CODEC_STORE_H

/** @} */