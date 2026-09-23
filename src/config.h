#pragma once

// Tunable constants. See docs/design.md §9 for the rationale of each value.
namespace config
{
constexpr float kSampleRate = 48000.f;

// Audio buffer (§3.4)
constexpr float kBufferSeconds = 6.0f;

// Offset d (§3.3)
constexpr float kOffsetMaxMs   = 350.f;
constexpr float kOffsetDeadZone = 0.03f; // ±3% of the knob range around centre

// Mode switch (§3.6)
constexpr float kModeOnThreshold  = 0.52f;
constexpr float kModeOffThreshold = 0.48f;

// Fades (§3.1, §3.2)
constexpr float kModeFadeMs    = 2.f;
constexpr float kSegmentFadeMs = 5.f;

// Clock (§3.5)
constexpr float kClockMaxIntervalMs     = 600.f; // BPM 50 @ 2PPQN
constexpr float kClockMinIntervalMs     = 20.f;
constexpr float kClockDefaultIntervalMs = 500.f; // BPM 60 @ 2PPQN, used before the first clock
constexpr float kClockDebounceMs        = 2.f;
constexpr float kClockReanchorRatio     = 0.2f;

// Gate input polarity. DaisyVersio::Gate() should already return true while
// the jack is high; flip this if hardware testing shows otherwise.
constexpr bool kGateInvert = false;

constexpr int MsToSamples(float ms)
{
    return static_cast<int>(ms * kSampleRate / 1000.f + 0.5f);
}
} // namespace config
