#include <cstddef>
#include <utility>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <functional>
#include <chrono>
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
    const uint32_t filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> bytes(filesize);

    fread(bytes.data(), 1, filesize, fp);
    fclose(fp);
    return bytes;
}

class ff_decoder
{
   public:
    explicit ff_decoder(std::string filename, DecodeCb cb) : cb_(cb), filename(std::move(filename)) {}
    ~ff_decoder() = default;

   public:
    void run()
    {
        open();
        decode();
        close();
    }

   private:
    void open()
    {
        AVInputFormat* iformat = nullptr;
        int ret = avformat_open_input(&fmt_ctx, filename.data(), iformat, nullptr);
        if (ret != 0)
        {
            LOG_ERROR << "avformat open file " << filename << " failed " << av_errno_to_string(ret);
            return;
        }
        av_dump_format(fmt_ctx, 0, filename.data(), 0);
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0)
        {
            LOG_ERROR << "cannot find input stream information";
            return;
        }
        const AVCodec* dec;
        // av_find_best_stream()
        const int video_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
        if (video_index < 0)
        {
            LOG_ERROR << "file " << filename << " find video stream failed " << av_errno_to_string(ret);
            return;
        }
        dec_ctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_index]->codecpar);
        ret = avcodec_open2(dec_ctx, dec, nullptr);
        if (ret < 0)
        {
            LOG_ERROR << "file " << filename << " open codec failed " << av_errno_to_string(ret);
            return;
        }
        video_stream_index = video_index;
    }
    void decode()
    {
        auto* pkg = av_packet_alloc();
        while (av_read_frame(fmt_ctx, pkg) == 0)
        {
            if (pkg->stream_index == video_stream_index)
            {
                packet_count++;
                auto* stream = fmt_ctx->streams[pkg->stream_index];
                LOG_DEBUG << "1 packet index " << packet_count << " pkt ptos " << pkg->pos << " pkt size " << pkg->size
                          << " pts " << pkg->pts << " dts " << pkg->dts << " pts "
                          << pkg->pts * av_q2d(stream->time_base);

                auto video_time_base = av_inv_q(dec_ctx->framerate);
                av_packet_rescale_ts(pkg, stream->time_base, video_time_base);
                LOG_DEBUG << "2 packet index " << packet_count << " pkt ptos " << pkg->pos << " pkt size " << pkg->size
                          << " pts " << pkg->pts << " dts " << pkg->dts;

                decode_package(pkg);
            }
            av_packet_unref(pkg);
        }
        av_packet_free(&pkg);
    }
    void decode_package(AVPacket* pkg)
    {
        int ret = avcodec_send_packet(dec_ctx, pkg);
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
            auto* frame = av_frame_alloc();
            DEFER(av_frame_free(&frame));
            ret = avcodec_receive_frame(dec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                return;
            }
            if (ret < 0)
            {
                LOG_DEBUG << "avcodec_receive_frame failed " << av_errno_to_string(ret);
                return;
            }

            process_yuv_frame(frame);
        }
    }
    void process_yuv_frame(AVFrame* frame)
    {
        if (cb_)
        {
            cb_(frame);
        }
    }
    void close()
    {
        avformat_free_context(fmt_ctx);
        avcodec_free_context(&dec_ctx);
    }

   private:
    DecodeCb cb_;
    int video_stream_index = -1;
    std::string filename;
    uint32_t packet_count = 0;
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
};

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <in_h264_file> <out_yuv_file>\n";
        return 1;
    }

    auto decode_cb = [&](AVFrame* frame)
    {
        LOG_DEBUG << "1 coded_picture_number " << frame->coded_picture_number << " pix_fmt "
                  << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) << " pkt pos " << frame->pkt_pos
                  << " pkt size " << frame->pkt_size << " pts " << frame->pts;
        write_frame_to_file(argv[2], frame);
    };

    ff_decoder decoder(argv[1], decode_cb);
    decoder.run();
    LOG_DEBUG << "Hello World";

    return 0;
}
