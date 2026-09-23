#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Feed-forward comb on the high band (docs/design.md §3.7). Hardware
// independent.
//
//     h = HPF(x),  l = x - h
//     y = l + g * (h + w * h(t - tau_ch)),   g = 1 / sqrt(1 + w^2)
//
// Mix m (0..1): w = 0 -> 1 over 0..50%, stereo spread s = 0 -> 1 over 50..100%.
class CombFilter
{
  public:
    struct Config
    {
        float sample_rate;
        float hpf_hz;
        float stereo_spread_ms;   // Δmax: L/R difference at 100%
        float min_channel_delay_ms;
        float delay_smooth_ms;
        float mix_smooth_ms;
    };

    static constexpr size_t kDelayLen = 512; // > 8.25 ms at 48 kHz + margin

    void Init(const Config& cfg)
    {
        cfg_ = cfg;
        ms_to_samples_ = cfg.sample_rate / 1000.f;

        // 2nd-order Butterworth high-pass (RBJ cookbook, Q = 1/sqrt(2)).
        const float w0    = 2.f * 3.14159265f * cfg.hpf_hz / cfg.sample_rate;
        const float cw    = cosf(w0);
        const float alpha = sinf(w0) / (2.f * 0.70710678f);
        const float a0    = 1.f + alpha;
        b0_               = (1.f + cw) / 2.f / a0;
        b1_               = -(1.f + cw) / a0;
        b2_               = b0_;
        a1_               = -2.f * cw / a0;
        a2_               = (1.f - alpha) / a0;

        delay_coef_ = SmoothCoef(cfg.delay_smooth_ms);
        mix_coef_   = SmoothCoef(cfg.mix_smooth_ms);

        for(int ch = 0; ch < 2; ch++)
        {
            z1_[ch] = z2_[ch] = 0.f;
            for(size_t i = 0; i < kDelayLen; i++)
                line_[ch][i] = 0.f;
        }
        write_ = 0;

        SetMix(0.f);
        SetDelayMs(1.f);
        SnapParameters();
    }

    void SetMix(float m)
    {
        m         = m < 0.f ? 0.f : m > 1.f ? 1.f : m;
        w_target_ = m >= 0.5f ? 1.f : m / 0.5f;
        s_target_ = m <= 0.5f ? 0.f : (m - 0.5f) / 0.5f;
    }

    void SetDelayMs(float ms) { tau_target_ = ms * ms_to_samples_; }

    // Jump the smoothed parameters to their targets (startup / tests).
    void SnapParameters()
    {
        w_   = w_target_;
        s_   = s_target_;
        tau_ = tau_target_;
    }

    void Process(float in_l, float in_r, float* out_l, float* out_r)
    {
        w_ += (w_target_ - w_) * mix_coef_;
        s_ += (s_target_ - s_) * mix_coef_;
        tau_ += (tau_target_ - tau_) * delay_coef_;

        const float g      = 1.f / sqrtf(1.f + w_ * w_);
        const float half   = 0.5f * s_ * cfg_.stereo_spread_ms * ms_to_samples_;
        const float min_d  = cfg_.min_channel_delay_ms * ms_to_samples_;
        const float tau_l  = tau_ - half < min_d ? min_d : tau_ - half;
        const float max_d  = static_cast<float>(kDelayLen - 3);
        const float tau_r  = tau_ + half > max_d ? max_d : tau_ + half;

        *out_l = Channel(0, in_l, tau_l, g);
        *out_r = Channel(1, in_r, tau_r, g);
        write_ = (write_ + 1) & (kDelayLen - 1);
    }

  private:
    float Channel(int ch, float x, float delay, float g)
    {
        // Transposed direct form II
        const float h = b0_ * x + z1_[ch];
        z1_[ch]       = b1_ * x - a1_ * h + z2_[ch];
        z2_[ch]       = b2_ * x - a2_ * h;

        line_[ch][write_] = h;
        const float hd    = ReadHermite(line_[ch], delay);
        return (x - h) + g * (h + w_ * hd);
    }

    // Fractional delay read (delay >= 1 sample so the newer neighbour exists).
    float ReadHermite(const float* buf, float delay) const
    {
        const int32_t k    = static_cast<int32_t>(delay);
        const float   f    = delay - static_cast<float>(k);
        const size_t  mask = kDelayLen - 1;
        const size_t  i0   = (write_ - k) & mask;
        const float   ym1  = buf[(i0 + 1) & mask];
        const float   y0   = buf[i0];
        const float   y1   = buf[(i0 - 1) & mask];
        const float   y2   = buf[(i0 - 2) & mask];

        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

    float SmoothCoef(float ms) const
    {
        const float n = ms * ms_to_samples_;
        return n <= 1.f ? 1.f : 1.f - expf(-1.f / n);
    }

    Config cfg_;
    float  ms_to_samples_;
    float  b0_, b1_, b2_, a1_, a2_;
    float  z1_[2], z2_[2];
    float  line_[2][kDelayLen];
    size_t write_;
    float  delay_coef_, mix_coef_;
    float  w_target_, s_target_, tau_target_;
    float  w_, s_, tau_;
};

// Output safety stage: linear up to `threshold`, then saturates smoothly
// towards 1.0 with a continuous slope.
inline float SoftClip(float x, float threshold)
{
    const float a = x < 0.f ? -x : x;
    if(a <= threshold)
        return x;
    const float room = 1.f - threshold;
    const float y    = threshold + room * tanhf((a - threshold) / room);
    return x < 0.f ? -y : y;
}
