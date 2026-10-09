#pragma once

#include "wiiuport/title/DrawInterpolation.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's sea waves and sky clouds in the in-between paint.
//
// The weather effects move them in execute (`wave_move`, `vrkumo_move`) and the paint draws them
// from two packets the environment holds. When the in-between paint opens, each one that continues
// from the last tick is set to the midpoint of its last two ticks; the tick's own values are back
// when it closes.
class EnvironmentInterpolation final : public DrawInterpolation::MidPaintListener {
  public:
    // g_env_light (dScnKy_env_light_c), TWW's layout 0x80 later.
    static constexpr uint32_t kEnvLight = 0x10475a68;
    static constexpr uint32_t kWaveSpawnRadius = 0x9e0;
    // mWaveCount, s16 in the high half.
    static constexpr uint32_t kWaveCount = 0x9f8;
    static constexpr uint32_t kCloudPacket = 0xa94;
    static constexpr uint32_t kWavePacket = 0xaa0;

    // dKankyo_wave_Packet: WAVE_EFF mEff[300] at +0xa0.
    static constexpr uint32_t kWaves = 0xa0;
    static constexpr uint32_t kWaveWords = 0x38 / 4;
    static constexpr uint32_t kMaxWaves = 300;
    // WAVE_EFF, in words: mPos, mBasePos, mCounter, mAlpha, then mStatus (s8, high byte).
    static constexpr size_t kWavePos = 0;
    static constexpr size_t kWaveBase = 3;
    static constexpr size_t kWaveCounter = 9;
    static constexpr size_t kWaveAlpha = 10;
    static constexpr size_t kWaveStatus = 13;

    // dKankyo_vrkumo_Packet: VRKUMO_EFF mInst[100] at +0xa4.
    static constexpr uint32_t kClouds = 0xa4;
    static constexpr uint32_t kCloudWords = 0x2c / 4;
    static constexpr uint32_t kCloudCount = 100;
    // VRKUMO_EFF, in words: mStatus (s8, high byte), mPosition, mAlpha.
    static constexpr size_t kCloudStatus = 0;
    static constexpr size_t kCloudPosition = 1;
    static constexpr size_t kCloudAlpha = 8;
    // vrkumo_move wraps a cloud past this distance from the centre.
    static constexpr float kCloudWrapRadius = 15000.0f;

    struct Seams {
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit EnvironmentInterpolation(Seams seams);

    void onMidPaintBegin(uint64_t tick) override;
    void onMidPaintEnd() override;

    std::string json() const;

  private:
    // One packet's entries as a tick left them.
    struct Seen {
        uint64_t tick = 0;
        uint32_t packet = 0;
        std::vector<uint32_t> words;
    };

    struct Restore {
        uint32_t address = 0;
        std::vector<uint32_t> words;
    };

    // The wave packet, how many waves are in use, and the radius they respawn within.
    struct Waves {
        uint32_t packet = 0;
        uint32_t count = 0;
        float spawnRadius = 0.0f;
    };

    void blendWaves(Waves waves);
    void blendClouds(uint32_t packet);
    // Writes the entries that changed, keeping the tick's own to put back.
    void write(uint32_t address, const std::vector<uint32_t>& blended,
               const std::vector<uint32_t>& original);

    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;

    mutable std::mutex m_mutex;
    uint64_t m_tick = 0;
    Seen m_waves;
    Seen m_clouds;
    std::vector<Restore> m_restores;

    uint64_t m_waveBlends = 0;
    uint64_t m_wavesRespawned = 0;
    uint64_t m_cloudBlends = 0;
    uint64_t m_cloudsWrapped = 0;
    uint64_t m_unblendable = 0;
    uint64_t m_unreadable = 0;
    uint64_t m_writeFailures = 0;
    uint64_t m_restored = 0;
};

} // namespace wiiuport::title
