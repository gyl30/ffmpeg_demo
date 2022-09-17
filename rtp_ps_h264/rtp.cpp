#include "rtp.h"
#include <string>
#include <sstream>
;
#pragma pack(push, 1)
struct RtpHeader
{
    uint8_t csrc_len : 4;
    uint8_t extension : 1;
    uint8_t padding : 1;
    uint8_t version : 2;
    uint8_t payload : 7;
    uint8_t marker : 1;
    uint16_t seq_no;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

static std::string rtp_header_to_string(const RtpHeader* header)
{
    std::stringstream ss;
    ss << "RTP:";
    ss << "csrc_len " << static_cast<int>(header->csrc_len) << " ";
    ss << "extension " << static_cast<int>(header->extension) << " ";
    ss << "padding " << static_cast<int>(header->padding) << " ";
    ss << "version " << static_cast<int>(header->version) << " ";
    ss << "payload " << static_cast<int>(header->payload) << " ";
    ss << "marker " << static_cast<int>(header->marker) << " ";
    ss << "seq_no " << ntohs((int)header->seq_no) << " ";
    ss << "timestamp " << ntohl((int)header->timestamp) << " ";
    ss << "ssrc " << ntohl((int)header->ssrc) << " ";
    return ss.str();
}
void rtp::demuxer(const uint8_t* bytes, uint32_t size)
{
    const uint8_t* begin = bytes;
    const uint8_t* end = bytes + size;

    constexpr auto kTcpRtpHeadSize = 2;
    constexpr auto kRtpHeaderSize = sizeof(RtpHeader);

    while (begin < end)
    {
        uint16_t pkg_len = ntohs(*((uint16_t*)begin));

        const uint8_t* ps_start = begin + kTcpRtpHeadSize + kRtpHeaderSize;

        uint32_t ps_len = pkg_len - kRtpHeaderSize;
        if (cb_)
        {
            cb_(ps_start, ps_len);
        }
        begin = begin + kTcpRtpHeadSize + pkg_len;
    }
}
