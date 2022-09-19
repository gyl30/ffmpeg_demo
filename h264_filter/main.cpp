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
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

using DecodeCb = std::function<void(AVFrame*)>;
using FilterCb = std::function<void(AVFrame*)>;

struct FilterContext
{
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;
    AVFilterGraph* graph = nullptr;
    AVFrame* frame = nullptr;
};
struct H264Context
{
    AVCodecContext* codec_ctx = nullptr;
    AVCodecParserContext* codec_parse_ctx = nullptr;
    AVFrame* yuv_frame = nullptr;
    AVPacket* pkg = nullptr;
};

std::string av_errno_to_string(int err)
{
    char buff[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buff, AV_ERROR_MAX_STRING_SIZE);
    return buff;
}

void close_filter(FilterContext* ptr)
{
    if (ptr->graph != nullptr)
    {
        avfilter_graph_free(&ptr->graph);
        ptr->graph = nullptr;
    }

    if (ptr->frame != nullptr)
    {
        av_frame_free(&ptr->frame);
        ptr->frame = nullptr;
    }
    delete ptr;
    ptr = nullptr;
}

std::shared_ptr<FilterContext> create_filter(const AVCodecContext* ctx, const std::string& filter)
{
    auto filter_ctx = std::shared_ptr<FilterContext>(new FilterContext, close_filter);
    char args[128] = {0};
    snprintf(args,
             sizeof(args),
             "width=%d:height=%d:time_base=%d/%d:pix_fmt=%d:pixel_aspect=%d/%d",
             ctx->width,
             ctx->height,
             ctx->time_base.num,
             ctx->time_base.den,
             ctx->pix_fmt,
             ctx->sample_aspect_ratio.num,
             ctx->sample_aspect_ratio.den);

    LOG_DEBUG << "filter args " << args;

    filter_ctx->graph = avfilter_graph_alloc();

    const AVFilter* buffer_src = avfilter_get_by_name("buffer");
    const AVFilter* buffer_sink = avfilter_get_by_name("buffersink");

    // clang-format off
    int ret = avfilter_graph_create_filter(&filter_ctx->src_ctx, buffer_src, "in", args, nullptr, filter_ctx->graph);
    if (ret < 0)
    {
        return nullptr;
    }

    ret = avfilter_graph_create_filter( &filter_ctx->sink_ctx, buffer_sink, "out", nullptr, nullptr, filter_ctx->graph);
    if (ret < 0)
    {
        return nullptr;
    }
    // clang-format on

    auto* inputs = avfilter_inout_alloc();
    auto* outputs = avfilter_inout_alloc();

    DEFER(avfilter_inout_free(&inputs));
    DEFER(avfilter_inout_free(&outputs));

    outputs->name = av_strdup("in");
    outputs->filter_ctx = filter_ctx->src_ctx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = filter_ctx->sink_ctx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    ret = avfilter_graph_parse_ptr(filter_ctx->graph, filter.data(), &inputs, &outputs, nullptr);
    if (ret < 0)
    {
        return nullptr;
    }

    LOG_DEBUG << "config filter " << filter;

    ret = avfilter_graph_config(filter_ctx->graph, nullptr);
    if (ret < 0)
    {
        return nullptr;
    }

    filter_ctx->frame = av_frame_alloc();

    return filter_ctx;
}

void close_h264_context(H264Context* ctx)
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
    delete ctx;
}

std::shared_ptr<H264Context> create_h264_context()
{
    std::shared_ptr<H264Context> ctx(new H264Context, close_h264_context);
    const auto codec_id = AV_CODEC_ID_H264;
    const auto* codec = avcodec_find_decoder(codec_id);
    if (codec == nullptr)
    {
        LOG_ERROR << "codec find failed";
        return nullptr;
    }

    ctx->codec_parse_ctx = av_parser_init(codec_id);
    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (ctx->codec_ctx == nullptr)
    {
        LOG_ERROR << "codec context alloc failed";
        return nullptr;
    }
    ctx->pkg = av_packet_alloc();
    ctx->yuv_frame = av_frame_alloc();

    int ret = avcodec_open2(ctx->codec_ctx, codec, nullptr);
    if (ret != 0)
    {
        LOG_ERROR << "open codec failed " << av_errno_to_string(ret);
        return nullptr;
    }

    return ctx;
}

void filter_frame(std::shared_ptr<FilterContext> ctx, AVFrame* frame1, const FilterCb& cb)
{
    int ret = av_buffersrc_add_frame_flags(ctx->src_ctx, frame1, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0)
    {
        LOG_ERROR << "av filter add frame failed " << av_errno_to_string(ret);
        return;
    }
    while (true)
    {
        ret = av_buffersink_get_frame(ctx->sink_ctx, ctx->frame);
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
            cb(ctx->frame);
        }
        DEFER(av_frame_unref(ctx->frame));
    }
}

void decode_package(std::shared_ptr<H264Context> ctx, const DecodeCb& cb)
{
    int ret = avcodec_send_packet(ctx->codec_ctx, ctx->pkg);
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
        ret = avcodec_receive_frame(ctx->codec_ctx, ctx->yuv_frame);
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
            cb(ctx->yuv_frame);
        }
        DEFER(av_frame_unref(ctx->yuv_frame));
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

    auto h264_ctx = create_h264_context();
    if (!h264_ctx)
    {
        LOG_ERROR << "open h264 codec failed";
        return -1;
    }

    auto file_bytes = read_file_to_buffer(argv[1]);
    file_bytes.insert(file_bytes.end(), AV_INPUT_BUFFER_PADDING_SIZE, 0);

    uint8_t* data = file_bytes.data();
    size_t data_size = file_bytes.size();
    std::shared_ptr<FilterContext> filter_ctx = nullptr;
    // std::string filter_describe = " drawbox=100:200:200:60:red@0.5:t=2";

    //const char* filter_describe = "drawgrid=width=100:height=100:thickness=2:color=red@0.5";
    const char* filter_describe = "split [main][tmp]; [tmp] crop=iw/2:ih:0:0 [flip]; [main][flip] overlay=W/2:0";

    auto filter_cb = [&](AVFrame* frame) { write_frame_to_file(argv[2], frame); };

    auto decode_cb = [&](AVFrame* frame)
    {
        if (filter_ctx == nullptr)
        {
            filter_ctx = create_filter(h264_ctx->codec_ctx, filter_describe);
        }
        if (filter_ctx == nullptr)
        {
            LOG_ERROR << "create filter failed";
            return;
        }
        filter_frame(filter_ctx, frame, filter_cb);
    };

    while (data_size > 0)
    {
        int ret = av_parser_parse2(h264_ctx->codec_parse_ctx,
                                   h264_ctx->codec_ctx,
                                   &h264_ctx->pkg->data,
                                   &h264_ctx->pkg->size,
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
        if (h264_ctx->pkg->size != 0)
        {
            decode_package(h264_ctx, decode_cb);
        }
    }
    decode_package(h264_ctx, decode_cb);

    LOG_DEBUG << "Hello World";

    return 0;
}
