#include <cstddef>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>
#include "log.h"
#include "scoped_exit.hpp"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

using DecodeCb = std::function<void(AVFrame*)>;

std::string av_errno_to_string(int err)
{
    char buff[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buff, AV_ERROR_MAX_STRING_SIZE);
    return buff;
}

struct StreamContext
{
    int stream_index = -1;
    AVStream* stream = nullptr;
    AVCodecContext* dec_ctx = nullptr;
};

struct InputContext
{
    AVPacket* pkg = nullptr;
    AVFrame* yuv_frame = nullptr;
    AVFormatContext* fmt_ctx = nullptr;
    std::vector<std::shared_ptr<StreamContext>> streams;
};

std::shared_ptr<InputContext> create_input_context(const std::string& file)
{
    auto input_fn = [](InputContext* c)
    {
        avformat_free_context(c->fmt_ctx);
        av_packet_free(&c->pkg);
        av_frame_free(&c->yuv_frame);
    };

    std::shared_ptr<InputContext> input_ctx(new InputContext, input_fn);
    // clang-format off
    // clang-format on
    input_ctx->fmt_ctx = avformat_alloc_context();
    const AVInputFormat* iformat = nullptr;
    int ret = avformat_open_input(&input_ctx->fmt_ctx, file.data(), iformat, nullptr);
    if (ret != 0)
    {
        LOG_ERROR << "avformat open file " << file << " failed " << av_errno_to_string(ret);
        return nullptr;
    }
    av_dump_format(input_ctx->fmt_ctx, 0, file.data(), 0);

    auto delete_fn = [](StreamContext* c)
    {
        if (c->dec_ctx != nullptr)
        {
            avcodec_free_context(&c->dec_ctx);
        }
    };

    for (int i = 0; i < input_ctx->fmt_ctx->nb_streams; i++)
    {
        std::shared_ptr<StreamContext> stream_ctx(new StreamContext, delete_fn);
        stream_ctx->stream_index = i;
        stream_ctx->stream = input_ctx->fmt_ctx->streams[i];
        const auto* codec = avcodec_find_decoder(stream_ctx->stream->codecpar->codec_id);
        stream_ctx->dec_ctx = avcodec_alloc_context3(codec);
        int err = avcodec_parameters_to_context(stream_ctx->dec_ctx, stream_ctx->stream->codecpar);
        if (err < 0)
        {
            LOG_ERROR << "avcodec parameters to context failed " << av_errno_to_string(err);
            continue;
        }

        stream_ctx->dec_ctx->pkt_timebase = stream_ctx->stream->time_base;
        if (avcodec_open2(stream_ctx->dec_ctx, codec, nullptr) < 0)
        {
            LOG_ERROR << "open codec failed input stream index " << stream_ctx->stream_index;
            continue;
        }
        input_ctx->streams.push_back(stream_ctx);
    }
    input_ctx->pkg = av_packet_alloc();
    input_ctx->yuv_frame = av_frame_alloc();
    return input_ctx;
}

void decode_package(AVCodecContext* codec_ctx, AVPacket* pkg, AVFrame* frame, const DecodeCb& cb)
{
    int ret = avcodec_send_packet(codec_ctx, pkg);
    if (ret == AVERROR(EAGAIN))
    {
        LOG_ERROR << "Receive_frame and send_packet both returned EAGAIN, which is an API violation";
    }
    else if (ret < 0)
    {
        LOG_ERROR << "avcodec_send_packet failed " << av_errno_to_string(ret);
        return;
    }
    while (ret >= 0)
    {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return;
        }
        if (ret < 0)
        {
            LOG_DEBUG << "avcodec_receive_frame failed " << av_errno_to_string(ret);
            return;
        }
        if (cb)
        {
            cb(frame);
        }
    }
}

static void write_to_file(const std::string& filename, const uint8_t* data, int size)
{
    FILE* fp = fopen(filename.data(), "ab+");
    if (fp != nullptr)
    {
        fwrite(data, 1, size, fp);
        fclose(fp);
    }
}

static void write_frame_to_file(const std::string& filename, AVFrame* frame)
{
    for (int j = 0; j < frame->height; j++)
    {
        write_to_file(filename, frame->data[0] + static_cast<ptrdiff_t>(j * frame->linesize[0]), frame->width);
    }
    for (int j = 0; j < frame->height / 2; j++)
    {
        write_to_file(filename, frame->data[1] + static_cast<ptrdiff_t>(j * frame->linesize[1]), frame->width / 2);
    }
    for (int j = 0; j < frame->height / 2; j++)
    {
        write_to_file(filename, frame->data[2] + static_cast<ptrdiff_t>(j * frame->linesize[2]), frame->width / 2);
    }
}

std::vector<uint8_t> read_file_to_buffer(const std::string& filename)
{
    FILE* fp = fopen(filename.data(), "rb");
    if (fp == nullptr)
    {
        return {};
    }

    fseek(fp, 0, SEEK_END);
    uint32_t filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> bytes(filesize);

    fread(bytes.data(), 1, filesize, fp);
    fclose(fp);
    return bytes;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <in_h264_file> <out_yuv_file>\n";
        return 1;
    }

    auto input_ctx = create_input_context(argv[1]);
    if (!input_ctx)
    {
        return -1;
    }

    uint32_t frame_count = 0;
    uint32_t packet_count = 0;

    auto decode_cb = [&](AVFrame* frame)
    {
        LOG_DEBUG << "frame index " << frame_count++ << " coded_picture_number " << frame->coded_picture_number
                  << " pix_fmt " << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) << " pkt pos "
                  << frame->pkt_pos << " pkt size " << frame->pkt_size;
        write_frame_to_file(argv[2], frame);
    };
    while (av_read_frame(input_ctx->fmt_ctx, input_ctx->pkg) == 0)
    {
        LOG_DEBUG << "packet index " << packet_count++ << " pkt ptos " << input_ctx->pkg->pos << " pkt size "
                  << input_ctx->pkg->size;
        // clang-format off
        decode_package(input_ctx->streams[input_ctx->pkg->stream_index]->dec_ctx, input_ctx->pkg, input_ctx->yuv_frame, decode_cb);
        // clang-format on
        av_packet_unref(input_ctx->pkg);
    }
    input_ctx->pkg->data = nullptr;
    input_ctx->pkg->size = 0;

    // clang-format off
    decode_package( input_ctx->streams[input_ctx->pkg->stream_index]->dec_ctx, input_ctx->pkg, input_ctx->yuv_frame, decode_cb);
    // clang-format on

    LOG_DEBUG << "Hello World";

    return 0;
}
