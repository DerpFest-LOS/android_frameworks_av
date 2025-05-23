/*
 * Copyright (C) 2021 The Android Open Source Project
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

package android.media;

import android.media.audio.common.AudioConfigBase;
import android.media.audio.common.AudioSource;

/**
 * {@hide}
 */
parcelable GetInputForAttrResponse {
    /** Interpreted as audio_io_handle_t. */
    int input;
    /** Interpreted as audio_port_handle_t. */
    int selectedDeviceId;
    /** Interpreted as audio_port_handle_t. */
    int portId;
    /** The virtual device id corresponding to the opened input. */
    int virtualDeviceId;
    /** The suggested config if fails to get an input. **/
    AudioConfigBase config;
    /** The audio source, possibly updated by audio policy manager */
    AudioSource source;
}
