#include "ps.h"
#include "log.h"

#include "ps_parse.h"

#include <vector>

class ps_impl
{
   private:
    static int onpacket(
        void *param, int /*unused*/, int avtype, int flags, int64_t pts, int64_t dts, const void *data, size_t bytes)
    {
        auto *that = static_cast<ps_impl *>(param);
        that->cb_(avtype, flags, pts, dts, static_cast<const uint8_t *>(data), bytes);
        return 0;
    }

   public:
    explicit ps_impl(NaluCb cb) : cb_(std::move(cb)) { ps_ = ps_parse_create(onpacket, this); }
    ~ps_impl() { ps_parse_destroy(ps_); }
    void demuxer(const uint8_t *data, uint32_t size)
    {
        bytes_.insert(bytes_.end(), data, data + size);
        int len = ps_parse_input(ps_, bytes_.data(), bytes_.size());
        if (len < 0)
        {
            return;
        }
        bytes_.erase(bytes_.begin(), bytes_.begin() + len);
    }

   private:
    NaluCb cb_;
    std::vector<uint8_t> bytes_;
    ps_parse *ps_ = nullptr;
};

ps::ps(NaluCb cb) : ps_(std::make_shared<ps_impl>(std::move(cb))) {}
void ps::demuxer(const uint8_t *data, uint32_t size) { ps_->demuxer(data, size); }
