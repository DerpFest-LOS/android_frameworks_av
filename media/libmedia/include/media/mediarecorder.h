/*
 ** Copyright (C) 2008 The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 **
 ** limitations under the License.
 */

#ifndef ANDROID_MEDIARECORDER_H
#define ANDROID_MEDIARECORDER_H

#include <utils/Log.h>
#include <utils/threads.h>
#include <utils/List.h>
#include <utils/Errors.h>
#include <media/AudioContainers.h>
#include <media/IMediaRecorderClient.h>
#include <media/IMediaDeathNotifier.h>
#include <android/media/MicrophoneInfoFw.h>
#include <android/content/AttributionSourceState.h>

namespace android {

class Surface;
class IMediaRecorder;
class ICameraRecordingProxy;
class IGraphicBufferProducer;
struct PersistentSurface;
class Surface;

namespace hardware {
class ICamera;
}

typedef void (*media_completion_f)(status_t status, void *cookie);

enum video_source {
    VIDEO_SOURCE_DEFAULT = 0,
    VIDEO_SOURCE_CAMERA = 1,
    VIDEO_SOURCE_SURFACE = 2,

    VIDEO_SOURCE_LIST_END  // must be last - used to validate audio source type
};

//Please update media/java/android/media/MediaRecorder.java if the following is updated.
enum output_format {
    OUTPUT_FORMAT_DEFAULT = 0,
    OUTPUT_FORMAT_THREE_GPP = 1,
    OUTPUT_FORMAT_MPEG_4 = 2,


    OUTPUT_FORMAT_AUDIO_ONLY_START = 3, // Used in validating the output format.  Should be the
                                        //  at the start of the audio only output formats.

    /* These are audio only file formats */
    OUTPUT_FORMAT_RAW_AMR = 3, //to be backward compatible
    OUTPUT_FORMAT_AMR_NB = 3,
    OUTPUT_FORMAT_AMR_WB = 4,
    OUTPUT_FORMAT_AAC_ADIF = 5,
    OUTPUT_FORMAT_AAC_ADTS = 6,

    OUTPUT_FORMAT_AUDIO_ONLY_END = 7, // Used in validating the output format.  Should be the
                                      // at the end of the audio only output formats.

    /* Stream over a socket, limited to a single stream */
    OUTPUT_FORMAT_RTP_AVP = 7,

    /* H.264/AAC data encapsulated in MPEG2/TS */
    OUTPUT_FORMAT_MPEG2TS = 8,

    /* VP8/VORBIS data in a WEBM container */
    OUTPUT_FORMAT_WEBM = 9,

    /* HEIC data in a HEIF container */
    OUTPUT_FORMAT_HEIF = 10,

    /* Opus data in a OGG container */
    OUTPUT_FORMAT_OGG = 11,

    OUTPUT_FORMAT_LIST_END // must be last - used to validate format type
};

enum audio_encoder {
    AUDIO_ENCODER_DEFAULT = 0,
    AUDIO_ENCODER_AMR_NB = 1,
    AUDIO_ENCODER_AMR_WB = 2,
    AUDIO_ENCODER_AAC = 3,
    AUDIO_ENCODER_HE_AAC = 4,
    AUDIO_ENCODER_AAC_ELD = 5,
    AUDIO_ENCODER_VORBIS = 6,
    AUDIO_ENCODER_OPUS = 7,

    AUDIO_ENCODER_LIST_END // must be the last - used to validate the audio encoder type
};

enum video_encoder {
    VIDEO_ENCODER_DEFAULT = 0,
    VIDEO_ENCODER_H263 = 1,
    VIDEO_ENCODER_H264 = 2,
    VIDEO_ENCODER_MPEG_4_SP = 3,
    VIDEO_ENCODER_VP8 = 4,
    VIDEO_ENCODER_HEVC = 5,
    VIDEO_ENCODER_VP9 = 6,
    VIDEO_ENCODER_DOLBY_VISION = 7,
    VIDEO_ENCODER_AV1 = 8,
    VIDEO_ENCODER_LIST_END // must be the last - used to validate the video encoder type
};

/*
 * The state machine of the media_recorder.
 */
enum media_recorder_states {
    // Recorder was just created.
    MEDIA_RECORDER_IDLE                  = 1 << 0,

    // Recorder has been initialized.
    MEDIA_RECORDER_INITIALIZED           = 1 << 1,

    // Configuration of the recorder has been completed.
    MEDIA_RECORDER_DATASOURCE_CONFIGURED = 1 << 2,

    // Recorder is ready to start.
    MEDIA_RECORDER_PREPARED              = 1 << 3,

    // Recording is in progress.
    MEDIA_RECORDER_RECORDING             = 1 << 4,

    // Error state.
    MEDIA_RECORDER_ERROR                 = 1 << 5,
};

// The "msg" code passed to the listener in notify.
enum media_recorder_event_type {
    MEDIA_RECORDER_EVENT_LIST_START               = 1,
    MEDIA_RECORDER_EVENT_ERROR                    = 1,
    MEDIA_RECORDER_EVENT_INFO                     = 2,
    MEDIA_RECORDER_EVENT_LIST_END                 = 99,

    // Track related event types
    MEDIA_RECORDER_TRACK_EVENT_LIST_START         = 100,
    MEDIA_RECORDER_TRACK_EVENT_ERROR              = 100,
    MEDIA_RECORDER_TRACK_EVENT_INFO               = 101,
    MEDIA_RECORDER_TRACK_EVENT_LIST_END           = 1000,

    MEDIA_RECORDER_AUDIO_ROUTING_CHANGED          = 10000,
};

/*
 * The (part of) "what" code passed to the listener in notify.
 * When the error or info type is track specific, the what has
 * the following layout:
 * the left-most 16-bit is meant for error or info type.
 * the right-most 4-bit is meant for track id.
 * the rest is reserved.
 *
 * | track id | reserved |     error or info type     |
 * 31         28         16                           0
 *
 */
enum media_recorder_error_type {
    MEDIA_RECORDER_ERROR_UNKNOWN                   = 1,

    // Track related error type
    MEDIA_RECORDER_TRACK_ERROR_LIST_START          = 100,
    MEDIA_RECORDER_TRACK_ERROR_GENERAL             = 100,
    MEDIA_RECORDER_ERROR_VIDEO_NO_SYNC_FRAME       = 200,
    MEDIA_RECORDER_TRACK_ERROR_LIST_END            = 1000,
};

// The codes are distributed as follow:
//   0xx: Reserved
//   8xx: General info/warning
//
enum media_recorder_info_type {
    MEDIA_RECORDER_INFO_UNKNOWN                   = 1,

    MEDIA_RECORDER_INFO_MAX_DURATION_REACHED      = 800,
    MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED      = 801,
    MEDIA_RECORDER_INFO_MAX_FILESIZE_APPROACHING  = 802,
    MEDIA_RECORDER_INFO_NEXT_OUTPUT_FILE_STARTED  = 803,

    // All track related informtional events start here
    MEDIA_RECORDER_TRACK_INFO_LIST_START           = 1000,
    MEDIA_RECORDER_TRACK_INFO_COMPLETION_STATUS    = 1000,
    MEDIA_RECORDER_TRACK_INFO_PROGRESS_IN_TIME     = 1001,
    MEDIA_RECORDER_TRACK_INFO_TYPE                 = 1002,
    MEDIA_RECORDER_TRACK_INFO_DURATION_MS          = 1003,

    // The time to measure the max chunk duration
    MEDIA_RECORDER_TRACK_INFO_MAX_CHUNK_DUR_MS     = 1004,

    MEDIA_RECORDER_TRACK_INFO_ENCODED_FRAMES       = 1005,

    // The time to measure how well the audio and video
    // track data is interleaved.
    MEDIA_RECORDER_TRACK_INTER_CHUNK_TIME_MS       = 1006,

    // The time to measure system response. Note that
    // the delay does not include the intentional delay
    // we use to eliminate the recording sound.
    MEDIA_RECORDER_TRACK_INFO_INITIAL_DELAY_MS     = 1007,

    // The time used to compensate for initial A/V sync.
    MEDIA_RECORDER_TRACK_INFO_START_OFFSET_MS      = 1008,

    // Total number of bytes of the media data.
    MEDIA_RECORDER_TRACK_INFO_DATA_KBYTES          = 1009,

    MEDIA_RECORDER_TRACK_INFO_LIST_END             = 2000,
};

// ----------------------------------------------------------------------------
// ref-counted object for callbacks
class MediaRecorderListener: virtual public RefBase
{
public:
    virtual void notify(int msg, int ext1, int ext2) = 0;
};

class MediaRecorder : public BnMediaRecorderClient,
                      public virtual IMediaDeathNotifier
{
public:
    explicit MediaRecorder(const android::content::AttributionSourceState& attributionSource);
    ~MediaRecorder();

    void        died();
    status_t    initCheck();
    status_t    setCamera(const sp<hardware::ICamera>& camera,
            const sp<ICameraRecordingProxy>& proxy);
    status_t    setPreviewSurface(const sp<IGraphicBufferProducer>& surface);
    status_t    setVideoSource(int vs);
    status_t    setAudioSource(int as);
    status_t    setPrivacySensitive(bool privacySensitive);
    status_t    isPrivacySensitive(bool *privacySensitive) const;
    status_t    setOutputFormat(int of);
    status_t    setVideoEncoder(int ve);
    status_t    setAudioEncoder(int ae);
    status_t    setOutputFile(int fd);
    status_t    setNextOutputFile(int fd);
    status_t    setVideoSize(int width, int height);
    status_t    setVideoFrameRate(int frames_per_second);
    status_t    setParameters(const String8& params);
    status_t    setListener(const sp<MediaRecorderListener>& listener);
    status_t    setClientName(const String16& clientName);
    status_t    prepare();
    status_t    getMaxAmplitude(int* max);
    status_t    start();
    status_t    stop();
    status_t    reset();
    status_t    pause();
    status_t    resume();
    status_t    init();
    status_t    close();
    status_t    release();
    void        notify(int msg, int ext1, int ext2);
    status_t    setInputSurface(const sp<PersistentSurface>& surface);
    sp<IGraphicBufferProducer>     querySurfaceMediaSourceFromMediaServer();
    status_t    getMetrics(Parcel *reply);
    status_t    setInputDevice(audio_port_handle_t deviceId);
    status_t    getRoutedDeviceIds(DeviceIdVector& deviceIds);
    status_t    enableAudioDeviceCallback(bool enabled);
    status_t    getActiveMicrophones(std::vector<media::MicrophoneInfoFw>* activeMicrophones);
    status_t    setPreferredMicrophoneDirection(audio_microphone_direction_t direction);
    status_t    setPreferredMicrophoneFieldDimension(float zoom);

    status_t    getPortId(audio_port_handle_t *portId) const;
    status_t    getRtpDataUsage(uint64_t *bytes);

private:
    void                    doCleanUp();
    status_t                doReset();

    sp<IMediaRecorder>          mMediaRecorder;
    sp<MediaRecorderListener>   mListener;

    // Reference to IGraphicBufferProducer
    // for encoding GL Frames. That is useful only when the
    // video source is set to VIDEO_SOURCE_GRALLOC_BUFFER
    sp<IGraphicBufferProducer>  mSurfaceMediaSource;

    media_recorder_states       mCurrentState;
    bool                        mIsAudioSourceSet;
    bool                        mIsVideoSourceSet;
    bool                        mIsAudioEncoderSet;
    bool                        mIsVideoEncoderSet;
    bool                        mIsOutputFileSet;
    Mutex                       mLock;
    Mutex                       mNotifyLock;

    output_format               mOutputFormat;
};

};  // namespace android

#endif // ANDROID_MEDIARECORDER_H
