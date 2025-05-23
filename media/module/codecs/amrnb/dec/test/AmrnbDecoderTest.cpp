/*
 * Copyright (C) 2019 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "AmrnbDecoderTest"
#define OUTPUT_FILE "/data/local/tmp/amrnbDecode.out"

#include <utils/Log.h>

#include <audio_utils/sndfile.h>
#include <stdio.h>
#include <fstream>

#include "gsmamr_dec.h"

#include "AmrnbDecTestEnvironment.h"

// Constants for AMR-NB
constexpr int32_t kInputBufferSize = 64;
constexpr int32_t kSamplesPerFrame = L_FRAME;
constexpr int32_t kBitsPerSample = 16;
constexpr int32_t kSampleRate = 8000;
constexpr int32_t kChannels = 1;
constexpr int32_t kOutputBufferSize = kSamplesPerFrame * kBitsPerSample / 8;
const int32_t kFrameSizes[] = {12, 13, 15, 17, 19, 20, 26, 31, -1, -1, -1, -1, -1, -1, -1, -1};

constexpr int32_t kNumFrameReset = 150;

static AmrnbDecTestEnvironment *gEnv = nullptr;

class AmrnbDecoderTest : public ::testing::TestWithParam<std::tuple<string, string>> {
  public:
    AmrnbDecoderTest() : mFpInput(nullptr) {}

    ~AmrnbDecoderTest() {
        if (mFpInput) {
            fclose(mFpInput);
            mFpInput = nullptr;
        }
    }

    FILE *mFpInput;
    SNDFILE *openOutputFile(SF_INFO *sfInfo);
    int32_t DecodeFrames(void *amrHandle, SNDFILE *outFileHandle, int32_t frameCount = INT32_MAX);
    bool compareBinaryFiles(const std::string& refFilePath, const std::string& outFilePath);
};

SNDFILE *AmrnbDecoderTest::openOutputFile(SF_INFO *sfInfo) {
    memset(sfInfo, 0, sizeof(SF_INFO));
    sfInfo->channels = kChannels;
    sfInfo->format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    sfInfo->samplerate = kSampleRate;
    SNDFILE *outFileHandle = sf_open(OUTPUT_FILE, SFM_WRITE, sfInfo);
    return outFileHandle;
}

int32_t AmrnbDecoderTest::DecodeFrames(void *amrHandle, SNDFILE *outFileHandle,
                                       int32_t frameCount) {
    uint8_t inputBuf[kInputBufferSize];
    int16_t outputBuf[kOutputBufferSize];

    while (frameCount > 0) {
        uint8_t mode;
        int32_t bytesRead = fread(&mode, 1, 1, mFpInput);
        if (bytesRead != 1) break;

        // Find frame type
        Frame_Type_3GPP frameType = (Frame_Type_3GPP)((mode >> 3) & 0x0f);
        int32_t frameSize = kFrameSizes[frameType];
        if (frameSize < 0) {
            ALOGE("Illegal frame type");
            return -1;
        }
        bytesRead = fread(inputBuf, 1, frameSize, mFpInput);
        if (bytesRead != frameSize) break;

        int32_t bytesDecoded = AMRDecode(amrHandle, frameType, inputBuf, outputBuf, MIME_IETF);
        if (bytesDecoded == -1) {
            ALOGE("Failed to decode the input file");
            return -1;
        }

        sf_writef_short(outFileHandle, outputBuf, kSamplesPerFrame);
        frameCount--;
    }
    return 0;
}

bool AmrnbDecoderTest::compareBinaryFiles(const std::string &refFilePath,
                                          const std::string &outFilePath) {
    std::ifstream refFile(refFilePath, std::ios::binary | std::ios::ate);
    std::ifstream outFile(outFilePath, std::ios::binary | std::ios::ate);
    assert(refFile.is_open() && "Error opening reference file " + refFilePath);
    assert(outFile.is_open() && "Error opening output file " + outFilePath);

    std::streamsize refFileSize = refFile.tellg();
    std::streamsize outFileSize = outFile.tellg();
    if (refFileSize != outFileSize) {
        ALOGE("Error, File size mismatch: Reference file size = %td bytes,"
              " but output file size = %td bytes.", refFileSize, outFileSize);
        return false;
    }

    refFile.seekg(0, std::ios::beg);
    outFile.seekg(0, std::ios::beg);
    constexpr std::streamsize kBufferSize = 16 * 1024;
    char refBuffer[kBufferSize];
    char outBuffer[kBufferSize];

    while (refFile && outFile) {
        refFile.read(refBuffer, kBufferSize);
        outFile.read(outBuffer, kBufferSize);

        std::streamsize refBytesRead = refFile.gcount();
        std::streamsize outBytesRead = outFile.gcount();

        if (refBytesRead != outBytesRead || memcmp(refBuffer, outBuffer, refBytesRead) != 0) {
            ALOGE("Error, File content mismatch.");
            return false;
        }
    }
    return true;
}

TEST_F(AmrnbDecoderTest, CreateAmrnbDecoderTest) {
    void *amrHandle;
    int32_t status = GSMInitDecode(&amrHandle, (Word8 *)"AMRNBDecoder");
    ASSERT_EQ(status, 0) << "Error creating AMR-NB decoder";
    GSMDecodeFrameExit(&amrHandle);
    ASSERT_EQ(amrHandle, nullptr) << "Error deleting AMR-NB decoder";
}

TEST_P(AmrnbDecoderTest, DecodeTest) {
    string inputFile = gEnv->getRes() + std::get<0>(GetParam());
    mFpInput = fopen(inputFile.c_str(), "rb");
    ASSERT_NE(mFpInput, nullptr) << "Error opening input file " << inputFile;

    // Open the output file.
    SF_INFO sfInfo;
    SNDFILE *outFileHandle = openOutputFile(&sfInfo);
    ASSERT_NE(outFileHandle, nullptr) << "Error opening output file for writing decoded output";

    void *amrHandle;
    int32_t status = GSMInitDecode(&amrHandle, (Word8 *)"AMRNBDecoder");
    ASSERT_EQ(status, 0) << "Error creating AMR-NB decoder";

    // Decode
    int32_t decoderErr = DecodeFrames(amrHandle, outFileHandle);
    ASSERT_EQ(decoderErr, 0) << "DecodeFrames returned error";

    sf_close(outFileHandle);
    GSMDecodeFrameExit(&amrHandle);
    ASSERT_EQ(amrHandle, nullptr) << "Error deleting AMR-NB decoder";

    string refFilePath = gEnv->getRes() + std::get<1>(GetParam());
    ASSERT_TRUE(compareBinaryFiles(refFilePath, OUTPUT_FILE))
       << "Error, Binary file comparison failed: Output file " << OUTPUT_FILE
       << " does not match the reference file " << refFilePath << ".";
}

TEST_P(AmrnbDecoderTest, ResetDecodeTest) {
    string inputFile = gEnv->getRes() + std::get<0>(GetParam());
    mFpInput = fopen(inputFile.c_str(), "rb");
    ASSERT_NE(mFpInput, nullptr) << "Error opening input file " << inputFile;

    // Open the output file.
    SF_INFO sfInfo;
    SNDFILE *outFileHandle = openOutputFile(&sfInfo);
    ASSERT_NE(outFileHandle, nullptr) << "Error opening output file for writing decoded output";

    void *amrHandle;
    int32_t status = GSMInitDecode(&amrHandle, (Word8 *)"AMRNBDecoder");
    ASSERT_EQ(status, 0) << "Error creating AMR-NB decoder";

    // Decode kNumFrameReset first
    int32_t decoderErr = DecodeFrames(amrHandle, outFileHandle, kNumFrameReset);
    ASSERT_EQ(decoderErr, 0) << "DecodeFrames returned error";

    status = Speech_Decode_Frame_reset(amrHandle);
    ASSERT_EQ(status, 0) << "Error resting AMR-NB decoder";

    // Start decoding again
    decoderErr = DecodeFrames(amrHandle, outFileHandle);
    ASSERT_EQ(decoderErr, 0) << "DecodeFrames returned error";

    sf_close(outFileHandle);
    GSMDecodeFrameExit(&amrHandle);
    ASSERT_EQ(amrHandle, nullptr) << "Error deleting AMR-NB decoder";
}

INSTANTIATE_TEST_SUITE_P(AmrnbDecoderTestAll, AmrnbDecoderTest,
                         ::testing::Values(std::make_tuple(
                                                   "bbb_8000hz_1ch_8kbps_amrnb_30sec.amrnb",
                                                   "bbb_8000hz_1ch_8kbps_amrnb_30sec_ref.pcm"),
                                           std::make_tuple(
                                                   "sine_amrnb_1ch_12kbps_8000hz.amrnb",
                                                   "sine_amrnb_1ch_12kbps_8000hz_ref.pcm"),
                                           std::make_tuple(
                                                   "trim_8000hz_1ch_12kpbs_amrnb_200ms.amrnb",
                                                   "trim_8000hz_1ch_12kpbs_amrnb_200ms_ref.pcm"),
                                           std::make_tuple(
                                                   "bbb_8kHz_1ch_4.75kbps_amrnb_3sec.amrnb",
                                                   "bbb_8kHz_1ch_4.75kbps_amrnb_3sec_ref.pcm"),
                                           std::make_tuple(
                                                   "bbb_8kHz_1ch_10kbps_amrnb_1sec.amrnb",
                                                   "bbb_8kHz_1ch_10kbps_amrnb_1sec_ref.pcm"),
                                           std::make_tuple(
                                                   "bbb_8kHz_1ch_12.2kbps_amrnb_3sec.amrnb",
                                                   "bbb_8kHz_1ch_12.2kbps_amrnb_3sec_ref.pcm")));

int main(int argc, char **argv) {
    gEnv = new AmrnbDecTestEnvironment();
    ::testing::AddGlobalTestEnvironment(gEnv);
    ::testing::InitGoogleTest(&argc, argv);
    int status = gEnv->initFromOptions(argc, argv);
    if (status == 0) {
        status = RUN_ALL_TESTS();
        ALOGV("Test result = %d\n", status);
    }
    return status;
}
