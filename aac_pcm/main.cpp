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
}

using DecodeCb = std::function<void(AVFrame*)>;
using FilterCb = std::function<void(AVFrame*)>;

struct AACContext
{
    AVCodecContext* codec_ctx = nullptr;
    AVCodecParserContext* codec_parse_ctx = nullptr;
    AVFrame* pcm_frame = nullptr;
    AVPacket* pkg = nullptr;
};

std::string av_errno_to_string(int err)
{
    char buff[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buff, AV_ERROR_MAX_STRING_SIZE);
    return buff;
}

void close_aac_context(AACContext* ctx)
{
    if (ctx->codec_ctx != nullptr)
    {
        avcodec_free_context(&ctx->codec_ctx);
    }
    if (ctx->codec_parse_ctx != nullptr)
    {
        av_parser_close(ctx->codec_parse_ctx);
    }
    if (ctx->pcm_frame != nullptr)
    {
        av_frame_free(&ctx->pcm_frame);
    }
    if (ctx->pkg != nullptr)
    {
        av_packet_free(&ctx->pkg);
    }
    delete ctx;
}

std::shared_ptr<AACContext> create_aac_context()
{
    std::shared_ptr<AACContext> ctx(new AACContext, close_aac_context);
    const auto codec_id = AV_CODEC_ID_AAC;
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
    ctx->pcm_frame = av_frame_alloc();

    int ret = avcodec_open2(ctx->codec_ctx, codec, nullptr);
    if (ret != 0)
    {
        LOG_ERROR << "open codec failed " << av_errno_to_string(ret);
        return nullptr;
    }

    return ctx;
}

void decode_package(const std::shared_ptr<AACContext>& ctx, const DecodeCb& cb)
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
        ret = avcodec_receive_frame(ctx->codec_ctx, ctx->pcm_frame);
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
            cb(ctx->pcm_frame);
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
        std::cerr << "Usage: " << argv[0] << " <in_aac_file> <out_pcm_file>\n";
        // ffplay -f f32le  -ar 8000 -ac 1  out.pcm
        return 1;
    }

    auto aac_ctx = create_aac_context();
    if (!aac_ctx)
    {
        LOG_ERROR << "open aac codec failed";
        return -1;
    }

    auto file_bytes = read_file_to_buffer(argv[1]);
    file_bytes.insert(file_bytes.end(), AV_INPUT_BUFFER_PADDING_SIZE, 0);

    uint8_t* data = file_bytes.data();
    size_t data_size = file_bytes.size();

    auto decode_cb = [&](AVFrame* frame)
    {
        int sample_size = av_get_bytes_per_sample(aac_ctx->codec_ctx->sample_fmt);
        if (sample_size == 0)
        {
            LOG_ERROR << "unknown sample size";
            return;
        }
        LOG_DEBUG << "sample_rate " << frame->sample_rate << " channels " << frame->ch_layout.nb_channels << " format "
                  << frame->format << " sample_size " << sample_size;
        for (int i = 0; i < frame->nb_samples; i++)
        {
            for (int channle = 0; channle < aac_ctx->codec_ctx->ch_layout.nb_channels; channle++)
            {
                write_to_file(argv[2], frame->data[channle] + static_cast<ptrdiff_t>(sample_size * i), sample_size);
            }
        }
    };

    while (data_size > 0)
    {
        int ret = av_parser_parse2(aac_ctx->codec_parse_ctx,
                                   aac_ctx->codec_ctx,
                                   &aac_ctx->pkg->data,
                                   &aac_ctx->pkg->size,
                                   data,
                                   static_cast<int>(data_size),
                                   AV_NOPTS_VALUE,
                                   AV_NOPTS_VALUE,
                                   0);
        if (ret < 0)

        {
            LOG_DEBUG << "av parse 2 failed " << av_errno_to_string(ret);
            break;
        }
        data += ret;
        data_size -= ret;
        if (aac_ctx->pkg->size != 0)
        {
            decode_package(aac_ctx, decode_cb);
        }
    }
    decode_package(aac_ctx, decode_cb);

    LOG_DEBUG << "Hello World";

    return 0;
}
