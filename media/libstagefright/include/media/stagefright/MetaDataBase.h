/*
 * Copyright (C) 2009 The Android Open Source Project
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

#ifndef META_DATA_BASE_H_

#define META_DATA_BASE_H_

#include <sys/types.h>

#include <stdint.h>

#include <utils/RefBase.h>
#include <utils/String8.h>

namespace android {

// The following keys map to int32_t data unless indicated otherwise.
enum {
    kKeyMIMEType          = 'mime',  // cstring
    kKeyWidth             = 'widt',  // int32_t, image pixel
    kKeyHeight            = 'heig',  // int32_t, image pixel
    kKeyDisplayWidth      = 'dWid',  // int32_t, display/presentation
    kKeyDisplayHeight     = 'dHgt',  // int32_t, display/presentation
    kKeySARWidth          = 'sarW',  // int32_t, sampleAspectRatio width
    kKeySARHeight         = 'sarH',  // int32_t, sampleAspectRatio height
    kKeyThumbnailWidth    = 'thbW',  // int32_t, thumbnail width
    kKeyThumbnailHeight   = 'thbH',  // int32_t, thumbnail height

    // a rectangle, if absent assumed to be (0, 0, width - 1, height - 1)
    kKeyCropRect          = 'crop',

    kKeyRotation          = 'rotA',  // int32_t (angle in degrees)
    kKeyIFramesInterval   = 'ifiv',  // int32_t
    kKeyStride            = 'strd',  // int32_t
    kKeySliceHeight       = 'slht',  // int32_t
    kKeyChannelCount      = '#chn',  // int32_t
    kKeyChannelMask       = 'chnm',  // int32_t
    kKeySampleRate        = 'srte',  // int32_t (audio sampling rate Hz)
    kKeyPcmEncoding       = 'PCMe',  // int32_t (audio encoding enum)
    kKeyFrameRate         = 'frmR',  // int32_t (video frame rate fps)
    kKeyBitRate           = 'brte',  // int32_t (bps)
    kKeyMaxBitRate        = 'mxBr',  // int32_t (bps)
    kKeyBitsPerSample     = 'bits',  // int32_t (bits per sample)
    kKeyStreamHeader      = 'stHd',  // raw data
    kKeyESDS              = 'esds',  // raw data
    kKeyAACProfile        = 'aacp',  // int32_t
    kKeyAVCC              = 'avcc',  // raw data
    kKeyHVCC              = 'hvcc',  // raw data
    kKeyDVCC              = 'dvcc',  // raw data
    kKeyDVVC              = 'dvvc',  // raw data
    kKeyDVWC              = 'dvwc',  // raw data
    kKeyAV1C              = 'av1c',  // raw data
    kKeyAPVC              = 'apvc',  // raw data
    kKeyThumbnailHVCC     = 'thvc',  // raw data
    kKeyThumbnailAV1C     = 'tav1',  // raw data
    kKeyD263              = 'd263',  // raw data
    kKeyOpusHeader        = 'ohdr',  // raw data
    kKeyOpusCodecDelay    = 'ocod',  // uint64_t (codec delay in ns)
    kKeyOpusSeekPreRoll   = 'ospr',  // uint64_t (seek preroll in ns)
    kKeyVp9CodecPrivate   = 'vp9p',  // raw data (vp9 csd information)
    kKeyIsSyncFrame       = 'sync',  // int32_t (bool)
    kKeyIsCodecConfig     = 'conf',  // int32_t (bool)
    kKeyIsMuxerData       = 'muxd',  // int32_t (bool)
    kKeyIsEndOfStream     = 'feos',  // int32_t (bool)
    kKeyTime              = 'time',  // int64_t (usecs)
    kKeyDecodingTime      = 'decT',  // int64_t (decoding timestamp in usecs)
    kKeyNTPTime           = 'ntpT',  // uint64_t (ntp-timestamp)
    kKeyTargetTime        = 'tarT',  // int64_t (usecs)
    kKeyDriftTime         = 'dftT',  // int64_t (usecs)
    kKeyAnchorTime        = 'ancT',  // int64_t (usecs)
    kKeyDuration          = 'dura',  // int64_t (usecs)
    kKeyPixelFormat       = 'pixf',  // int32_t
    kKeyColorFormat       = 'colf',  // int32_t
    kKeyColorSpace        = 'cols',  // int32_t
    kKeyGainmap           = 'gmap',  // int32_t
    kKeyPlatformPrivate   = 'priv',  // pointer
    kKeyDecoderComponent  = 'decC',  // cstring
    kKeyBufferID          = 'bfID',
    kKeyMaxInputSize      = 'inpS',
    kKeyMaxWidth          = 'maxW',
    kKeyMaxHeight         = 'maxH',
    kKeyThumbnailTime     = 'thbT',  // int64_t (usecs)
    kKeyTrackID           = 'trID',
    kKeyEncoderDelay      = 'encd',  // int32_t (frames)
    kKeyEncoderPadding    = 'encp',  // int32_t (frames)

    kKeyAlbum             = 'albu',  // cstring
    kKeyArtist            = 'arti',  // cstring
    kKeyAlbumArtist       = 'aart',  // cstring
    kKeyComposer          = 'comp',  // cstring
    kKeyGenre             = 'genr',  // cstring
    kKeyTitle             = 'titl',  // cstring
    kKeyYear              = 'year',  // cstring
    kKeyAlbumArt          = 'albA',  // compressed image data
    kKeyAuthor            = 'auth',  // cstring
    kKeyCDTrackNumber     = 'cdtr',  // cstring
    kKeyDiscNumber        = 'dnum',  // cstring
    kKeyDate              = 'date',  // cstring
    kKeyWriter            = 'writ',  // cstring
    kKeyCompilation       = 'cpil',  // cstring
    kKeyLocation          = 'loc ',  // cstring
    kKeyTimeScale         = 'tmsl',  // int32_t
    kKeyCaptureFramerate  = 'capF',  // float (capture fps)

    // video profile and level
    kKeyVideoProfile      = 'vprf',  // int32_t
    kKeyVideoLevel        = 'vlev',  // int32_t

    // audio profile and level
    // The codec framework doesn't distinguish between video and audio profiles,
    // so there is no need to define a separate key
    kKeyAudioProfile      = 'vprf',  // int32_t
    kKeyAudioLevel        = 'vlev',  // int32_t

    kKey2ByteNalLength    = '2NAL',  // int32_t (bool)

    // Identify the file output format for authoring
    // Please see <media/mediarecorder.h> for the supported
    // file output formats.
    kKeyFileType          = 'ftyp',  // int32_t

    // Track authoring progress status
    // kKeyTrackTimeStatus is used to track progress in elapsed time
    kKeyTrackTimeStatus   = 'tktm',  // int64_t

    kKeyRealTimeRecording = 'rtrc',  // bool (int32_t)
    kKeyBackgroundMode = 'bkmd',  // bool (int32_t)

    kKeyNumBuffers        = 'nbbf',  // int32_t

    // Ogg files can be tagged to be automatically looping...
    kKeyAutoLoop          = 'autL',  // bool (int32_t)

    kKeyValidSamples      = 'valD',  // int32_t

    kKeyIsUnreadable      = 'unre',  // bool (int32_t)

    // An indication that a video buffer has been rendered.
    kKeyRendered          = 'rend',  // bool (int32_t)

    // The language code for this media
    kKeyMediaLanguage     = 'lang',  // cstring

    // The manufacturer code for this media
    kKeyManufacturer  = 'manu',  // cstring

    // To store the timed text format data
    kKeyTextFormatData    = 'text',  // raw data

    kKeyRequiresSecureBuffers = 'secu',  // bool (int32_t)

    kKeyIsADTS            = 'adts',  // bool (int32_t)
    kKeyAACAOT            = 'aaot',  // int32_t

    kKeyMpeghProfileLevelIndication = 'hpli', // int32_t
    kKeyMpeghReferenceChannelLayout = 'hrcl', // int32_t
    kKeyMpeghCompatibleSets         = 'hcos', // raw data

    // If a MediaBuffer's data represents (at least partially) encrypted
    // data, the following fields aid in decryption.
    // The data can be thought of as pairs of plain and encrypted data
    // fragments, i.e. plain and encrypted data alternate.
    // The first fragment is by convention plain data (if that's not the
    // case, simply specify plain fragment size of 0).
    // kKeyEncryptedSizes and kKeyPlainSizes each map to an array of
    // size_t values. The sum total of all size_t values of both arrays
    // must equal the amount of data (i.e. MediaBuffer's range_length()).
    // If both arrays are present, they must be of the same size.
    // If only encrypted sizes are present it is assumed that all
    // plain sizes are 0, i.e. all fragments are encrypted.
    // To programmatically set these array, use the MetaDataBase::setData API, i.e.
    // const size_t encSizes[];
    // meta->setData(
    //  kKeyEncryptedSizes, 0 /* type */, encSizes, sizeof(encSizes));
    // A plain sizes array by itself makes no sense.
    kKeyEncryptedSizes    = 'encr',  // size_t[]
    kKeyPlainSizes        = 'plai',  // size_t[]
    kKeyCryptoKey         = 'cryK',  // uint8_t[16]
    kKeyCryptoIV          = 'cryI',  // uint8_t[16]
    kKeyCryptoMode        = 'cryM',  // int32_t

    kKeyCryptoDefaultIVSize = 'cryS',  // int32_t

    kKeyPssh              = 'pssh',  // raw data
    kKeyCASystemID        = 'caid',  // int32_t
    kKeyCASessionID       = 'seid',  // raw data
    kKeyCAPrivateData     = 'cadc',  // raw data

    kKeyEncryptedByteBlock = 'cblk',  // uint8_t
    kKeySkipByteBlock     = 'sblk',  // uint8_t

    // Please see MediaFormat.KEY_IS_AUTOSELECT.
    kKeyTrackIsAutoselect = 'auto', // bool (int32_t)
    // Please see MediaFormat.KEY_IS_DEFAULT.
    kKeyTrackIsDefault    = 'dflt', // bool (int32_t)
    // Similar to MediaFormat.KEY_IS_FORCED_SUBTITLE but pertains to av tracks as well.
    kKeyTrackIsForced     = 'frcd', // bool (int32_t)

    // H264 supplemental enhancement information offsets/sizes
    kKeySEI               = 'sei ', // raw data

    // MPEG user data offsets
    kKeyMpegUserData      = 'mpud', // size_t[]

    // HDR related
    kKeyHdrStaticInfo    = 'hdrS', // HDRStaticInfo
    kKeyHdr10PlusInfo    = 'hdrD', // raw data

    // color aspects
    kKeyColorRange       = 'cRng', // int32_t, color range, value defined by ColorAspects.Range
    kKeyColorPrimaries   = 'cPrm', // int32_t,
                                   // color Primaries, value defined by ColorAspects.Primaries
    kKeyTransferFunction = 'tFun', // int32_t,
                                   // transfer Function, value defined by ColorAspects.Transfer.
    kKeyColorMatrix      = 'cMtx', // int32_t,
                                   // color Matrix, value defined by ColorAspects.MatrixCoeffs.
    kKeyTemporalLayerId  = 'iLyr', // int32_t, temporal layer-id. 0-based (0 => base layer)
    kKeyTemporalLayerCount = 'cLyr', // int32_t, number of temporal layers encoded

    kKeyTileWidth        = 'tilW', // int32_t, HEIF tile width
    kKeyTileHeight       = 'tilH', // int32_t, HEIF tile height
    kKeyGridRows         = 'grdR', // int32_t, HEIF grid rows
    kKeyGridCols         = 'grdC', // int32_t, HEIF grid columns
    kKeyIccProfile       = 'prof', // raw data, ICC profile data
    kKeyIsPrimaryImage   = 'prim', // bool (int32_t), image track is the primary image
    kKeyFrameCount       = 'nfrm', // int32_t, total number of frame in video track
    kKeyExifOffset       = 'exof', // int64_t, Exif data offset
    kKeyExifSize         = 'exsz', // int64_t, Exif data size
    kKeyExifTiffOffset   = 'thdr', // int32_t, if > 0, buffer contains exif data block with
                                   // tiff hdr at specified offset
    kKeyXmpOffset        = 'xmof', // int64_t, XMP data offset
    kKeyXmpSize          = 'xmsz', // int64_t, XMP data size
    kKeyPcmBigEndian     = 'pcmb', // bool (int32_t)

    // Key for ALAC Magic Cookie
    kKeyAlacMagicCookie  = 'almc', // raw data

    // AC-4 AudioPresentationInfo
    kKeyAudioPresentationInfo = 'audP',  // raw data

    // opaque codec specific data being passed from extractor to codec
    kKeyOpaqueCSD0       = 'csd0',
    kKeyOpaqueCSD1       = 'csd1',
    kKeyOpaqueCSD2       = 'csd2',

    kKeyHapticChannelCount = 'hapC',

    /* MediaRecorder.h, error notifications can represent track ids with 4 bits only.
     * | track id | reserved |     error or info type     |
     * 31         28         16                           0
     */
    kKey4BitTrackIds = '4bid',

    // Treat empty track as malformed for MediaRecorder.
    kKeyEmptyTrackMalFormed = 'nemt', // bool (int32_t)

    kKeyVps              = 'sVps', // int32_t, indicates that a buffer has vps.
    kKeySps              = 'sSps', // int32_t, indicates that a buffer has sps.
    kKeyPps              = 'sPps', // int32_t, indicates that a buffer has pps.
    kKeySelfID           = 'sfid', // int32_t, source ID to identify itself on RTP protocol.
    kKeyPayloadType      = 'pTyp', // int32_t, SDP negotiated payload type.
    kKeyRtpExtMap        = 'extm', // int32_t, rtp extension ID for cvo on RTP protocol.
    kKeyRtpCvoDegrees    = 'cvod', // int32_t, rtp cvo degrees as per 3GPP 26.114.
    kKeyRtpDscp          = 'dscp', // int32_t, DSCP(Differentiated services codepoint) of RFC 2474.
    kKeyRtpEcn           = 'sEcn', // int32_t, ECN (Explicit Congestion Notification) of RFC 3168
    kKeySocketNetwork    = 'sNet', // int64_t, socket will be bound to network handle.

    // Slow-motion markers
    kKeySlowMotionMarkers = 'slmo', // raw data, byte array following spec for
                                    // MediaFormat#KEY_SLOW_MOTION_MARKERS

    kKeySampleFileOffset = 'sfof', // int64_t, sample's offset in a media file.
    kKeyLastSampleIndexInChunk = 'lsic',  //int64_t, index of last sample in a chunk.
    kKeySampleTimeBeforeAppend = 'lsba', // int64_t, timestamp of last sample of a track.

    // DVB component tag
    kKeyDvbComponentTag = 'copt', // int32_t, component tag for DVB video/audio/subtitle

    // DVB audio description
    kKeyDvbAudioDescription = 'addt', // bool (int32_t), DVB audio description only defined for
                                      // audio component

    // DVB teletext magazine number
    kKeyDvbTeletextMagazineNumber = 'ttxm', // int32_t, DVB teletext magazine number

    // DVB teletext page number
    kKeyDvbTeletextPageNumber = 'ttxp', // int32_t, DVB teletext page number
};

enum {
    kTypeESDS        = 'esds',
    kTypeAVCC        = 'avcc',
    kTypeHVCC        = 'hvcc',
    kTypeAV1C        = 'av1c',
    kTypeDVCC        = 'dvcc',
    kTypeDVVC        = 'dvvc',
    kTypeDVWC        = 'dvwc',
    kTypeD263        = 'd263',
    kTypeHCOS        = 'hcos',
};

enum {
    kCryptoModeUnencrypted = 0,
    kCryptoModeAesCtr      = 1,
    kCryptoModeAesCbc      = 2,
};

class Parcel;

class MetaDataBase {
public:
    MetaDataBase();
    MetaDataBase(const MetaDataBase &from);
    MetaDataBase& operator = (const MetaDataBase &);

    virtual ~MetaDataBase();

    enum Type {
        TYPE_NONE     = 'none',
        TYPE_C_STRING = 'cstr',
        TYPE_INT32    = 'in32',
        TYPE_INT64    = 'in64',
        TYPE_FLOAT    = 'floa',
        TYPE_POINTER  = 'ptr ',
        TYPE_RECT     = 'rect',
    };

    void clear();
    bool remove(uint32_t key);

    bool setCString(uint32_t key, const char *value);
    bool setInt32(uint32_t key, int32_t value);
    bool setInt64(uint32_t key, int64_t value);
    bool setFloat(uint32_t key, float value);
    bool setPointer(uint32_t key, void *value);

    bool setRect(
            uint32_t key,
            int32_t left, int32_t top,
            int32_t right, int32_t bottom);

    bool findCString(uint32_t key, const char **value) const;
    bool findInt32(uint32_t key, int32_t *value) const;
    bool findInt64(uint32_t key, int64_t *value) const;
    bool findFloat(uint32_t key, float *value) const;
    bool findPointer(uint32_t key, void **value) const;

    bool findRect(
            uint32_t key,
            int32_t *left, int32_t *top,
            int32_t *right, int32_t *bottom) const;

    bool setData(uint32_t key, uint32_t type, const void *data, size_t size);

    bool findData(uint32_t key, uint32_t *type,
                  const void **data, size_t *size) const;

    bool hasData(uint32_t key) const;

    String8 toString() const;
    void dumpToLog() const;

private:
    friend class BpMediaSource;
    friend class BnMediaSource;
    friend class BnMediaExtractor;
    friend class MetaData;

    struct typed_data;
    struct Rect;
    struct MetaDataInternal;
    MetaDataInternal *mInternalData;
#ifndef __ANDROID_VNDK__
    status_t writeToParcel(Parcel &parcel);
    status_t updateFromParcel(const Parcel &parcel);
#endif
};

}  // namespace android

#endif  // META_DATA_H_
