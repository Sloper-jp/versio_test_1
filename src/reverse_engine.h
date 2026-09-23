#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Clock-synced reverse (docs/design.md §3.2–3.4). Hardware independent.
//
// Input is recorded continuously. For a segment anchored at clock time t_k
// with offset d, the output is
//     read(t) = 2*t_k - t + d
// which is realised as a head that starts at index (t_k + min(d, 0)) at time
// (t_k + max(d, 0)) and then moves backwards one sample per sample.
class ReverseEngine
{
  public:
    static constexpr size_t kMaxFade    = 1024;
    static constexpr size_t kMaxPending = 32;

    // buf_l / buf_r must each hold `len` samples. They are cleared here.
    void Init(float* buf_l, float* buf_r, size_t len, size_t fade_samples)
    {
        buf_[0] = buf_l;
        buf_[1] = buf_r;
        len_    = static_cast<uint32_t>(len);
        std::memset(buf_l, 0, len * sizeof(float));
        std::memset(buf_r, 0, len * sizeof(float));

        fade_len_ = fade_samples < 1 ? 1
                    : fade_samples > kMaxFade ? kMaxFade
                                              : fade_samples;
        // Equal-power fade: the two heads play unrelated material.
        for(size_t i = 0; i <= fade_len_; i++)
        {
            const float x = static_cast<float>(i) / fade_len_ * 1.5707963f;
            fade_in_[i]   = sinf(x);
            fade_out_[i]  = cosf(x);
        }

        write_       = 0;
        now_         = 0;
        cur_         = 0;
        prev_        = 0;
        fade_pos_    = fade_len_;
        pending_len_ = 0;
        latched_d_   = 0;
    }

    // Call at the sample a clock event occurs, before Process() for that
    // sample. `offset` is d in samples. When `relatch` is false (rule 4
    // re-anchor) the offset latched at the previous clock is reused.
    void OnClock(int32_t offset, bool relatch = true)
    {
        if(relatch)
            latched_d_ = offset;
        const int32_t d = latched_d_;

        Pending p;
        p.due   = now_ + static_cast<uint32_t>(d > 0 ? d : 0);
        p.start = Wrap(static_cast<int64_t>(write_) + (d < 0 ? d : 0));

        // A later clock whose segment would start no later than a queued one
        // supersedes it (can happen when d is turned down quickly).
        while(pending_len_ > 0
              && static_cast<int32_t>(p.due - pending_[pending_len_ - 1].due)
                     <= 0)
            pending_len_--;
        if(pending_len_ == kMaxPending)
            PopFront();
        pending_[pending_len_++] = p;
    }

    void Process(float in_l, float in_r, float* out_l, float* out_r)
    {
        buf_[0][write_] = in_l;
        buf_[1][write_] = in_r;

        while(pending_len_ > 0
              && static_cast<int32_t>(now_ - pending_[0].due) >= 0)
        {
            StartSegment(pending_[0].start);
            PopFront();
        }

        float l = buf_[0][cur_];
        float r = buf_[1][cur_];
        if(fade_pos_ < fade_len_)
        {
            const float gi = fade_in_[fade_pos_];
            const float go = fade_out_[fade_pos_];
            l              = l * gi + buf_[0][prev_] * go;
            r              = r * gi + buf_[1][prev_] * go;
            fade_pos_++;
        }
        *out_l = l;
        *out_r = r;

        cur_   = cur_ == 0 ? len_ - 1 : cur_ - 1;
        prev_  = prev_ == 0 ? len_ - 1 : prev_ - 1;
        write_ = write_ + 1 == len_ ? 0 : write_ + 1;
        now_++;
    }

  private:
    struct Pending
    {
        uint32_t due;
        uint32_t start;
    };

    void StartSegment(uint32_t start)
    {
        prev_     = cur_;
        cur_      = start;
        fade_pos_ = 0;
    }

    void PopFront()
    {
        for(size_t i = 1; i < pending_len_; i++)
            pending_[i - 1] = pending_[i];
        pending_len_--;
    }

    uint32_t Wrap(int64_t i) const
    {
        const int64_t n = len_;
        return static_cast<uint32_t>(((i % n) + n) % n);
    }

    float*   buf_[2];
    uint32_t len_;
    uint32_t write_;
    uint32_t now_;
    uint32_t cur_;
    uint32_t prev_;
    size_t   fade_len_;
    size_t   fade_pos_;
    float    fade_in_[kMaxFade + 1];
    float    fade_out_[kMaxFade + 1];
    Pending  pending_[kMaxPending];
    size_t   pending_len_;
    int32_t  latched_d_;
};
