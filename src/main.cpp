// Versio Reverse — clock-synced reverse for Noise Engineering Versio.
// See docs/design.md for the specification.

#include "daisy_versio.h"

#include "clock_tracker.h"
#include "comb_filter.h"
#include "config.h"
#include "controls.h"
#include "reverse_engine.h"

using namespace daisy;

namespace
{
constexpr size_t kBufferLen
    = static_cast<size_t>(config::kBufferSeconds * config::kSampleRate);
constexpr uint32_t kClockFlashMs = 30;

DaisyVersio hw;

DSY_SDRAM_BSS float buffer_l[kBufferLen];
DSY_SDRAM_BSS float buffer_r[kBufferLen];

ReverseEngine engine;
ClockTracker  clock_tracker;
ModeSwitch    mode_switch;
OutputSwitch  output_switch;
CombFilter    comb;

// Shared with the main loop for the LEDs.
volatile bool  led_reverse      = false;
volatile float led_offset_ms    = 0.f;
volatile bool  led_has_clock    = false;
volatile bool  led_real_flash   = false;
volatile bool  led_virtual_flash = false;

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.ProcessAllControls();

    const bool reverse
        = mode_switch.Process(hw.GetKnobValue(DaisyVersio::KNOB_0));
    const float offset_ms = KnobToOffsetMs(hw.GetKnobValue(DaisyVersio::KNOB_1),
                                           config::kOffsetMaxMs,
                                           config::kOffsetDeadZone);
    const int32_t offset
        = static_cast<int32_t>(offset_ms * config::kSampleRate / 1000.f);

    comb.SetMix(hw.GetKnobValue(DaisyVersio::KNOB_2));
    comb.SetDelayMs(KnobToCombDelayMs(hw.GetKnobValue(DaisyVersio::KNOB_3),
                                      config::kCombMinDelayMs,
                                      config::kCombMaxDelayMs));

    for(size_t i = 0; i < size; i++)
    {
        const bool gate = hw.Gate() != config::kGateInvert;
        const ClockTracker::Event ev = clock_tracker.Process(gate);
        if(ev != ClockTracker::Event::None)
        {
            engine.OnClock(offset, ev != ClockTracker::Event::Reanchor);
            if(ev == ClockTracker::Event::Virtual)
                led_virtual_flash = true;
            else
                led_real_flash = true;
        }

        float wet_l, wet_r;
        engine.Process(in[0][i], in[1][i], &wet_l, &wet_r);

        output_switch.Tick(reverse);
        const float sw_l = output_switch.Mix(in[0][i], wet_l);
        const float sw_r = output_switch.Mix(in[1][i], wet_r);

        float comb_l, comb_r;
        comb.Process(sw_l, sw_r, &comb_l, &comb_r);
        out[0][i] = SoftClip(comb_l, config::kSoftClipThreshold);
        out[1][i] = SoftClip(comb_r, config::kSoftClipThreshold);
    }

    led_reverse   = reverse;
    led_offset_ms = offset_ms;
    led_has_clock = clock_tracker.HasRealClock();
}

void UpdateLeds()
{
    static uint32_t flash_until = 0;
    static bool     flash_real  = false;

    const uint32_t now = System::GetNow();
    if(led_real_flash || led_virtual_flash)
    {
        flash_real        = led_real_flash;
        led_real_flash    = false;
        led_virtual_flash = false;
        flash_until       = now + kClockFlashMs;
    }
    const bool flashing = static_cast<int32_t>(flash_until - now) > 0;

    // LED0: mode
    hw.SetLed(DaisyVersio::LED_0, led_reverse ? 1.f : 0.f, 0.f, 0.f);

    // LED1: segment start (real = white, virtual = yellow)
    if(!flashing)
        hw.SetLed(DaisyVersio::LED_1, 0.f, 0.f, 0.f);
    else if(flash_real)
        hw.SetLed(DaisyVersio::LED_1, 1.f, 1.f, 1.f);
    else
        hw.SetLed(DaisyVersio::LED_1, 1.f, 1.f, 0.f);

    // LED2: offset (negative = blue, positive = green, brightness = amount)
    const float d   = led_offset_ms;
    const float amt = (d < 0.f ? -d : d) / config::kOffsetMaxMs;
    hw.SetLed(DaisyVersio::LED_2, 0.f, d > 0.f ? amt : 0.f, d < 0.f ? amt : 0.f);

    // LED3: dim orange until the first real clock arrives
    if(led_has_clock)
        hw.SetLed(DaisyVersio::LED_3, 0.f, 0.f, 0.f);
    else
        hw.SetLed(DaisyVersio::LED_3, 0.3f, 0.08f, 0.f);

    hw.UpdateLeds();
}
} // namespace

int main(void)
{
    hw.Init();

    engine.Init(buffer_l,
                buffer_r,
                kBufferLen,
                config::MsToSamples(config::kSegmentFadeMs));

    ClockTracker::Config clock_cfg;
    clock_cfg.default_interval
        = config::MsToSamples(config::kClockDefaultIntervalMs);
    clock_cfg.max_interval   = config::MsToSamples(config::kClockMaxIntervalMs);
    clock_cfg.min_interval   = config::MsToSamples(config::kClockMinIntervalMs);
    clock_cfg.debounce       = config::MsToSamples(config::kClockDebounceMs);
    clock_cfg.reanchor_ratio = config::kClockReanchorRatio;
    clock_tracker.Init(clock_cfg);

    mode_switch.Init(config::kModeOnThreshold, config::kModeOffThreshold);
    output_switch.Init(config::MsToSamples(config::kModeFadeMs));

    CombFilter::Config comb_cfg;
    comb_cfg.sample_rate          = config::kSampleRate;
    comb_cfg.hpf_hz               = config::kCombHpfHz;
    comb_cfg.stereo_spread_ms     = config::kCombStereoSpreadMs;
    comb_cfg.min_channel_delay_ms = config::kCombMinChannelDelayMs;
    comb_cfg.delay_smooth_ms      = config::kCombDelaySmoothMs;
    comb_cfg.mix_smooth_ms        = config::kCombMixSmoothMs;
    comb.Init(comb_cfg);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    while(1)
    {
        UpdateLeds();
    }
}
