#include <cstddef>
#include <utility>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>
#include <cassert>
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
using EncodeCb = std::function<void(AVPacket*)>;

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
    uint32_t filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> bytes(filesize);

    fread(bytes.data(), 1, filesize, fp);
    fclose(fp);
    return bytes;
}

class ff_encode
{
   public:
    ff_encode(std::string codec_name, EncodeCb cb) : codec_name_(std::move(codec_name)), cb_(std::move(cb)) {}
    ~ff_encode() = default;

   public:
    void encode(AVFrame* frame)
    {
        if (encoder_ctx == nullptr)
        {
            open_encoder(frame);
        }
        if (encoder_ctx != nullptr)
        {
            do_encode(frame);
        }
    }
    void flush() { close_encoder(); }

   private:
    void do_encode(AVFrame* frame)
    {
        int ret = avcodec_send_frame(encoder_ctx, frame);
        if (ret < 0)
        {
            LOG_ERROR << "encode frame failed " << av_errno_to_string(ret);
            return;
        }

        while (ret >= 0)
        {
            ret = avcodec_receive_packet(encoder_ctx, pkg_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            if (ret < 0)
            {
                break;
            }
            if (cb_)
            {
                cb_(pkg_);
            }
        }
    }
    void open_encoder(AVFrame* frame)
    {
        const auto* codec = avcodec_find_encoder_by_name(codec_name_.data());
        auto* tmp_codec_ctx = avcodec_alloc_context3(codec);
        if (tmp_codec_ctx == nullptr)
        {
            return;
        }
        auto clean_up = ScopedExit::make_scoped_exit([&tmp_codec_ctx]() { avcodec_free_context(&tmp_codec_ctx); });
        tmp_codec_ctx->height = frame->height;
        tmp_codec_ctx->width = frame->width;
        tmp_codec_ctx->sample_aspect_ratio = frame->sample_aspect_ratio;
        if (codec->pix_fmts != nullptr)
        {
            tmp_codec_ctx->pix_fmt = codec->pix_fmts[0];
        }
        else
        {
            tmp_codec_ctx->pix_fmt = static_cast<AVPixelFormat>(frame->format);
        }
        // 25 fps
        tmp_codec_ctx->time_base = (AVRational){25, 1};
        tmp_codec_ctx->framerate = (AVRational){25, 1};

        const int ret = avcodec_open2(tmp_codec_ctx, codec, nullptr);
        if (ret < 0)
        {
            LOG_ERROR << "open " << codec_name_ << " codec failed " << av_errno_to_string(ret);
            return;
        }
        clean_up.cancel();
        encoder_ctx = tmp_codec_ctx;
        pkg_ = av_packet_alloc();
    }
    void close_encoder()
    {
        avcodec_free_context(&encoder_ctx);
        av_packet_free(&pkg_);
    }

   private:
    std::string codec_name_;
    EncodeCb cb_;
    uint32_t packet_count = 0;
    AVPacket* pkg_ = nullptr;
    AVCodecContext* encoder_ctx = nullptr;
};
class ff_decoder
{
   public:
    explicit ff_decoder(std::string filename, DecodeCb cb) : cb_(std::move(cb)), filename(std::move(filename)) {}
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
                // auto* stream = fmt_ctx->streams[pkg->stream_index];
                // LOG_DEBUG << "1 packet index " << packet_count << " pkt ptos " << pkg->pos << " pkt size " <<
                // pkg->size
                //<< " pts " << pkg->pts << " dts " << pkg->dts << " pts "
                //<< pkg->pts * av_q2d(stream->time_base);

                // auto video_time_base = av_inv_q(dec_ctx->framerate);
                // av_packet_rescale_ts(pkg, stream->time_base, video_time_base);
                // LOG_DEBUG << "2 packet index " << packet_count << " pkt ptos " << pkg->pos << " pkt size " <<
                // pkg->size
                //<< " pts " << pkg->pts << " dts " << pkg->dts;

                decode_package(pkg);
            }
            av_packet_unref(pkg);
        }
        pkg->data = nullptr;
        pkg->size = 0;
        av_packet_unref(pkg);
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
        std::cerr << "Usage: " << argv[0] << " <in_file> <out_file>\n";
        return 1;
    }
    uint32_t packet_count = 0;
    auto encode_cb = [&](AVPacket* pkg)
    {
        packet_count++;
        LOG_DEBUG << "write " << packet_count << " packet size " << pkg->size;

        write_to_file(argv[2], pkg->data, pkg->size);
    };

    ff_encode encoder("libx264", encode_cb);

    auto decode_cb = [&](AVFrame* frame)
    {
        LOG_DEBUG << "1 coded_picture_number " << frame->coded_picture_number << " pix_fmt "
                  << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) << " pkt pos " << frame->pkt_pos
                  << " pkt size " << frame->pkt_size << " pts " << frame->pts;
        encoder.encode(frame);
    };
    ff_decoder decoder(argv[1], decode_cb);
    decoder.run();

    LOG_DEBUG << "Hello World";

    return 0;
}
