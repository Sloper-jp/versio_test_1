// Host-side tests for the hardware independent parts of the firmware.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clock_tracker.h"
#include "comb_filter.h"
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

// ----------------------------------------------------------------- comb ---

static CombFilter MakeComb(float mix, float delay_ms)
{
    CombFilter::Config cfg;
    cfg.sample_rate          = 48000.f;
    cfg.hpf_hz               = 500.f;
    cfg.stereo_spread_ms     = 0.5f;
    cfg.min_channel_delay_ms = 0.05f;
    cfg.delay_smooth_ms      = 20.f;
    cfg.mix_smooth_ms        = 5.f;
    CombFilter c;
    c.Init(cfg);
    c.SetMix(mix);
    c.SetDelayMs(delay_ms);
    c.SnapParameters();
    return c;
}

// Reference: same biquad in double precision.
static std::vector<double> RefHighPass(const std::vector<float>& x)
{
    const double w0 = 2.0 * M_PI * 500.0 / 48000.0, cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::sqrt(0.5)), a0 = 1.0 + alpha;
    const double b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
    const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
    std::vector<double> y(x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for(size_t n = 0; n < x.size(); n++)
    {
        y[n] = b0 * x[n] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x[n]; y2 = y1; y1 = y[n];
    }
    return y;
}

static std::vector<float> Noise(size_t n)
{
    std::vector<float> x(n);
    uint32_t           r = 12345;
    for(auto& v : x)
    {
        r = r * 1664525u + 1013904223u;
        v = (static_cast<float>(r >> 8) / 16777216.f - 0.5f);
    }
    return x;
}

// Runs the comb and compares with y = l + g (h + w h(n - D)) computed from
// the reference high-pass, for integer delays D (exact through Hermite).
static double CombError(float mix, float delay_ms, int dl, int dr, double w)
{
    CombFilter c = MakeComb(mix, delay_ms);
    auto       x = Noise(2000);
    auto       h = RefHighPass(x);
    const double g = 1.0 / std::sqrt(1.0 + w * w);
    double err = 0;
    for(size_t n = 0; n < x.size(); n++)
    {
        float yl, yr;
        c.Process(x[n], x[n], &yl, &yr);
        const double hl = n >= size_t(dl) ? h[n - dl] : 0.0;
        const double hr = n >= size_t(dr) ? h[n - dr] : 0.0;
        const double el = (x[n] - h[n]) + g * (h[n] + w * hl);
        const double er = (x[n] - h[n]) + g * (h[n] + w * hr);
        err = std::fmax(err, std::fmax(std::fabs(yl - el), std::fabs(yr - er)));
    }
    return err;
}

static void TestCombBypassAtZero()
{
    CombFilter c = MakeComb(0.f, 3.f);
    auto       x = Noise(2000);
    double     err = 0;
    for(float v : x)
    {
        float yl, yr;
        c.Process(v, -v, &yl, &yr);
        err = std::fmax(err, std::fmax(std::fabs(yl - v), std::fabs(yr + v)));
    }
    CHECK(err < 1e-6); // 0%: dry 100%
}

static void TestCombHalfMix()
{
    // 50%: w = 1, same delay both sides (1 ms = 48 samples)
    CHECK(CombError(0.5f, 1.f, 48, 48, 1.0) < 1e-4);
    // 25%: w = 0.5
    CHECK(CombError(0.25f, 1.f, 48, 48, 0.5) < 1e-4);
}

static void TestCombStereoSpread()
{
    // 100%: w = 1, L/R = 1 ms -/+ 0.25 ms = 36 / 60 samples
    CHECK(CombError(1.f, 1.f, 36, 60, 1.0) < 1e-4);
    // 75%: spread halved -> 42 / 54
    CHECK(CombError(0.75f, 1.f, 42, 54, 1.0) < 1e-4);
}

static void TestCombFractionalDelay()
{
    // 10.5 samples through Hermite interpolation, checked with a sine
    // at 1 kHz: the 10.5-sample result must lie halfway between 10 and 11).
    const float delay_ms = 10.5f / 48.f;
    CombFilter  a = MakeComb(0.5f, delay_ms);
    CombFilter  b = MakeComb(0.5f, 10.f / 48.f);
    CombFilter  d = MakeComb(0.5f, 11.f / 48.f);
    double      max_ab = 0, max_between = 0;
    for(int n = 0; n < 4000; n++)
    {
        const float x = std::sin(2.f * 3.14159265f * 1000.f * n / 48000.f);
        float ya, yb, yd, t;
        a.Process(x, x, &ya, &t);
        b.Process(x, x, &yb, &t);
        d.Process(x, x, &yd, &t);
        if(n > 2000)
        {
            // Output with 10.5 must sit between (≈ average of) 10 and 11.
            max_between = std::fmax(max_between, std::fabs(ya - 0.5 * (yb + yd)));
            max_ab      = std::fmax(max_ab, std::fabs(ya));
        }
    }
    CHECK(max_ab > 0.1);
    CHECK(max_between < 0.01);
}

static void TestCombLowBandUntouched()
{
    // 60 Hz sine at 50% mix: level stays within 0.5 dB of the input.
    CombFilter c = MakeComb(0.5f, 2.f);
    double     in_pk = 0, out_pk = 0;
    for(int n = 0; n < 48000; n++)
    {
        const float x = 0.5f * std::sin(2.f * 3.14159265f * 60.f * n / 48000.f);
        float yl, yr;
        c.Process(x, x, &yl, &yr);
        if(n > 24000)
        {
            in_pk  = std::fmax(in_pk, std::fabs(x));
            out_pk = std::fmax(out_pk, std::fabs(yl));
        }
    }
    CHECK(std::fabs(20.0 * std::log10(out_pk / in_pk)) < 0.5);
}

static void TestCombPeakBound()
{
    // Worst case at 50%: a high-band comb peak (1 ms delay -> peaks every
    // 1 kHz) is +3 dB, not +6 dB.
    CombFilter c = MakeComb(0.5f, 1.f);
    double     pk = 0;
    for(int n = 0; n < 48000; n++)
    {
        const float x = 0.5f * std::sin(2.f * 3.14159265f * 4000.f * n / 48000.f);
        float yl, yr;
        c.Process(x, x, &yl, &yr);
        if(n > 24000)
            pk = std::fmax(pk, std::fabs(yl));
    }
    const double db = 20.0 * std::log10(pk / 0.5);
    CHECK(db > 2.5 && db < 3.3);
}

static void TestCombDelaySmoothing()
{
    CombFilter c = MakeComb(0.5f, 0.1f);
    c.SetDelayMs(8.f); // big jump must not produce spikes
    auto   x  = Noise(9600);
    double pk = 0;
    for(float v : x)
    {
        float yl, yr;
        c.Process(0.3f * v, 0.3f * v, &yl, &yr);
        pk = std::fmax(pk, std::fabs(yl));
    }
    CHECK(pk < 0.5);
}

static void TestSoftClip()
{
    CHECK(SoftClip(0.5f, 0.708f) == 0.5f);
    CHECK(SoftClip(-0.708f, 0.708f) == -0.708f);
    CHECK(SoftClip(10.f, 0.708f) <= 1.f);
    CHECK(SoftClip(-10.f, 0.708f) >= -1.f);
    CHECK(SoftClip(1.f, 0.708f) > 0.708f);
    // Continuous slope at the threshold
    const float e = 1e-3f;
    CHECK(std::fabs((SoftClip(0.708f + e, 0.708f) - 0.708f) / e - 1.f) < 0.01f);
}

static void TestCombDelayKnob()
{
    CHECK(std::fabs(KnobToCombDelayMs(0.f, 0.1f, 8.f) - 0.1f) < 1e-6f);
    CHECK(std::fabs(KnobToCombDelayMs(1.f, 0.1f, 8.f) - 8.f) < 1e-4f);
    CHECK(std::fabs(KnobToCombDelayMs(0.5f, 0.1f, 8.f) - 0.894427f) < 1e-4f);
    CHECK(KnobToCombDelayMs(1.5f, 0.1f, 8.f) <= 8.0001f);
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
    TestCombBypassAtZero();
    TestCombHalfMix();
    TestCombStereoSpread();
    TestCombFractionalDelay();
    TestCombLowBandUntouched();
    TestCombPeakBound();
    TestCombDelaySmoothing();
    TestSoftClip();
    TestCombDelayKnob();
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
