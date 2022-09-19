#ifndef __RTP_H__
#define __RTP_H__

#include <cstdint>
#include <functional>

using RtpCb = std::function<void(const uint8_t* data, uint32_t len)>;

class rtp
{
   public:
    explicit rtp(RtpCb cb) : cb_(std::move(cb)) {}
    ~rtp() = default;

   public:
    void demuxer(const uint8_t* bytes, uint32_t size);

   private:
    RtpCb cb_;
};

#endif    //  __RTP_H__
