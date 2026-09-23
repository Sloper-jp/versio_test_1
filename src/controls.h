#pragma once
#include <cstddef>

// Knob/CV -> parameter conversion and the dry/wet switch (docs/design.md
// §3.1, §3.3, §3.6). Hardware independent.

// 50% threshold with hysteresis.
class ModeSwitch
{
  public:
    void Init(float on_threshold, float off_threshold)
    {
        on_th_  = on_threshold;
        off_th_ = off_threshold;
        on_     = false;
    }

    bool Process(float v)
    {
        if(!on_ && v >= on_th_)
            on_ = true;
        else if(on_ && v <= off_th_)
            on_ = false;
        return on_;
    }

    bool On() const { return on_; }

  private:
    float on_th_, off_th_;
    bool  on_;
};

// Knob 0..1 -> offset in ms, centre = 0 with a dead zone.
inline float KnobToOffsetMs(float v, float max_ms, float dead_zone)
{
    const float u   = v - 0.5f;
    const float mag = u < 0.f ? -u : u;
    if(mag <= dead_zone)
        return 0.f;
    float x = (mag - dead_zone) / (0.5f - dead_zone);
    if(x > 1.f)
        x = 1.f;
    return (u < 0.f ? -x : x) * max_ms;
}

// Dry/wet selector with a short linear fade so the switch does not click.
class OutputSwitch
{
  public:
    void Init(size_t fade_samples)
    {
        step_ = 1.f / static_cast<float>(fade_samples < 1 ? 1 : fade_samples);
        gain_ = 0.f;
    }

    // Advance the fade once per sample, then apply Mix() to each channel.
    void Tick(bool reverse)
    {
        const float target = reverse ? 1.f : 0.f;
        if(gain_ < target)
            gain_ = gain_ + step_ > target ? target : gain_ + step_;
        else if(gain_ > target)
            gain_ = gain_ - step_ < target ? target : gain_ - step_;
    }

    float Mix(float dry, float wet) const { return dry + (wet - dry) * gain_; }

  private:
    float step_;
    float gain_;
};
