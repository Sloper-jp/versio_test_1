// Host-side tests for the hardware independent parts of the firmware.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clock_tracker.h"
#include "controls.h"
#include "reverse_engine.h"

static int failures = 0;

#define CHECK(cond)                                                   \
    do                                                                \
    {                                                                 \
        if(!(cond))                                                   \
        {                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                               \
        }                                                             \
    } while(0)

// ---------------------------------------------------------------- clock ---

using Ev = ClockTracker::Event;

static ClockTracker MakeClock()
{
    ClockTracker::Config cfg;
    cfg.default_interval = 500;
    cfg.max_interval     = 600;
    cfg.min_interval     = 20;
    cfg.debounce         = 2;
    cfg.reanchor_ratio   = 0.2f;
    ClockTracker c;
    c.Init(cfg);
    return c;
}

// Runs the tracker from sample `from` up to (excluding) `to`, with 1-sample
// gate pulses at `pulses`, and records every event.
struct Rec
{
    uint32_t t;
    Ev       ev;
};
static std::vector<Rec> Run(ClockTracker&                c,
                            uint32_t                     from,
                            uint32_t                     to,
                            const std::vector<uint32_t>& pulses)
{
    std::vector<Rec> out;
    for(uint32_t t = from; t < to; t++)
    {
        bool g = false;
        for(uint32_t p : pulses)
            g = g || p == t;
        Ev e = c.Process(g);
        if(e != Ev::None)
            out.push_back({t, e});
    }
    return out;
}

static void TestClockFreeRunAtStartup()
{
    ClockTracker c = MakeClock();
    auto         r = Run(c, 0, 1200, {});
    CHECK(r.size() == 2);
    CHECK(r[0].t == 500 && r[0].ev == Ev::Virtual);
    CHECK(r[1].t == 1000 && r[1].ev == Ev::Virtual);
    CHECK(!c.HasRealClock());
}

static void TestClockAdoptAndDropout()
{
    ClockTracker c = MakeClock();
    // First clock: phase reset only (rule 2), T stays 500.
    auto r = Run(c, 0, 301, {300});
    CHECK(r.size() == 1 && r[0].t == 300 && r[0].ev == Ev::Real);
    CHECK(c.Interval() == 500);
    // Second clock 400 later: adopted (rule 1).
    r = Run(c, 301, 701, {700});
    CHECK(r.size() == 1 && r[0].t == 700 && r[0].ev == Ev::Real);
    CHECK(c.Interval() == 400);
    // Dropout: virtual clocks every 400 (rule 3).
    r = Run(c, 701, 1600, {});
    CHECK(r.size() == 2);
    CHECK(r[0].t == 1100 && r[0].ev == Ev::Virtual);
    CHECK(r[1].t == 1500 && r[1].ev == Ev::Virtual);
    // Recovery 1000 after the last real clock: T kept, phase reset (rule 2).
    r = Run(c, 1600, 1701, {1700});
    CHECK(r.size() == 1 && r[0].t == 1700 && r[0].ev == Ev::Real);
    CHECK(c.Interval() == 400);
    // Next clock decides T again.
    r = Run(c, 1701, 2051, {2050});
    CHECK(r.size() == 1 && r[0].t == 2050 && r[0].ev == Ev::Real);
    CHECK(c.Interval() == 350);
}

static void TestClockLongIntervalNotAdopted()
{
    ClockTracker c = MakeClock();
    Run(c, 0, 101, {100});
    Run(c, 101, 401, {400}); // T = 300
    CHECK(c.Interval() == 300);
    // 650 > 600: not adopted even without dropout virtual clocks in between.
    auto r = Run(c, 401, 1051, {1050});
    CHECK(c.Interval() == 300);
    CHECK(r.back().t == 1050);
}

static void TestClockReanchor()
{
    ClockTracker c = MakeClock();
    Run(c, 0, 101, {100});
    Run(c, 101, 501, {500}); // T = 400
    // Tempo slows slightly: virtual at 900, real at 950 (within 20% of 400).
    auto r = Run(c, 501, 951, {950});
    CHECK(r.size() == 2);
    CHECK(r[0].t == 900 && r[0].ev == Ev::Virtual);
    CHECK(r[1].t == 950 && r[1].ev == Ev::Reanchor);
    CHECK(c.Interval() == 450);
    // Outside the window: a normal real clock.
    ClockTracker c2 = MakeClock();
    Run(c2, 0, 101, {100});
    Run(c2, 101, 501, {500});
    r = Run(c2, 501, 1001, {1000});
    CHECK(r.back().t == 1000 && r.back().ev == Ev::Real);
}

static void TestClockDebounceAndMinInterval()
{
    ClockTracker c = MakeClock();
    Run(c, 0, 101, {100});
    // Edge 2 samples later (low in between) is chatter and ignored.
    auto r = Run(c, 101, 103, {102});
    CHECK(r.empty());
    // An edge 10 samples after the first real one: T clamps to 20.
    r = Run(c, 103, 111, {110});
    CHECK(r.size() == 1 && r[0].ev == Ev::Real);
    CHECK(c.Interval() == 20);
}

// --------------------------------------------------------------- engine ---

struct EngineFixture
{
    static constexpr size_t kLen = 1000;
    std::vector<float>      l, r;
    ReverseEngine           e;
    uint32_t                t = 0;
    std::vector<float>      out;

    EngineFixture() : l(kLen), r(kLen) { e.Init(l.data(), r.data(), kLen, 1); }

    static float X(uint32_t n) { return static_cast<float>(n + 1); }

    void RunTo(uint32_t end)
    {
        for(; t < end; t++)
            Step();
    }
    void Step()
    {
        float ol, orr;
        e.Process(X(t), -X(t), &ol, &orr);
        CHECK(orr == -ol);
        out.push_back(ol);
    }
};

static void TestEngineZeroOffset()
{
    EngineFixture f;
    f.RunTo(200);
    f.e.OnClock(0);
    f.RunTo(400);
    bool ok = true;
    for(uint32_t n = 201; n < 400; n++)
        ok = ok && f.out[n] == EngineFixture::X(400 - n);
    CHECK(ok); // y(t_k + tau) = x(t_k - tau)
}

static void TestEnginePositiveOffset()
{
    EngineFixture f;
    f.RunTo(200);
    f.e.OnClock(50);
    f.RunTo(450);
    // Segment starts 50 later, at the clock position.
    bool ok = true;
    for(uint32_t n = 251; n < 450; n++)
        ok = ok && f.out[n] == EngineFixture::X(450 - n);
    CHECK(ok);
    // Before the switch the old (initial) head is still playing.
    CHECK(f.out[240] != EngineFixture::X(450 - 240));
}

static void TestEngineNegativeOffset()
{
    EngineFixture f;
    f.RunTo(200);
    f.e.OnClock(-50);
    f.RunTo(400);
    bool ok = true;
    for(uint32_t n = 201; n < 350; n++)
        ok = ok && f.out[n] == EngineFixture::X(350 - n);
    CHECK(ok);
}

static void TestEngineWrapAround()
{
    EngineFixture f;
    f.RunTo(2010);
    f.e.OnClock(-50); // start index = 2010 - 50 -> wraps inside the buffer
    f.RunTo(2300);
    bool ok = true;
    for(uint32_t n = 2011; n < 2300; n++)
        ok = ok && f.out[n] == EngineFixture::X(3970 - n);
    CHECK(ok);
}

static void TestEngineSupersede()
{
    EngineFixture f;
    f.RunTo(200);
    f.e.OnClock(100); // would start at 300
    f.RunTo(220);
    f.e.OnClock(-10); // starts now, supersedes the pending one
    f.RunTo(400);
    bool ok = true;
    for(uint32_t n = 221; n < 400; n++)
        ok = ok && f.out[n] == EngineFixture::X(430 - n);
    CHECK(ok); // no jump at 300
}

static void TestEngineReanchorKeepsOffset()
{
    EngineFixture f;
    f.RunTo(200);
    f.e.OnClock(-50);
    f.RunTo(220);
    f.e.OnClock(0, false); // re-anchor: keep d = -50
    f.RunTo(400);
    bool ok = true;
    for(uint32_t n = 221; n < 390; n++)
        ok = ok && f.out[n] == EngineFixture::X(390 - n);
    CHECK(ok);
}

static void TestEngineCrossfade()
{
    ReverseEngine      e;
    std::vector<float> l(1000), r(1000);
    e.Init(l.data(), r.data(), 1000, 8);
    float o, o2;
    for(int i = 0; i < 100; i++)
        e.Process(1.f, 1.f, &o, &o2);
    e.OnClock(0);
    for(int i = 0; i < 50; i++)
        e.Process(1.f, 1.f, &o, &o2);
    e.OnClock(0);
    // Old and new heads both read 1.0: equal-power sum is >= 1, never a gap.
    bool ok = true;
    for(int i = 0; i < 8; i++)
    {
        e.Process(1.f, 1.f, &o, &o2);
        ok = ok && o >= 0.999f && o <= 1.415f;
    }
    CHECK(ok);
}

// ---------------------------------------------------------- integration ---

// Impulses on every clock; the reversed impulse must land on the next clock
// shifted by d (design §3.3).
static void TestTransientAlignment(int32_t d)
{
    const uint32_t     period = 400, len = 48000;
    std::vector<float> l(len), r(len);
    ReverseEngine      e;
    e.Init(l.data(), r.data(), len, 240);
    ClockTracker c = MakeClock();

    std::vector<uint32_t> peaks;
    for(uint32_t t = 0; t < 20 * period; t++)
    {
        const bool beat = t % period == 0;
        Ev         ev   = c.Process(beat);
        if(ev != Ev::None)
            e.OnClock(d, ev != Ev::Reanchor);
        float ol, orr;
        e.Process(beat ? 1.f : 0.f, 0.f, &ol, &orr);
        if(ol > 0.5f && t > 5 * period)
            peaks.push_back(t);
    }
    bool ok = !peaks.empty();
    for(uint32_t p : peaks)
        ok = ok && (p + period - static_cast<uint32_t>((d % 400 + 400) % 400)) % period == 0;
    if(!ok || getenv("VERBOSE"))
        std::printf("  d=%d: %zu peaks, first at %u\n", d, peaks.size(), peaks.empty() ? 0 : peaks[0]);
    CHECK(ok);
}

// ------------------------------------------------------------- controls ---

static void TestModeSwitchHysteresis()
{
    ModeSwitch m;
    m.Init(0.52f, 0.48f);
    CHECK(!m.Process(0.50f));
    CHECK(!m.Process(0.519f));
    CHECK(m.Process(0.52f));
    CHECK(m.Process(0.49f));
    CHECK(!m.Process(0.48f));
}

static void TestOffsetKnob()
{
    CHECK(KnobToOffsetMs(0.5f, 350.f, 0.03f) == 0.f);
    CHECK(KnobToOffsetMs(0.525f, 350.f, 0.03f) == 0.f);
    CHECK(KnobToOffsetMs(0.475f, 350.f, 0.03f) == 0.f);
    CHECK(std::fabs(KnobToOffsetMs(1.f, 350.f, 0.03f) - 350.f) < 1e-3f);
    CHECK(std::fabs(KnobToOffsetMs(0.f, 350.f, 0.03f) + 350.f) < 1e-3f);
    CHECK(KnobToOffsetMs(0.6f, 350.f, 0.03f) > 0.f);
    CHECK(KnobToOffsetMs(0.4f, 350.f, 0.03f) < 0.f);
}

static void TestOutputSwitchFade()
{
    OutputSwitch s;
    s.Init(4);
    CHECK(s.Mix(1.f, 0.f) == 1.f);
    for(int i = 0; i < 2; i++)
        s.Tick(true);
    CHECK(std::fabs(s.Mix(1.f, 0.f) - 0.5f) < 1e-6f);
    for(int i = 0; i < 2; i++)
        s.Tick(true);
    CHECK(s.Mix(1.f, 0.f) == 0.f); // fully wet: dry 0%
    for(int i = 0; i < 4; i++)
        s.Tick(false);
    CHECK(s.Mix(1.f, 0.f) == 1.f);
}

int main()
{
    TestClockFreeRunAtStartup();
    TestClockAdoptAndDropout();
    TestClockLongIntervalNotAdopted();
    TestClockReanchor();
    TestClockDebounceAndMinInterval();
    TestEngineZeroOffset();
    TestEnginePositiveOffset();
    TestEngineNegativeOffset();
    TestEngineWrapAround();
    TestEngineSupersede();
    TestEngineReanchorKeepsOffset();
    TestEngineCrossfade();
    TestTransientAlignment(0);
    TestTransientAlignment(30);
    TestTransientAlignment(-30);
    TestModeSwitchHysteresis();
    TestOffsetKnob();
    TestOutputSwitchFade();

    if(failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
