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
#define LOG_TAG "AmrwbDecoderTest"
#define OUTPUT_FILE "/data/local/tmp/amrwbDecode.out"

#include <utils/Log.h>

#include <audio_utils/sndfile.h>
#include <memory>
#include <stdio.h>
#include <fstream>

#include "pvamrwbdecoder.h"
#include "pvamrwbdecoder_api.h"

#include "AmrwbDecTestEnvironment.h"

// Constants for AMR-WB.
constexpr int32_t kInputBufferSize = 64;
constexpr int32_t kSamplesPerFrame = 320;
constexpr int32_t kBitsPerSample = 16;
constexpr int32_t kSampleRate = 16000;
constexpr int32_t kChannels = 1;
constexpr int32_t kMaxSourceDataUnitSize = KAMRWB_NB_BITS_MAX * sizeof(int16_t);
constexpr int32_t kOutputBufferSize = kSamplesPerFrame * kBitsPerSample / 8;
const int32_t kFrameSizes[16] = {17, 23, 32, 36, 40, 46, 50, 58, 60, -1, -1, -1, -1, -1, -1, -1};
constexpr int32_t kNumFrameReset = 150;

constexpr int32_t kMaxCount = 10;

static AmrwbDecTestEnvironment *gEnv = nullptr;

class AmrwbDecoderTest : public ::testing::TestWithParam<std::tuple<string, string>> {
  public:
    AmrwbDecoderTest() : mFpInput(nullptr) {}

    ~AmrwbDecoderTest() {
        if (mFpInput) {
            fclose(mFpInput);
            mFpInput = nullptr;
        }
    }

    FILE *mFpInput;
    int32_t DecodeFrames(int16_t *decoderCookie, void *decoderBuf, SNDFILE *outFileHandle,
                         int32_t frameCount = INT32_MAX);
    SNDFILE *openOutputFile(SF_INFO *sfInfo);
    bool compareBinaryFiles(const std::string& refFilePath, const std::string& outFilePath);
};

SNDFILE *AmrwbDecoderTest::openOutputFile(SF_INFO *sfInfo) {
    memset(sfInfo, 0, sizeof(SF_INFO));
    sfInfo->channels = kChannels;
    sfInfo->format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    sfInfo->samplerate = kSampleRate;
    SNDFILE *outFileHandle = sf_open(OUTPUT_FILE, SFM_WRITE, sfInfo);
    return outFileHandle;
}

int32_t AmrwbDecoderTest::DecodeFrames(int16_t *decoderCookie, void *decoderBuf,
                                       SNDFILE *outFileHandle, int32_t frameCount) {
    uint8_t inputBuf[kInputBufferSize];
    int16_t inputSampleBuf[kMaxSourceDataUnitSize];
    int16_t outputBuf[kOutputBufferSize];
    RX_State_wb rx_state{};

    while (frameCount > 0) {
        uint8_t modeByte;
        int32_t bytesRead = fread(&modeByte, 1, 1, mFpInput);
        if (bytesRead != 1) break;

        int16 mode = ((modeByte >> 3) & 0x0f);
        if (mode >= 9) {
            // Produce silence for comfort noise, speech lost and no data.
            int32_t outputBufferSize = kSamplesPerFrame * kBitsPerSample / 8;
            memset(outputBuf, 0, outputBufferSize);
        } else {
            // Read rest of the frame.
            int32_t frameSize = kFrameSizes[mode];
            // AMR-WB file format cannot have mode 10, 11, 12 and 13.
            if (frameSize < 0) {
                ALOGE("Illegal frame mode");
                return -1;
            }
            bytesRead = fread(inputBuf, 1, frameSize, mFpInput);
            if (bytesRead != frameSize) break;

            int16 frameMode = mode;
            int16 frameType;
            mime_unsorting(inputBuf, inputSampleBuf, &frameType, &frameMode, 1, &rx_state);

            int16_t numSamplesOutput;
            pvDecoder_AmrWb(frameMode, inputSampleBuf, outputBuf, &numSamplesOutput, decoderBuf,
                            frameType, decoderCookie);
            if (numSamplesOutput != kSamplesPerFrame) {
                ALOGE("Failed to decode the input file");
                return -1;
            }
            for (int count = 0; count < kSamplesPerFrame; ++count) {
                /* Delete the 2 LSBs (14-bit output) */
                outputBuf[count] &= 0xfffc;
            }
        }
        sf_writef_short(outFileHandle, outputBuf, kSamplesPerFrame / kChannels);
        frameCount--;
    }
    return 0;
}

bool AmrwbDecoderTest::compareBinaryFiles(const std::string &refFilePath,
                                          const std::string &outFilePath) {
    std::ifstream refFile(refFilePath, std::ios::binary | std::ios::ate);
    std::ifstream outFile(outFilePath, std::ios::binary | std::ios::ate);
    assert(refFile.is_open() && "Error opening reference file " + refFilePath);
    assert(outFile.is_open() && "Error opening output file " + outFilePath);

    std::streamsize refFileSize = refFile.tellg();
    std::streamsize outFileSize = outFile.tellg();
    if (refFileSize != outFileSize) {
        ALOGE("Error, File size mismatch: Reference file size = %td bytes,"
               "but output file size = %td bytes", refFileSize, outFileSize);
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

TEST_F(AmrwbDecoderTest, MultiCreateAmrwbDecoderTest) {
    uint32_t memRequirements = pvDecoder_AmrWbMemRequirements();
    std::unique_ptr<char[]> decoderBuf(new char[memRequirements]);
    ASSERT_NE(decoderBuf, nullptr)
            << "Failed to allocate decoder memory of size " << memRequirements;

    // Create AMR-WB decoder instance.
    void *amrHandle = nullptr;
    int16_t *decoderCookie;
    for (int count = 0; count < kMaxCount; count++) {
        pvDecoder_AmrWb_Init(&amrHandle, decoderBuf.get(), &decoderCookie);
        ASSERT_NE(amrHandle, nullptr) << "Failed to initialize decoder";
        ALOGV("Decoder created successfully");
    }
}

TEST_P(AmrwbDecoderTest, DecodeTest) {
    uint32_t memRequirements = pvDecoder_AmrWbMemRequirements();
    std::unique_ptr<char[]> decoderBuf(new char[memRequirements]);
    ASSERT_NE(decoderBuf, nullptr)
            << "Failed to allocate decoder memory of size " << memRequirements;

    void *amrHandle = nullptr;
    int16_t *decoderCookie;
    pvDecoder_AmrWb_Init(&amrHandle, decoderBuf.get(), &decoderCookie);
    ASSERT_NE(amrHandle, nullptr) << "Failed to initialize decoder";

    string inputFile = gEnv->getRes() + std::get<0>(GetParam());
    mFpInput = fopen(inputFile.c_str(), "rb");
    ASSERT_NE(mFpInput, nullptr) << "Error opening input file " << inputFile;

    // Open the output file.
    SF_INFO sfInfo;
    SNDFILE *outFileHandle = openOutputFile(&sfInfo);
    ASSERT_NE(outFileHandle, nullptr) << "Error opening output file for writing decoded output";

    int32_t decoderErr = DecodeFrames(decoderCookie, decoderBuf.get(), outFileHandle);
    ASSERT_EQ(decoderErr, 0) << "DecodeFrames returned error";

    sf_close(outFileHandle);
    string refFilePath = gEnv->getRes() + std::get<1>(GetParam());
    ASSERT_TRUE(compareBinaryFiles(refFilePath, OUTPUT_FILE))
    << "Error, Binary file comparison failed: Output file "
    << OUTPUT_FILE << " does not match the reference file " << refFilePath << ".";
}

TEST_P(AmrwbDecoderTest, ResetDecoderTest) {
    uint32_t memRequirements = pvDecoder_AmrWbMemRequirements();
    std::unique_ptr<char[]> decoderBuf(new char[memRequirements]);
    ASSERT_NE(decoderBuf, nullptr)
            << "Failed to allocate decoder memory of size " << memRequirements;

    void *amrHandle = nullptr;
    int16_t *decoderCookie;
    pvDecoder_AmrWb_Init(&amrHandle, decoderBuf.get(), &decoderCookie);
    ASSERT_NE(amrHandle, nullptr) << "Failed to initialize decoder";

    string inputFile = gEnv->getRes() + std::get<0>(GetParam());
    mFpInput = fopen(inputFile.c_str(), "rb");
    ASSERT_NE(mFpInput, nullptr) << "Error opening input file " << inputFile;

    // Open the output file.
    SF_INFO sfInfo;
    SNDFILE *outFileHandle = openOutputFile(&sfInfo);
    ASSERT_NE(outFileHandle, nullptr) << "Error opening output file for writing decoded output";

    // Decode 150 frames first
    int32_t decoderErr =
            DecodeFrames(decoderCookie, decoderBuf.get(), outFileHandle, kNumFrameReset);
    ASSERT_EQ(decoderErr, 0) << "DecodeFrames returned error";

    // Reset Decoder
    pvDecoder_AmrWb_Reset(decoderBuf.get(), 1);

    // Start decoding again
    decoderErr = DecodeFrames(decoderCookie, decoderBuf.get(), outFileHandle);
    ASSERT_EQ(decoderErr, 0) << "DecodeFrames returned error";

    sf_close(outFileHandle);
}

INSTANTIATE_TEST_SUITE_P(AmrwbDecoderTestAll, AmrwbDecoderTest,
                         ::testing::Values(std::make_tuple(
                                                "bbb_amrwb_1ch_14kbps_16000hz.amrwb",
                                                "bbb_amrwb_1ch_14kbps_16000hz_ref.pcm"),
                                           std::make_tuple(
                                                "bbb_16000hz_1ch_9kbps_amrwb_30sec.amrwb",
                                                "bbb_16000hz_1ch_9kbps_amrwb_30sec_ref.pcm"),
                                           std::make_tuple(
                                                "bbb_16kHz_1ch_16bps_1sec.amrwb",
                                                "bbb_16kHz_1ch_16bps_1sec_ref.pcm"),
                                           std::make_tuple(
                                                "bbb_16kHz_1ch_6.6bps_3sec.amrwb",
                                                "bbb_16kHz_1ch_6.6bps_3sec_ref.pcm"),
                                           std::make_tuple(
                                                "bbb_16kHz_1ch_23.85bps_3sec.amrwb",
                                                "bbb_16kHz_1ch_23.85bps_3sec_ref.pcm")));

int main(int argc, char **argv) {
    gEnv = new AmrwbDecTestEnvironment();
    ::testing::AddGlobalTestEnvironment(gEnv);
    ::testing::InitGoogleTest(&argc, argv);
    int status = gEnv->initFromOptions(argc, argv);
    if (status == 0) {
        status = RUN_ALL_TESTS();
        ALOGV("Test result = %d\n", status);
    }
    return status;
}
