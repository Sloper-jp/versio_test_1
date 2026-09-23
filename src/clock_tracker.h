#pragma once
#include <cstdint>

// Real/virtual clock management (docs/design.md §3.5). Hardware independent.
//
// Call Process() once per sample with the gate level. It returns an event
// when a new reverse segment should start at this sample.
class ClockTracker
{
  public:
    enum class Event
    {
        None,
        Real,     // rules 1 & 2: a real clock edge starts a segment
        Virtual,  // rules 0 & 3: no real clock within T, keep running at T
        Reanchor, // rule 4: real clock shortly after a virtual one
    };

    struct Config
    {
        uint32_t default_interval; // samples
        uint32_t max_interval;
        uint32_t min_interval;
        uint32_t debounce;
        float    reanchor_ratio;
    };

    void Init(const Config& cfg)
    {
        cfg_                 = cfg;
        interval_            = cfg.default_interval;
        since_segment_       = 0;
        since_real_          = 0;
        since_edge_          = cfg.debounce + 1;
        has_real_            = false;
        segment_was_virtual_ = true;
        prev_gate_           = false;
    }

    Event Process(bool gate)
    {
        const bool rising = gate && !prev_gate_;
        prev_gate_        = gate;

        // Counters hold the samples elapsed since each event at this sample;
        // they advance on return.
        Event ev = Event::None;
        if(rising && since_edge_ > cfg_.debounce)
        {
            since_edge_ = 0;

            // Rule 4 uses the interval that produced the virtual clock.
            const bool reanchor
                = segment_was_virtual_
                  && since_segment_ <= static_cast<uint32_t>(
                         interval_ * cfg_.reanchor_ratio);

            // Rule 1: short enough -> adopt. Rule 2: too long (or first
            // clock) -> keep T; the next clock measures it again.
            if(has_real_ && since_real_ <= cfg_.max_interval)
                interval_ = since_real_ < cfg_.min_interval ? cfg_.min_interval
                                                            : since_real_;

            has_real_            = true;
            since_real_          = 0;
            since_segment_       = 0;
            segment_was_virtual_ = false;
            ev = reanchor ? Event::Reanchor : Event::Real;
        }
        else if(since_segment_ >= interval_)
        {
            since_segment_       = 0;
            segment_was_virtual_ = true;
            ev                   = Event::Virtual;
        }

        Tick(since_segment_);
        Tick(since_real_);
        Tick(since_edge_);
        return ev;
    }

    uint32_t Interval() const { return interval_; }
    bool     HasRealClock() const { return has_real_; }

  private:
    static void Tick(uint32_t& c)
    {
        if(c != UINT32_MAX)
            c++;
    }

    Config   cfg_;
    uint32_t interval_;
    uint32_t since_segment_;
    uint32_t since_real_;
    uint32_t since_edge_;
    bool     has_real_;
    bool     segment_was_virtual_;
    bool     prev_gate_;
};
