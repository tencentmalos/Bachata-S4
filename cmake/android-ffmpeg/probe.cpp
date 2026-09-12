// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
extern "C" int ffmpeg_media_check(const char* path) {
    int count = 0, failed = 0;
    auto check = [&](bool ok, const char* name) {
        ++count;
        failed += !ok;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    };
    check(avcodec_version() == LIBAVCODEC_VERSION_INT &&
              avformat_version() == LIBAVFORMAT_VERSION_INT &&
              avutil_version() == LIBAVUTIL_VERSION_INT,
          "header_library_versions");
    std::printf("FFmpeg=%s codec=%u format=%u util=%u\n", av_version_info(), avcodec_version(),
                avformat_version(), avutil_version());
    AVFilterGraph* graph = avfilter_graph_alloc();
    check(graph != nullptr, "avfilter_graph");
    avfilter_graph_free(&graph);
    check(avcodec_find_decoder(AV_CODEC_ID_MP3) && avcodec_find_decoder(AV_CODEC_ID_AAC),
          "audio_decoders");
    AVFormatContext* format = nullptr;
    const bool opened = avformat_open_input(&format, path, nullptr, nullptr) >= 0;
    check(opened && avformat_find_stream_info(format, nullptr) >= 0, "avformat_demux");
    const int stream =
        opened ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    AVCodecContext* codec = nullptr;
    if (stream >= 0) {
        codec = avcodec_alloc_context3(
            avcodec_find_decoder(format->streams[stream]->codecpar->codec_id));
        if (codec) {
            avcodec_parameters_to_context(codec, format->streams[stream]->codecpar);
            codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        }
    }
    const bool ready = codec && avcodec_open2(codec, codec->codec, nullptr) >= 0;
    check(ready, "decoder_open");
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool decoded = false, opaque = false, scaled = false;
    int marker = 42;
    if (ready && packet && frame) {
        while (av_read_frame(format, packet) >= 0 && !decoded) {
            if (packet->stream_index == stream) {
                packet->opaque = &marker;
                if (avcodec_send_packet(codec, packet) >= 0 &&
                    avcodec_receive_frame(codec, frame) >= 0)
                    decoded = true;
            }
            av_packet_unref(packet);
        }
        if (!decoded && avcodec_send_packet(codec, nullptr) >= 0)
            decoded = avcodec_receive_frame(codec, frame) >= 0;
        if (decoded) {
            opaque = frame->opaque == &marker;
            SwsContext* sws = sws_getContext(frame->width, frame->height,
                                             static_cast<AVPixelFormat>(frame->format),
                                             frame->width, frame->height, AV_PIX_FMT_RGBA,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws) {
                std::vector<unsigned char> pixels(frame->width * frame->height * 4);
                unsigned char* dst[] = {pixels.data(), nullptr, nullptr, nullptr};
                int stride[] = {frame->width * 4, 0, 0, 0};
                scaled = sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst,
                                   stride) == frame->height;
                sws_freeContext(sws);
            }
        }
    }
    check(decoded, "h264_decode");
    check(opaque, "packet_frame_copy_opaque");
    check(scaled, "swscale_rgba");
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    SwrContext* swr = nullptr;
    AVChannelLayout in_layout = AV_CHANNEL_LAYOUT_MONO, out_layout = AV_CHANNEL_LAYOUT_STEREO;
    bool resampled = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, 48000, &in_layout,
                                         AV_SAMPLE_FMT_S16, 24000, 0, nullptr) >= 0 &&
                     swr_init(swr) >= 0;
    if (resampled) {
        short input[64]{};
        float output[256]{};
        const unsigned char* in[] = {reinterpret_cast<const unsigned char*>(input)};
        unsigned char* out[] = {reinterpret_cast<unsigned char*>(output)};
        resampled = swr_convert(swr, out, 128, in, 64) > 0;
    }
    check(resampled, "swresample_pcm");
    swr_free(&swr);
    std::printf("checks=%d failures=%d\n", count, failed);
    return failed ? 1 : 0;
}
