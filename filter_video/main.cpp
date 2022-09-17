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
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

using DecodeCb = std::function<void(AVFrame*, AVPacket*)>;
using EncodeCb = std::function<void(AVPacket*)>;
using FilterCb = std::function<void(AVFrame*)>;

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
    AVCodecContext* codec_ctx = nullptr;
};

struct InputContext
{
    AVPacket* pkg = nullptr;
    AVFrame* yuv_frame = nullptr;
    AVFormatContext* fmt_ctx = nullptr;
    std::vector<std::shared_ptr<StreamContext>> streams;
};

struct OutputContext
{
    AVPacket* pkg = nullptr;
    std::vector<std::shared_ptr<StreamContext>> streams;
};

struct FilterContext
{
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;
    AVFilterGraph* graph = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkg = nullptr;
};

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

std::shared_ptr<OutputContext> create_output_context(const std::shared_ptr<InputContext>& input)
{
    auto delete_fn = [](StreamContext* c)
    {
        if (c->codec_ctx != nullptr)
        {
            avcodec_free_context(&c->codec_ctx);
        }
    };

    auto out_ctx = std::make_shared<OutputContext>();
    for (const auto& stream : input->streams)
    {
        if (stream->codec_ctx->codec_type != AVMEDIA_TYPE_VIDEO && stream->codec_ctx->codec_type != AVMEDIA_TYPE_AUDIO)
        {
            continue;
        }

        std::string codec_type = av_get_media_type_string(stream->codec_ctx->codec_type);

        std::shared_ptr<StreamContext> stream_ctx(new StreamContext, delete_fn);

        const auto* codec = avcodec_find_encoder(stream->codec_ctx->codec_id);
        stream_ctx->codec_ctx = avcodec_alloc_context3(codec);
        stream_ctx->stream_index = stream->stream_index;
        if (stream->codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            stream_ctx->codec_ctx->height = stream->codec_ctx->height;
            stream_ctx->codec_ctx->width = stream->codec_ctx->width;
            stream_ctx->codec_ctx->sample_aspect_ratio = stream->codec_ctx->sample_aspect_ratio;

            if (codec->pix_fmts != nullptr)
            {
                stream_ctx->codec_ctx->pix_fmt = codec->pix_fmts[0];
            }
            else
            {
                stream_ctx->codec_ctx->pix_fmt = stream->codec_ctx->pix_fmt;
            }
            stream_ctx->codec_ctx->time_base = (AVRational){1, 25};
            stream_ctx->codec_ctx->framerate = (AVRational){25, 1};
        }
        else if (stream->codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            stream_ctx->codec_ctx->sample_rate = stream->codec_ctx->sample_rate;
            stream_ctx->codec_ctx->ch_layout = stream->codec_ctx->ch_layout;
            stream_ctx->codec_ctx->sample_fmt = codec->sample_fmts[0];
            stream_ctx->codec_ctx->time_base = (AVRational){1, stream_ctx->codec_ctx->sample_rate};
        }

        int ret = avcodec_open2(stream_ctx->codec_ctx, codec, nullptr);
        if (ret < 0)
        {
            LOG_ERROR << "codec open failed " << codec_type << " " << av_errno_to_string(ret);
            assert(false);
            continue;
        }
        out_ctx->streams.push_back(stream_ctx);
    }
    out_ctx->pkg = av_packet_alloc();
    return out_ctx;
}
std::shared_ptr<FilterContext> create_filter(const AVCodecContext* ctx,
                                             const AVCodecContext* enc_ctx,
                                             const std::string& filter)
{
    auto close_filter = [](FilterContext* ptr)
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
        if (ptr->frame != nullptr)
        {
            av_frame_free(&ptr->frame);
        }
        if (ptr->pkg != nullptr)
        {
            av_packet_free(&ptr->pkg);
        }
        delete ptr;
    };
    char args[128] = {0};
    auto filter_ctx = std::shared_ptr<FilterContext>(new FilterContext, close_filter);
    if (ctx->codec_type == AVMEDIA_TYPE_VIDEO)
    {
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
            LOG_ERROR << "video in filter create failed " << av_errno_to_string(ret);

            assert(false);

            return nullptr;
        }
        ret = avfilter_graph_create_filter( &filter_ctx->sink_ctx, buffer_sink, "out", nullptr, nullptr, filter_ctx->graph);
        if (ret < 0)
        {
            LOG_ERROR << "video out filter create failed " << av_errno_to_string(ret);
            return nullptr;
        }
        // clang-format on
    }
    else if (ctx->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        const auto* buffer_src = avfilter_get_by_name("abuffer");
        const auto* buffer_sink = avfilter_get_by_name("abuffersink");

        snprintf(args,
                 sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
                 ctx->time_base.num,
                 ctx->time_base.den,
                 ctx->sample_rate,
                 av_get_sample_fmt_name(ctx->sample_fmt),
                 ctx->channel_layout);
        // clang-format off
        int ret = avfilter_graph_create_filter(&filter_ctx->src_ctx, buffer_src, "in", args, nullptr, filter_ctx->graph);
        if (ret < 0)
        {
            LOG_ERROR << "audio in filter create failed " << av_errno_to_string(ret);
            return nullptr;
        }

        ret = avfilter_graph_create_filter( &filter_ctx->sink_ctx, buffer_sink, "out", nullptr, nullptr, filter_ctx->graph);
        if (ret < 0)
        {
            LOG_ERROR << "audio out filter create failed " << av_errno_to_string(ret);
            return nullptr;
        }
        // clang-format on

        av_opt_set_bin(filter_ctx->sink_ctx,
                       "sample_fmts",
                       reinterpret_cast<const uint8_t*>(&enc_ctx->sample_fmt),
                       sizeof(enc_ctx->sample_fmt),
                       AV_OPT_SEARCH_CHILDREN);

        av_opt_set_bin(filter_ctx->sink_ctx,
                       "channel_layouts",
                       reinterpret_cast<const uint8_t*>(&enc_ctx->channel_layout),
                       sizeof(enc_ctx->channel_layout),
                       AV_OPT_SEARCH_CHILDREN);

        av_opt_set_bin(filter_ctx->sink_ctx,
                       "sample_rates",
                       reinterpret_cast<const uint8_t*>(&enc_ctx->sample_rate),
                       sizeof(enc_ctx->sample_rate),
                       AV_OPT_SEARCH_CHILDREN);
    }

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

    int ret = avfilter_graph_parse_ptr(filter_ctx->graph, filter.data(), &inputs, &outputs, nullptr);
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
    filter_ctx->pkg = av_packet_alloc();

    return filter_ctx;
}
std::shared_ptr<InputContext> create_input_context(const std::string& file)
{
    auto input_fn = [](InputContext* c)
    {
        avformat_free_context(c->fmt_ctx);
        av_packet_free(&c->pkg);
        av_frame_free(&c->yuv_frame);
    };

    std::shared_ptr<InputContext> input_ctx(new InputContext, input_fn);

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
        if (c->codec_ctx != nullptr)
        {
            avcodec_free_context(&c->codec_ctx);
        }
    };

    for (int i = 0; i < input_ctx->fmt_ctx->nb_streams; i++)
    {
        std::shared_ptr<StreamContext> stream_ctx(new StreamContext, delete_fn);
        stream_ctx->stream_index = i;
        stream_ctx->stream = input_ctx->fmt_ctx->streams[i];
        const auto* codec = avcodec_find_decoder(stream_ctx->stream->codecpar->codec_id);
        stream_ctx->codec_ctx = avcodec_alloc_context3(codec);
        int err = avcodec_parameters_to_context(stream_ctx->codec_ctx, stream_ctx->stream->codecpar);
        if (err < 0)
        {
            LOG_ERROR << "avcodec parameters to context failed " << av_errno_to_string(err);
            continue;
        }

        stream_ctx->codec_ctx->time_base = stream_ctx->stream->time_base;
        if (avcodec_open2(stream_ctx->codec_ctx, codec, nullptr) < 0)
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
            cb(frame, pkg);
        }
    }
}

void encode_frame(AVCodecContext* codec_ctx, AVPacket* pkg, AVFrame* frame, const EncodeCb& cb)
{
    int ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0)
    {
        LOG_ERROR << "encode frame failed " << av_errno_to_string(ret);
        return;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(codec_ctx, pkg);
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
            cb(pkg);
        }

        // av_packet_unref(pkg);
    }
}

void filter_frame(const std::shared_ptr<FilterContext>& ctx, AVFrame* frame1, const FilterCb& cb)
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
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <in_file> <out_file>\n";
        return 1;
    }

    auto input_ctx = create_input_context(argv[1]);
    if (!input_ctx)
    {
        return -1;
    }
    std::shared_ptr<OutputContext> output_ctx = nullptr;
    std::shared_ptr<FilterContext> audio_filter_ctx = nullptr;
    std::shared_ptr<FilterContext> video_filter_ctx = nullptr;

    const char* video_filter_describe =
        "split [main][tmp]; [tmp] crop=iw:ih/2:0:0, vflip [flip]; [main][flip] overlay=0:H/2";
    const char* audio_filter_describe = "anull";

    uint32_t frame_count = 0;
    uint32_t packet_count = 0;

    auto encode_cb = [&](AVPacket* pkg)
    {
        printf("write packet %3" PRId64 " (size=%5d)\n", pkg->pts, pkg->size);
        write_to_file(argv[2], pkg->data, pkg->size);
    };

    auto video_filter_cb = [&](AVFrame* frame)
    {
        for (auto& out_stream : output_ctx->streams)
        {
            if (out_stream->codec_ctx->codec_type != AVMEDIA_TYPE_VIDEO)
            {
                continue;
            }
            encode_frame(out_stream->codec_ctx, video_filter_ctx->pkg, frame, encode_cb);
        }
    };
    // auto audio_filter_cb = [&](AVFrame* frame) { write_frame_to_file(argv[2], frame); };
    auto audio_filter_cb = nullptr;
    bool audio = false;
    bool video = false;
    auto decode_cb = [&](AVFrame* frame, AVPacket* pkg)
    {
        std::shared_ptr<StreamContext> in = nullptr;
        for (auto& in_stream : input_ctx->streams)
        {
            if (in_stream->stream_index != pkg->stream_index)
            {
                continue;
            }
            in = in_stream;
            break;
        }
        if (!in)
        {
            return;
        }
        if (in->codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video = true;
        }
        if (in->codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio = true;
        }
        if (!video)
        {
            return;
        }
        if (output_ctx == nullptr)
        {
            output_ctx = create_output_context(input_ctx);
        }
        if (!output_ctx)
        {
            return;
        }
        LOG_DEBUG << av_get_media_type_string(in->codec_ctx->codec_type) << " frame index " << frame_count++
                  << " coded_picture_number " << frame->coded_picture_number << " pix_fmt "
                  << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) << " pkt pos " << frame->pkt_pos
                  << " pkt size " << frame->pkt_size;

        std::shared_ptr<StreamContext> out = nullptr;
        for (auto& out_stream : output_ctx->streams)
        {
            if (out_stream->stream_index != pkg->stream_index)
            {
                continue;
            }
            out = out_stream;
            break;
        }
        if (in == nullptr || out == nullptr)
        {
            return;
        }
        if (in->codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            if (video_filter_ctx == nullptr)
            {
                video_filter_ctx = create_filter(in->codec_ctx, out->codec_ctx, video_filter_describe);
            }
            if (video_filter_ctx == nullptr)
            {
                LOG_ERROR << "create video filter failed";
                return;
            }
            filter_frame(video_filter_ctx, frame, video_filter_cb);
        }
        else
        {
            if (audio_filter_ctx == nullptr)
            {
                audio_filter_ctx = create_filter(in->codec_ctx, out->codec_ctx, audio_filter_describe);
            }
            if (audio_filter_ctx == nullptr)
            {
                LOG_ERROR << "create audio filter failed";
                return;
            }
            filter_frame(audio_filter_ctx, frame, audio_filter_cb);
        }
    };
    while (av_read_frame(input_ctx->fmt_ctx, input_ctx->pkg) == 0)
    {
        // clang-format off
        LOG_DEBUG << "packet index " << packet_count++ << " pkt ptos " << input_ctx->pkg->pos << " pkt size " << input_ctx->pkg->size;
        // clang-format on

        for (auto& stream : input_ctx->streams)
        {
            if (stream->stream_index != input_ctx->pkg->stream_index)
            {
                continue;
            }
            decode_package(stream->codec_ctx, input_ctx->pkg, input_ctx->yuv_frame, decode_cb);
            av_packet_unref(input_ctx->pkg);
        }
    }
    input_ctx->pkg->data = nullptr;
    input_ctx->pkg->size = 0;

    // clang-format off
    decode_package( input_ctx->streams[input_ctx->pkg->stream_index]->codec_ctx, input_ctx->pkg, input_ctx->yuv_frame, decode_cb);
    // clang-format on

    LOG_DEBUG << "Hello World";

    return 0;
}
