#include <cstddef>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

#include "log.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

std::string av_errno_to_string(int err)
{
    char buff[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buff, AV_ERROR_MAX_STRING_SIZE);
    return buff;
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

void decoder_package_to_frames(AVCodecContext* codec_ctx, AVPacket* pkt, AVFrame* frame, const std::string& out);

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <in_h264_file> <out_yuv_file>\n";
        return 1;
    }
    const auto codec_id = AV_CODEC_ID_H264;
    auto* codec = avcodec_find_decoder(codec_id);
    auto* codec_parse_ctx = av_parser_init(codec_id);
    auto* codec_ctx = avcodec_alloc_context3(codec);
    auto* pkg = av_packet_alloc();
    auto* frame = av_frame_alloc();

    int ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret != 0)
    {
        LOG_ERROR << "avcode open failed codec id " << codec_id;
        return -1;
    }

    auto file_bytes = read_file_to_buffer(argv[1]);
    file_bytes.insert(file_bytes.end(), AV_INPUT_BUFFER_PADDING_SIZE, 0);

    uint8_t* data = file_bytes.data();
    size_t data_size = file_bytes.size();
    while (data_size > 0)
    {
        ret = av_parser_parse2(codec_parse_ctx,
                               codec_ctx,
                               &pkg->data,
                               &pkg->size,
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
        if (pkg->size != 0)
        {
            // 解码
            decoder_package_to_frames(codec_ctx, pkg, frame, argv[2]);
        }
    }

    LOG_DEBUG << "Hello World";

    av_parser_close(codec_parse_ctx);
    avcodec_free_context(&codec_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkg);

    return 0;
}

static void wirte_frame_to_file(const std::string& filename, const uint8_t* data, int size)
{
    FILE* fp = fopen(filename.c_str(), "ab+");
    if (fp != nullptr)
    {
        fwrite(data, 1, size, fp);
        fclose(fp);
    }
}
void decoder_package_to_frames(AVCodecContext* codec_ctx, AVPacket* pkt, AVFrame* frame, const std::string& out)
{
    int ret = avcodec_send_packet(codec_ctx, pkt);
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
        LOG_DEBUG << "frame pts " << frame->pts << " pkg dts " << frame->pkt_dts << " height " << frame->height
                  << " width " << frame->width;
        for (int j = 0; j < frame->height; j++)
        {
            wirte_frame_to_file(out, frame->data[0] + static_cast<ptrdiff_t>(j * frame->linesize[0]), frame->width);
        }
        for (int j = 0; j < frame->height / 2; j++)
        {
            wirte_frame_to_file(out, frame->data[1] + static_cast<ptrdiff_t>(j * frame->linesize[1]), frame->width / 2);
        }
        for (int j = 0; j < frame->height / 2; j++)
        {
            wirte_frame_to_file(out, frame->data[2] + static_cast<ptrdiff_t>(j * frame->linesize[2]), frame->width / 2);
        }
    }
}
