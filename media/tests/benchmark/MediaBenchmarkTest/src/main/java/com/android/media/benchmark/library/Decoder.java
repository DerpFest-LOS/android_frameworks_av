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

package com.android.media.benchmark.library;

import android.view.Surface;

import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaCodec.BufferInfo;
import android.media.MediaFormat;
import android.util.Log;

import androidx.annotation.NonNull;

import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

import com.android.media.benchmark.library.IBufferXfer;

public class Decoder implements IBufferXfer.IReceiveBuffer {
    private static final String TAG = "Decoder";
    private static final boolean DEBUG = false;
    private static final int kQueueDequeueTimeoutUs = 1000;

    protected final Object mLock = new Object();
    protected MediaCodec mCodec;
    protected int mExtraFlags = 0;
    protected Surface mSurface = null;
    protected boolean mRender = false;
    protected ArrayList<BufferInfo> mInputBufferInfo;
    protected Stats mStats;
    protected String mMime;

    protected boolean mSawInputEOS;
    protected boolean mSawOutputEOS;
    protected boolean mSignalledError;

    protected int mNumInFramesProvided;
    protected int mNumInFramesRequired;

    protected int mNumOutputFrame;
    protected int mIndex;

    protected boolean mUseFrameReleaseQueue = false;

    protected ArrayList<ByteBuffer> mInputBuffer;
    protected FileOutputStream mOutputStream;
    protected FrameReleaseQueue mFrameReleaseQueue = null;
    protected IBufferXfer.ISendBuffer mIBufferSend = null;

    /* success for decoder */
    public static final int DECODE_SUCCESS = 0;
    /* some error happened during decoding */
    public static final int DECODE_DECODER_ERROR = -1;
    /* error while creating a decoder */
    public static final int DECODE_CREATE_ERROR = -2;
    public Decoder() { mStats = new Stats(); }
    public Stats getStats() { return mStats; };
    @Override
    public boolean receiveBuffer(IBufferXfer.BufferXferInfo info) {
        MediaCodec codec = (MediaCodec)info.obj;
        if (info.isComplete) {
            codec.releaseOutputBuffer(info.idx, mRender);
        }
        return true;
    }
    @Override
    public boolean connect(IBufferXfer.ISendBuffer receiver) {
        Log.d(TAG,"Setting interface of the sender");
        mIBufferSend = receiver;
        return true;
    }

    public void setExtraConfigureFlags(int flags) {
        this.mExtraFlags = flags;
    }

    /**
     * Setup of decoder
     *
     * @param outputStream Will dump the output in this stream if not null.
     */
    public void setupDecoder(FileOutputStream outputStream) {
        mSignalledError = false;
        mOutputStream = outputStream;
    }

    /*
     * This can be used to setup audio decoding, simulating audio playback.
     */
    public void setupDecoder(
            boolean render, boolean useFrameReleaseQueue, int numInFramesRequired) {
        mRender = render;
        mUseFrameReleaseQueue = useFrameReleaseQueue;
        mNumInFramesRequired = numInFramesRequired;
        mSignalledError = false;
        setupDecoder(null);
    }

    public void setupDecoder(Surface surface, boolean render,
            boolean useFrameReleaseQueue, int frameRate) {
        setupDecoder(surface, render, useFrameReleaseQueue, frameRate, -1);
    }

    public void setupDecoder(Surface surface, boolean render,
            boolean useFrameReleaseQueue, int frameRate, int numInFramesRequired) {
        mSignalledError = false;
        mOutputStream = null;
        mSurface = surface;
        mRender = render;
        mUseFrameReleaseQueue = useFrameReleaseQueue;
        if (mUseFrameReleaseQueue) {
            Log.i(TAG, "Using FrameReleaseQueue with frameRate " + frameRate);
            mFrameReleaseQueue = new FrameReleaseQueue(mRender, frameRate);
        }
        mNumInFramesRequired = numInFramesRequired;
        Log.i(TAG, "Decoding " + mNumInFramesRequired + " frames");
    }

    private MediaCodec createCodec(String codecName, MediaFormat format) throws IOException {
        mMime = format.getString(MediaFormat.KEY_MIME);
        try {
            MediaCodec codec;
            if (codecName.isEmpty()) {
                Log.i(TAG, "File mime type: " + mMime);
                if (mMime != null) {
                    codec = MediaCodec.createDecoderByType(mMime);
                    Log.i(TAG, "Decoder created for mime type " + mMime);
                    return codec;
                } else {
                    Log.e(TAG, "Mime type is null, please specify a mime type to create decoder");
                    return null;
                }
            } else {
                codec = MediaCodec.createByCodecName(codecName);
                Log.i(TAG, "Decoder created with codec name: " + codecName + " mime: " + mMime);
                return codec;
            }
        } catch (IllegalArgumentException ex) {
            ex.printStackTrace();
            Log.e(TAG, "Failed to create decoder for " + codecName + " mime:" + mMime);
            return null;
        }
    }

    protected void setCallback(MediaCodec codec) {
        codec.setCallback(new MediaCodec.Callback() {
        @Override
        public void onInputBufferAvailable(
                @NonNull MediaCodec mediaCodec, int inputBufferId) {
            try {
                mStats.addInputTime();
                onInputAvailable(inputBufferId, mediaCodec);
            } catch (Exception e) {
                e.printStackTrace();
                Log.e(TAG, e.toString());
            }
        }

        @Override
        public void onOutputBufferAvailable(@NonNull MediaCodec mediaCodec,
                int outputBufferId, @NonNull MediaCodec.BufferInfo bufferInfo) {
            mStats.addOutputTime();
            onOutputAvailable(mediaCodec, outputBufferId, bufferInfo);
            if (mSawOutputEOS) {
                synchronized (mLock) { mLock.notify(); }
            }
        }

        @Override
        public void onOutputFormatChanged(
                @NonNull MediaCodec mediaCodec, @NonNull MediaFormat format) {
            Log.i(TAG, "Output format changed. Format: " + format.toString());
            if (mUseFrameReleaseQueue
                    && mFrameReleaseQueue == null && mMime.startsWith("audio/")) {
                // start a frame release thread for this configuration.
                int bytesPerSample = AudioFormat.getBytesPerSample(
                        format.getInteger(MediaFormat.KEY_PCM_ENCODING,
                                AudioFormat.ENCODING_PCM_16BIT));
                int sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                int channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                mFrameReleaseQueue = new FrameReleaseQueue(
                        mRender, sampleRate, channelCount, bytesPerSample);
                mFrameReleaseQueue.setMediaCodec(mCodec);
            }
        }

        @Override
        public void onError(
                @NonNull MediaCodec mediaCodec, @NonNull MediaCodec.CodecException e) {
            mSignalledError = true;
            Log.e(TAG, "Codec Error: " + e.toString());
            e.printStackTrace();
            synchronized (mLock) { mLock.notify(); }
        }
    });


    }

    /**
     * Decodes the given input buffer,
     * provided valid list of buffer info and format are passed as inputs.
     *
     * @param inputBuffer     Decode the provided list of ByteBuffers
     * @param inputBufferInfo List of buffer info corresponding to provided input buffers
     * @param asyncMode       Will run on async implementation if true
     * @param format          For creating the decoder if codec name is empty and configuring it
     * @param codecName       Will create the decoder with codecName
     * @return DECODE_SUCCESS if decode was successful, DECODE_DECODER_ERROR for fail,
     *         DECODE_CREATE_ERROR for decoder not created
     * @throws IOException if the codec cannot be created.
     */
    public int decode(@NonNull List<ByteBuffer> inputBuffer,
            @NonNull List<BufferInfo> inputBufferInfo, final boolean asyncMode,
            @NonNull MediaFormat format, String codecName)
            throws IOException, InterruptedException {
        mInputBuffer = new ArrayList<>(inputBuffer.size());
        mInputBuffer.addAll(inputBuffer);
        mInputBufferInfo = new ArrayList<>(inputBufferInfo.size());
        mInputBufferInfo.addAll(inputBufferInfo);
        mSawInputEOS = false;
        mSawOutputEOS = false;
        mNumOutputFrame = 0;
        mIndex = 0;
        mNumInFramesProvided = 0;
        if (mNumInFramesRequired < 0) {
            mNumInFramesRequired = mInputBuffer.size();
        }
        long sTime = mStats.getCurTime();
        mCodec = createCodec(codecName, format);
        if (mCodec == null) {
            return DECODE_CREATE_ERROR;
        }
        if (mFrameReleaseQueue != null) {
            mFrameReleaseQueue.setMediaCodec(mCodec);
            mFrameReleaseQueue.setMime(mMime);
        }

        if (asyncMode) {
            setCallback(mCodec);
        }
        if (DEBUG) {
            Log.d(TAG, "Media Format : " + format.toString());
        }
        mCodec.configure(format, mSurface, null, mExtraFlags);

        mCodec.start();
        Log.i(TAG, "Codec started async mode ?  " + asyncMode);
        long eTime = mStats.getCurTime();
        mStats.setInitTime(mStats.getTimeDiff(sTime, eTime));
        mStats.setStartTime();
        if (asyncMode) {
            try {
                synchronized (mLock) {
                    while (!mSawOutputEOS && !mSignalledError) {
                        mLock.wait();
                    }
                    if (mSignalledError) {
                        return DECODE_DECODER_ERROR;
                    }
                }
            } catch (InterruptedException e) {
                Log.e(TAG, "Error in waiting");
                throw e;
            }
        } else {
            while (!mSawOutputEOS && !mSignalledError) {
                /* Queue input data */
                if (!mSawInputEOS) {
                    int inputBufferId = mCodec.dequeueInputBuffer(kQueueDequeueTimeoutUs);
                    if (inputBufferId < 0 && inputBufferId != MediaCodec.INFO_TRY_AGAIN_LATER) {
                        Log.e(TAG,
                                "MediaCodec.dequeueInputBuffer "
                                        + " returned invalid index : " + inputBufferId);
                        return -1;
                    }
                    mStats.addInputTime();
                    onInputAvailable(inputBufferId, mCodec);
                }
                /* Dequeue output data */
                BufferInfo outputBufferInfo = new BufferInfo();
                int outputBufferId =
                        mCodec.dequeueOutputBuffer(outputBufferInfo, kQueueDequeueTimeoutUs);
                if (outputBufferId < 0) {
                    if (outputBufferId == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        MediaFormat outFormat = mCodec.getOutputFormat();
                        Log.i(TAG, "Output format changed. Format: " + outFormat.toString());
                    } else if (outputBufferId == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
                        Log.i(TAG, "Ignoring deprecated flag: INFO_OUTPUT_BUFFERS_CHANGED");
                    } else if (outputBufferId != MediaCodec.INFO_TRY_AGAIN_LATER) {
                        Log.e(TAG,
                                "MediaCodec.dequeueOutputBuffer"
                                        + " returned invalid index " + outputBufferId);
                        return DECODE_DECODER_ERROR;
                    }
                } else {
                    mStats.addOutputTime();
                    if (DEBUG) {
                        Log.d(TAG, "Dequeue O/P buffer with BufferID " + outputBufferId);
                    }
                    onOutputAvailable(mCodec, outputBufferId, outputBufferInfo);
                }
            }
        }
        if (mFrameReleaseQueue != null) {
            Log.i(TAG, "Ending FrameReleaseQueue");
            mFrameReleaseQueue.stopFrameRelease();
        }
        mInputBuffer.clear();
        mInputBufferInfo.clear();
        return DECODE_SUCCESS;
    }

    /**
     * Stops the codec and releases codec resources.
     */
    public void deInitCodec() {
        long sTime = mStats.getCurTime();
        if (mCodec != null) {
            mCodec.stop();
            mCodec.release();
            mCodec = null;
        }
        long eTime = mStats.getCurTime();
        mStats.setDeInitTime(mStats.getTimeDiff(sTime, eTime));
    }

    /**
     * Prints out the statistics in the information log
     *
     * @param inputReference The operation being performed, in this case decode
     * @param componentName  Name of the component/codec
     * @param mode           The operating mode: Sync/Async
     * @param durationUs     Duration of the clip in microseconds
     * @param statsFile      The output file where the stats data is written
     */
    public void dumpStatistics(String inputReference, String componentName, String mode,
            long durationUs, String statsFile) throws IOException {
        String operation = "decode";
        mStats.dumpStatistics(
                inputReference, operation, componentName, mode, durationUs, statsFile);
    }

    /**
     * Resets the stats
     */
    public void resetDecoder() { mStats.reset(); }

    /**
     * Returns the format of the output buffers
     */
    public MediaFormat getFormat() {
        return mCodec.getOutputFormat();
    }

    protected void onInputAvailable(int inputBufferId, MediaCodec mediaCodec) {
        if (inputBufferId >= 0) {
            ByteBuffer inputCodecBuffer = mediaCodec.getInputBuffer(inputBufferId);
            BufferInfo bufInfo;
            if (mNumInFramesProvided >= mNumInFramesRequired) {
                Log.i(TAG, "Input frame limit reached provided: " + mNumInFramesProvided);
                mIndex = mInputBufferInfo.size() - 1;
                bufInfo = mInputBufferInfo.get(mIndex);
                if ((bufInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0) {
                    Log.e(TAG, "Error in EOS flag for Decoder");
                }
            }
            bufInfo = mInputBufferInfo.get(mIndex);
            mSawInputEOS = (bufInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
            inputCodecBuffer.put(mInputBuffer.get(mIndex).array());
            mNumInFramesProvided++;
            mIndex = mNumInFramesProvided % (mInputBufferInfo.size() - 1);
            if (mSawInputEOS) {
                Log.i(TAG, "Saw input EOS");
            }
            mStats.addFrameSize(bufInfo.size);
            mediaCodec.queueInputBuffer(inputBufferId, bufInfo.offset, bufInfo.size,
                    bufInfo.presentationTimeUs, bufInfo.flags);
            if (DEBUG) {
                Log.d(TAG,
                        "Codec Input: "
                                + "flag = " + bufInfo.flags + " timestamp = "
                                + bufInfo.presentationTimeUs + " size = " + bufInfo.size);
            }
        }
    }

    protected void onOutputAvailable(
            MediaCodec mediaCodec, int outputBufferId, BufferInfo outputBufferInfo) {
        if (mSawOutputEOS || outputBufferId < 0) {
            return;
        }
        mNumOutputFrame++;
        if (DEBUG) {
            Log.d(TAG,
                    "In OutputBufferAvailable ,"
                            + " output frame number = " + mNumOutputFrame
                            + " timestamp = " + outputBufferInfo.presentationTimeUs
                            + " size = " + outputBufferInfo.size);
        }
        if (mOutputStream != null) {
            try {
                ByteBuffer outputBuffer = mediaCodec.getOutputBuffer(outputBufferId);
                byte[] bytesOutput = new byte[outputBuffer.remaining()];
                outputBuffer.get(bytesOutput);
                mOutputStream.write(bytesOutput);
            } catch (IOException e) {
                e.printStackTrace();
                Log.d(TAG, "Error Dumping File: Exception " + e.toString());
            }
        }
        if (mFrameReleaseQueue != null) {
            if (mMime.startsWith("audio/")) {
                try {
                    ByteBuffer outputBuffer = mediaCodec.getOutputBuffer(outputBufferId);
                    mFrameReleaseQueue.pushFrame(outputBufferId, outputBuffer.remaining());
                } catch (Exception e) {
                    Log.d(TAG, "Error in getting MediaCodec buffer" + e.toString());
                }
            } else {
                mFrameReleaseQueue.pushFrame(mNumOutputFrame, outputBufferId,
                                                outputBufferInfo.presentationTimeUs);
            }
        } else if (mIBufferSend != null) {
            IBufferXfer.BufferXferInfo info = new IBufferXfer.BufferXferInfo();
            info.buf = mediaCodec.getOutputBuffer(outputBufferId);
            info.idx = outputBufferId;
            info.obj = mediaCodec;
            info.bytesRead = outputBufferInfo.size;
            info.presentationTimeUs = outputBufferInfo.presentationTimeUs;
            info.flag = outputBufferInfo.flags;
            mIBufferSend.sendBuffer(this, info);
        } else {
            mediaCodec.releaseOutputBuffer(outputBufferId, mRender);
        }
        mSawOutputEOS = (outputBufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
        if (mSawOutputEOS) {
            Log.i(TAG, "Saw output EOS");
        }
    }
}
