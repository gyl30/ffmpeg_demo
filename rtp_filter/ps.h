#ifndef __PS_H__
#define __PS_H__

#include <memory>
#include <functional>
#include <cstdint>

using NaluCb = std::function<void(int avtype, int flags, int64_t pts, int64_t dts, const uint8_t *data, uint32_t len)>;

class ps
{
   public:
    explicit ps(NaluCb cb);
    ~ps() = default;

   public:
    void demuxer(const uint8_t *data, uint32_t size);

   private:
    std::shared_ptr<class ps_impl> ps_;
};
#endif    // __PS_H__
