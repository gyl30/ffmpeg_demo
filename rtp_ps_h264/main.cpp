#include <cstddef>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>
#include <string_view>
#include "scoped_exit.hpp"

#include "log.h"
#include "ps.h"
#include "rtp.h"

static void write_to_file(const std::string& filename, const uint8_t* data, size_t size)
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
        LOG_ERROR << "open failed " << filename;
        return {};
    }

    fseek(fp, 0, SEEK_END);
    uint32_t filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> bytes(filesize);

    fread(bytes.data(), 1, filesize, fp);
    fclose(fp);
    LOG_DEBUG << filename << " file size " << bytes.size();
    return bytes;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <in_rtp_file> <out_h264_file>\n";
        return 1;
    }

    auto file_bytes = read_file_to_buffer(argv[1]);

    auto h264_cb = [](int avtype, int flags, int64_t pts, int64_t dts, const uint8_t* data, uint32_t len)
    {
        LOG_DEBUG << "123 type " << avtype << " flags " << flags << " pts " << pts << " dts " << dts << " bytes "
                  << len;
        write_to_file("123.265", static_cast<const uint8_t*>(data), len);
    };

    ps p(std::move(h264_cb));

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

    LOG_DEBUG << "Hello World";

    return 0;
}
