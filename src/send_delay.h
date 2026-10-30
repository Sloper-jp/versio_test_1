#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "comb_filter.h" // SoftClip

// Send delay with L/R routing patterns (docs/design.md §3.8). Hardware
// independent.
//
// Sound is passed between delay lines, each pass applying the feedback once,
// so the first repeat of a pattern always comes from the same side:
//     Center:  L -> line 0 -> L,  R -> line 1 -> R  (each feeds itself)
//     L->R:    mono -> line 0 (L) -> line 1 (R) -> line 0 ...
//     L->L->R: mono -> line 0 (L) -> line 1 (L) -> line 2 (R) -> line 0 ...
// Every line input passes a high-pass filter, so low frequencies lose level on
// each repeat and do not build up, even with feedback above 100%.
class SendDelay
{
  public:
    enum class Pattern
    {
        Center,
        LeftRight,
        LeftLeftRight,
    };

    struct Config
    {
        float  sample_rate;
        size_t line_len;          // samples per line
        float  level_smooth_ms;   // send, return and feedback
        float  time_fade_ms;      // crossfade when the delay time changes
        float  route_fade_ms;     // crossfade when the pattern changes
        float  time_tolerance;    // ignore relative changes smaller than this
        float  hpf_hz;
        float  loop_clip;         // soft clip threshold in the feedback path
    };

    static constexpr int kNumLines = 3;

    // Each line must hold cfg.line_len samples. They are cleared here.
    void Init(const Config& cfg, float* line0, float* line1, float* line2)
    {
        cfg_      = cfg;
        lines_[0] = line0;
        lines_[1] = line1;
        lines_[2] = line2;
        for(int i = 0; i < kNumLines; i++)
            std::memset(lines_[i], 0, cfg.line_len * sizeof(float));

        const float ms_to_samples = cfg.sample_rate / 1000.f;
        const float n             = cfg.level_smooth_ms * ms_to_samples;
        level_coef_ = n <= 1.f ? 1.f : 1.f - expf(-1.f / n);
        time_step_  = FadeStep(cfg.time_fade_ms * ms_to_samples);
        route_step_ = FadeStep(cfg.route_fade_ms * ms_to_samples);

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

        write_ = 0;
        for(int i = 0; i < kNumLines; i++)
            z1_[i] = z2_[i] = 0.f;

        send_target_ = return_target_ = feedback_target_ = 0.f;
        send_ = return_ = feedback_ = 0.f;

        delay_ = old_delay_ = requested_delay_ = 1;
        time_fade_  = 1.f;
        pattern_    = old_pattern_ = Pattern::Center;
        route_fade_ = 1.f;
    }

    void SetSend(float g) { send_target_ = g; }
    void SetReturn(float g) { return_target_ = g; }
    void SetFeedback(float fb) { feedback_target_ = fb; }

    // Takes effect through a crossfade, unless the change is within tolerance.
    void SetDelaySamples(uint32_t samples) { requested_delay_ = Clamp(samples); }

    // Jump to the delay time and levels without fading (startup / tests).
    void Snap(uint32_t delay_samples)
    {
        delay_ = old_delay_ = requested_delay_ = Clamp(delay_samples);
        time_fade_ = 1.f;
        send_      = send_target_;
        return_    = return_target_;
        feedback_  = feedback_target_;
    }

    void SetPattern(Pattern p)
    {
        // A change during a fade waits until that fade has finished.
        if(p != pattern_ && route_fade_ >= 1.f)
        {
            old_pattern_ = pattern_;
            pattern_     = p;
            route_fade_  = 0.f;
        }
    }

    uint32_t Delay() const { return delay_; }

    // Outputs the return signal, already scaled by the return level.
    void Process(float in_l, float in_r, float* out_l, float* out_r)
    {
        send_ += (send_target_ - send_) * level_coef_;
        return_ += (return_target_ - return_) * level_coef_;
        feedback_ += (feedback_target_ - feedback_) * level_coef_;

        UpdateDelayTime();

        const float l = in_l * send_;
        const float r = in_r * send_;

        float read[kNumLines];
        for(int i = 0; i < kNumLines; i++)
            read[i] = ReadLine(i);

        Routed routed;
        Route(pattern_, l, r, read, routed);
        if(route_fade_ < 1.f)
        {
            Routed old;
            Route(old_pattern_, l, r, read, old);
            routed.l += (old.l - routed.l) * (1.f - route_fade_);
            routed.r += (old.r - routed.r) * (1.f - route_fade_);
            for(int i = 0; i < kNumLines; i++)
                routed.write[i]
                    += (old.write[i] - routed.write[i]) * (1.f - route_fade_);
            route_fade_ += route_step_;
        }

        for(int i = 0; i < kNumLines; i++)
            lines_[i][write_]
                = SoftClip(HighPass(i, routed.write[i]), cfg_.loop_clip);

        *out_l = routed.l * return_;
        *out_r = routed.r * return_;

        if(time_fade_ < 1.f)
            time_fade_ += time_step_;
        write_ = write_ + 1 == cfg_.line_len ? 0 : write_ + 1;
    }

  private:
    struct Routed
    {
        float write[kNumLines];
        float l, r;
    };

    void Route(Pattern      p,
               float        l,
               float        r,
               const float* read,
               Routed&      out) const
    {
        const float mono = 0.5f * (l + r);
        const float fb   = feedback_;
        switch(p)
        {
            case Pattern::Center:
                out.write[0] = l + fb * read[0];
                out.write[1] = r + fb * read[1];
                out.write[2] = 0.f;
                out.l        = read[0];
                out.r        = read[1];
                break;
            case Pattern::LeftRight:
                out.write[0] = mono + fb * read[1];
                out.write[1] = fb * read[0];
                out.write[2] = 0.f;
                out.l        = read[0];
                out.r        = read[1];
                break;
            case Pattern::LeftLeftRight:
            default:
                out.write[0] = mono + fb * read[2];
                out.write[1] = fb * read[0];
                out.write[2] = fb * read[1];
                out.l        = read[0] + read[1];
                out.r        = read[2];
                break;
        }
    }

    void UpdateDelayTime()
    {
        if(time_fade_ < 1.f || requested_delay_ == delay_)
            return;
        const float diff = static_cast<float>(requested_delay_)
                           - static_cast<float>(delay_);
        const float tol
            = static_cast<float>(delay_) * cfg_.time_tolerance;
        if(diff < tol && diff > -tol)
            return;
        old_delay_ = delay_;
        delay_     = requested_delay_;
        time_fade_ = 0.f;
    }

    float ReadLine(int i) const
    {
        const float now = lines_[i][ReadIndex(delay_)];
        if(time_fade_ >= 1.f)
            return now;
        const float old = lines_[i][ReadIndex(old_delay_)];
        return old + (now - old) * time_fade_;
    }

    size_t ReadIndex(uint32_t delay) const
    {
        return write_ >= delay ? write_ - delay
                               : write_ + cfg_.line_len - delay;
    }

    // Transposed direct form II
    float HighPass(int i, float x)
    {
        const float y = b0_ * x + z1_[i];
        z1_[i]        = b1_ * x - a1_ * y + z2_[i];
        z2_[i]        = b2_ * x - a2_ * y;
        return y;
    }

    uint32_t Clamp(uint32_t samples) const
    {
        const uint32_t max_delay = static_cast<uint32_t>(cfg_.line_len) - 1;
        return samples < 1 ? 1 : samples > max_delay ? max_delay : samples;
    }

    static float FadeStep(float samples)
    {
        return samples <= 1.f ? 1.f : 1.f / samples;
    }

    Config   cfg_;
    float*   lines_[kNumLines];
    size_t   write_;
    float    level_coef_, time_step_, route_step_;
    float    b0_, b1_, b2_, a1_, a2_;
    float    z1_[kNumLines], z2_[kNumLines];
    float    send_target_, return_target_, feedback_target_;
    float    send_, return_, feedback_;
    uint32_t delay_, old_delay_, requested_delay_;
    float    time_fade_;
    Pattern  pattern_, old_pattern_;
    float    route_fade_;
};
