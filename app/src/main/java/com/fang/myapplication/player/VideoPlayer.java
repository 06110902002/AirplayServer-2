package com.fang.myapplication.player;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import com.fang.myapplication.model.NALPacket;

import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;

public class VideoPlayer {
    private static final String TAG = "VideoPlayer";
    private static final String MIME_TYPE = MediaFormat.MIMETYPE_VIDEO_AVC;
    private final int mVideoWidth = 540;
    private final int mVideoHeight = 960;
    private MediaCodec mDecoder = null;
    private final Surface mSurface;
    private BlockingQueue<NALPacket> packets = new LinkedBlockingQueue<>(1);
    private final HandlerThread mDecodeThread = new HandlerThread("VideoDecoder");

    private final MediaCodec.Callback mDecoderCallback = new MediaCodec.Callback() {
        @Override
        public void onInputBufferAvailable(MediaCodec codec, int index) {
            try {
                NALPacket packet = packets.take();
                codec.getInputBuffer(index).put(packet.nalData);
                mDecoder.queueInputBuffer(index, 0, packet.nalData.length, packet.pts, 0);
            } catch (InterruptedException e) {
                throw new IllegalStateException("Interrupted when is waiting");
            } catch (IllegalStateException e) {
                e.printStackTrace();
            }
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec codec, int index, MediaCodec.BufferInfo info) {
            try {
                codec.releaseOutputBuffer(index, true);
            } catch (IllegalStateException e) {
                e.printStackTrace();
            }
        }

        @Override
        public void onError(MediaCodec codec, MediaCodec.CodecException e) {
            Log.e(TAG, "Decode error", e);
        }

        @Override
        public void onOutputFormatChanged(MediaCodec codec, MediaFormat format) {

        }
    };

    public VideoPlayer(Surface surface, int width, int heigth) {
//        this.mVideoWidth=width;
//        this.mVideoHeight=heigth;
        mSurface = surface;
    }

    public void initDecoder() {
        mDecodeThread.start();
        try {
            Log.i(TAG, "initDecoder: mVideoWidth=" + mVideoWidth + "---mVideoHeight=" + mVideoHeight);
            mDecoder = MediaCodec.createDecoderByType(MIME_TYPE);
            MediaFormat format = MediaFormat.createVideoFormat(MIME_TYPE, mVideoWidth, mVideoHeight);
            mDecoder.configure(format, mSurface, null, 0);
            mDecoder.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                mDecoder.setCallback(mDecoderCallback, new Handler(mDecodeThread.getLooper()));
            }
            mDecoder.start();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public void addPacker(NALPacket nalPacket) {
        try {
            packets.put(nalPacket);
        } catch (InterruptedException e) {
            Log.e(TAG, "run: put error:", e);
        }
    }

    public void start() {
        initDecoder();
    }

    public void stopVideoPlay() {
        if (mDecoder != null) {
            mDecoder.stop();
            mDecoder.release();
            mDecoder = null;
        }
        mDecodeThread.quit();
        packets.clear();
    }
}
