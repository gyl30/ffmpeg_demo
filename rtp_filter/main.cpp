#include <cstddef>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>
#include <string_view>
#include "log.h"
#include "scoped_exit.hpp"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

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
        LOG_ERROR << av_errno_to_string(errno);
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

struct StreamContext
{
    std::string filename;
    std::string filter_describe;
    AVPacket* pkg = nullptr;
    AVFrame* yuv_frame = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVCodecParserContext* codec_parse_ctx = nullptr;

    AVPacket* encode_pkg = nullptr;
    AVCodecContext* encode_codec_ctx = nullptr;
    // filter
    AVFilterGraph* graph = nullptr;
    AVFrame* filter_frame = nullptr;
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;
};

using DecodeCb = std::function<void(std::shared_ptr<StreamContext>&, AVFrame*)>;
using EncodeCb = std::function<void(std::shared_ptr<StreamContext>&, AVPacket*)>;
using FilterCb = std::function<void(std::shared_ptr<StreamContext>&, AVFrame*)>;

void destroy_steam_encode_codec(StreamContext* ctx)
{
    if (ctx->encode_codec_ctx != nullptr)
    {
        avcodec_free_context(&ctx->encode_codec_ctx);
    }
    if (ctx->encode_pkg != nullptr)
    {
        av_packet_free(&ctx->encode_pkg);
    }
}
void destroy_steam_decode_codec(StreamContext* ctx)
{
    if (ctx->codec_ctx != nullptr)
    {
        avcodec_free_context(&ctx->codec_ctx);
    }
    if (ctx->codec_parse_ctx != nullptr)
    {
        av_parser_close(ctx->codec_parse_ctx);
    }
    if (ctx->yuv_frame != nullptr)
    {
        av_frame_free(&ctx->yuv_frame);
    }
    if (ctx->pkg != nullptr)
    {
        av_packet_free(&ctx->pkg);
    }
}
void destroy_steam_filter(StreamContext* ctx)
{
    // filter
    if (ctx->graph != nullptr)
    {
        avfilter_graph_free(&ctx->graph);
        ctx->graph = nullptr;
    }

    if (ctx->filter_frame != nullptr)
    {
        av_frame_free(&ctx->yuv_frame);
        ctx->filter_frame = nullptr;
    }
}
void destroy_stream_context(StreamContext* ctx)
{
    destroy_steam_decode_codec(ctx);
    destroy_steam_filter(ctx);
    destroy_steam_encode_codec(ctx);
    delete ctx;
}
void create_encode_codec_context(std::shared_ptr<StreamContext>& ctx)
{
    const auto* codec = avcodec_find_encoder(ctx->codec_ctx->codec_id);
    if (codec == nullptr)
    {
        LOG_ERROR << "not found codec " << avcodec_get_name(ctx->codec_ctx->codec_id);
        return;
    }
    ctx->encode_codec_ctx = avcodec_alloc_context3(codec);
    if (ctx->encode_codec_ctx == nullptr)
    {
        LOG_ERROR << "codec context alloc failed";
        return;
    }
    ctx->encode_codec_ctx->width = ctx->codec_ctx->width;
    ctx->encode_codec_ctx->height = ctx->codec_ctx->height;
    ctx->encode_codec_ctx->time_base = ctx->codec_ctx->time_base;
    ctx->encode_codec_ctx->framerate = ctx->codec_ctx->framerate;
    ctx->encode_codec_ctx->sample_aspect_ratio = ctx->codec_ctx->sample_aspect_ratio;

    ctx->encode_codec_ctx->pix_fmt = ctx->codec_ctx->pix_fmt;

    if (codec->pix_fmts != nullptr)
    {
        ctx->encode_codec_ctx->pix_fmt = codec->pix_fmts[0];
    }

    auto ex = ScopedExit::make_scoped_exit([&]() { destroy_steam_encode_codec(ctx.get()); });
    int ret = avcodec_open2(ctx->encode_codec_ctx, codec, nullptr);
    if (ret != 0)
    {
        LOG_ERROR << "open codec failed " << av_errno_to_string(ret);
        return;
    }
    ex.cancel();
    ctx->encode_pkg = av_packet_alloc();
}
std::shared_ptr<StreamContext> create_stream_context(AVCodecID codec_id)
{
    std::shared_ptr<StreamContext> ctx(new StreamContext, destroy_stream_context);

    const auto* codec = avcodec_find_decoder(codec_id);
    if (codec == nullptr)
    {
        LOG_ERROR << "codec not find " << avcodec_get_name(codec_id);
        return nullptr;
    }

    ctx->codec_parse_ctx = av_parser_init(codec_id);
    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (ctx->codec_ctx == nullptr)
    {
        LOG_ERROR << "codec context alloc failed " << av_errno_to_string(errno);
        return nullptr;
    }
    ctx->codec_ctx->time_base = (AVRational){1, 25};
    ctx->codec_ctx->framerate = (AVRational){25, 1};
    int ret = avcodec_open2(ctx->codec_ctx, codec, nullptr);
    if (ret != 0)
    {
        LOG_ERROR << "open codec failed " << av_errno_to_string(ret);
        return nullptr;
    }
    ctx->pkg = av_packet_alloc();
    ctx->yuv_frame = av_frame_alloc();
    return ctx;
}

int create_filter(std::shared_ptr<StreamContext>& stream_ctx, const std::string& filter)
{
    char args[1024] = {0};
    snprintf(args,
             sizeof(args),
             "width=%d:height=%d:time_base=%d/%d:pix_fmt=%d:pixel_aspect=%d/%d",
             stream_ctx->codec_ctx->width,
             stream_ctx->codec_ctx->height,
             stream_ctx->codec_ctx->time_base.num,
             stream_ctx->codec_ctx->time_base.den,
             stream_ctx->codec_ctx->pix_fmt,
             stream_ctx->codec_ctx->sample_aspect_ratio.num,
             stream_ctx->codec_ctx->sample_aspect_ratio.den);

    LOG_DEBUG << "filter args " << args;
    auto ex = ScopedExit::make_scoped_exit([&]() { destroy_steam_filter(stream_ctx.get()); });
    stream_ctx->graph = avfilter_graph_alloc();

    const AVFilter* buffer_src = avfilter_get_by_name("buffer");
    const AVFilter* buffer_sink = avfilter_get_by_name("buffersink");

    // clang-format off
    int ret = avfilter_graph_create_filter(&stream_ctx->src_ctx, buffer_src, "in", args, nullptr, stream_ctx->graph);
    if (ret < 0)
    {
        return -1;
    }

    ret = avfilter_graph_create_filter( &stream_ctx->sink_ctx, buffer_sink, "out", nullptr, nullptr, stream_ctx->graph);
    if (ret < 0)
    {
        return -1;
    }
    // clang-format on

    auto* inputs = avfilter_inout_alloc();
    auto* outputs = avfilter_inout_alloc();

    DEFER(avfilter_inout_free(&inputs));
    DEFER(avfilter_inout_free(&outputs));

    outputs->name = av_strdup("in");
    outputs->filter_ctx = stream_ctx->src_ctx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = stream_ctx->sink_ctx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    ret = avfilter_graph_parse_ptr(stream_ctx->graph, filter.data(), &inputs, &outputs, nullptr);
    if (ret < 0)
    {
        return -1;
    }

    LOG_DEBUG << "config filter " << filter;

    ret = avfilter_graph_config(stream_ctx->graph, nullptr);
    if (ret < 0)
    {
        return -1;
    }
    ex.cancel();
    stream_ctx->filter_frame = av_frame_alloc();

    return 0;
}

void decode_package(std::shared_ptr<StreamContext>& stream_ctx, AVPacket* pkg, AVFrame* frame, const DecodeCb& cb)
{
    int ret = avcodec_send_packet(stream_ctx->codec_ctx, pkg);
    if (ret == AVERROR(EAGAIN))
    {
        LOG_ERROR << "Receive_frame and send_packet both returned EAGAIN, which is an API violation";
    }
    else if (ret < 0)
    {
        LOG_ERROR << "avcodec send packet failed " << av_errno_to_string(ret);
        return;
    }
    while (ret >= 0)
    {
        ret = avcodec_receive_frame(stream_ctx->codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return;
        }
        if (ret < 0)
        {
            LOG_DEBUG << "avcodec receive frame failed " << av_errno_to_string(ret);
            return;
        }
        if (cb)
        {
            cb(stream_ctx, frame);
        }
    }
}

void encode_frame(std::shared_ptr<StreamContext>& stream_ctx, AVPacket* pkg, AVFrame* frame, const EncodeCb& cb)
{
    int ret = avcodec_send_frame(stream_ctx->encode_codec_ctx, frame);
    if (ret < 0)
    {
        LOG_ERROR << "encode frame failed " << av_errno_to_string(ret);
        return;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(stream_ctx->encode_codec_ctx, pkg);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            break;
        }
        if (cb)
        {
            cb(stream_ctx, pkg);
        }
    }
}

void filter_frame(std::shared_ptr<StreamContext>& stream_ctx, AVFrame* frame1, const FilterCb& cb)
{
    int ret = av_buffersrc_add_frame_flags(stream_ctx->src_ctx, frame1, 0);
    if (ret < 0)
    {
        LOG_ERROR << "av filter add frame failed " << av_errno_to_string(ret);
        return;
    }
    while (true)
    {
        ret = av_buffersink_get_frame(stream_ctx->sink_ctx, stream_ctx->filter_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            break;
        }
        if (cb)
        {
            cb(stream_ctx, stream_ctx->filter_frame);
        }
    }
}

AVCodecID get_codec_id_by_filename(const std::string& filename)
{
    std::string_view s(filename);

    if (s.ends_with(".h264") || s.ends_with(".264"))
    {
        return AV_CODEC_ID_H264;
    }
    if (s.ends_with(".h265") || s.ends_with(".265"))
    {
        return AV_CODEC_ID_H265;
    }
    return AV_CODEC_ID_NONE;
}
#include "ps.h"
#include "rtp.h"

void encode_cb(std::shared_ptr<StreamContext>& stream_ctx, AVPacket* pkg)
{
    write_to_file(stream_ctx->filename, pkg->data, pkg->size);
}
void filter_cb(std::shared_ptr<StreamContext>& stream_ctx, AVFrame* frame)
{
    encode_frame(stream_ctx, stream_ctx->encode_pkg, frame, encode_cb);
}

void decode_cb(std::shared_ptr<StreamContext>& stream_ctx, AVFrame* frame)
{
    if (stream_ctx->graph == nullptr)
    {
        create_filter(stream_ctx, stream_ctx->filter_describe);
    }
    if (stream_ctx->graph == nullptr)
    {
        return;
    }
    if (stream_ctx->encode_codec_ctx == nullptr)
    {
        create_encode_codec_context(stream_ctx);
    }
    if (stream_ctx->encode_codec_ctx == nullptr)
    {
        return;
    }

    filter_frame(stream_ctx, frame, filter_cb);
}

void process_raw_bytes(std::shared_ptr<StreamContext>& stream_ctx, const uint8_t* buffer, uint32_t size)
{
    std::vector<uint8_t> data_bytes(buffer, buffer + size);
    data_bytes.insert(data_bytes.end(), AV_INPUT_BUFFER_PADDING_SIZE, 0);

    const uint8_t* data = data_bytes.data();
    uint32_t data_size = data_bytes.size();

    while (data_size > 0)
    {
        int ret = av_parser_parse2(stream_ctx->codec_parse_ctx,
                                   stream_ctx->codec_ctx,
                                   &stream_ctx->pkg->data,
                                   &stream_ctx->pkg->size,
                                   data,
                                   static_cast<int>(data_size),
                                   AV_NOPTS_VALUE,
                                   AV_NOPTS_VALUE,
                                   0);
        if (ret < 0)
        {
            LOG_DEBUG << "av_parser_parse2 failed " << av_errno_to_string(ret);
            break;
        }
        data += ret;
        data_size -= ret;
        if (stream_ctx->pkg->size != 0)
        {
            decode_package(stream_ctx, stream_ctx->pkg, stream_ctx->yuv_frame, decode_cb);
        }
    }
    if (buffer == nullptr)
    {
        stream_ctx->pkg->data = nullptr;
        stream_ctx->pkg->size = 0;
        decode_package(stream_ctx, stream_ctx->pkg, stream_ctx->yuv_frame, decode_cb);
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <in_file> <out_file>\n";
        return 1;
    }
    auto codec_id = get_codec_id_by_filename(argv[1]);

    if (codec_id == AV_CODEC_ID_NONE)
    {
        // default codec id
        codec_id = AV_CODEC_ID_H265;
    }

    auto stream_ctx = create_stream_context(codec_id);
    if (!stream_ctx)
    {
        LOG_ERROR << "create steam context file " << argv[1];
        return -1;
    }

    stream_ctx->filter_describe = "split [main][tmp]; [tmp] crop=iw:ih/2:0:0, vflip [flip]; [main][flip] overlay=0:H/2";
    stream_ctx->filename = argv[2];

    // clang-format off
    auto raw_cb = [&](int, int, int64_t, int64_t, const uint8_t* data, uint32_t len) { process_raw_bytes(stream_ctx, data, len); };
    // clang-format on

    auto file_bytes = read_file_to_buffer(argv[1]);

    ps p(std::move(raw_cb));

    auto ps_cb = [&p](const uint8_t* bytes, int len) { p.demuxer(bytes, len); };

    rtp r(std::move(ps_cb));

    if (std::string_view(argv[1]).ends_with(".tcp_rtp"))
    {
        r.demuxer(file_bytes.data(), file_bytes.size());
    }

    if (std::string_view(argv[1]).ends_with(".ps"))
    {
        p.demuxer(file_bytes.data(), file_bytes.size());
    }

    process_raw_bytes(stream_ctx, nullptr, 0);

    LOG_DEBUG << "Hello World";

    return 0;
}
