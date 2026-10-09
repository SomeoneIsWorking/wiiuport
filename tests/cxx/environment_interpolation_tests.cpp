// The weather's waves and clouds in the in-between paint: each at the midpoint of its last two
// ticks.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/EnvironmentInterpolation.h"

#include <bit>
#include <cstdint>
#include <map>

namespace {

using wiiuport::title::EnvironmentInterpolation;

constexpr uint32_t kWavePacket = 0x3a000000;
constexpr uint32_t kCloudPacket = 0x3b000000;
constexpr uint32_t kFirstWave = kWavePacket + EnvironmentInterpolation::kWaves;
constexpr uint32_t kFirstCloud = kCloudPacket + EnvironmentInterpolation::kClouds;
constexpr uint32_t kSpawnRadius = 0x44fa0000; // 2000.0f

uint32_t word(float value) {
    return std::bit_cast<uint32_t>(value);
}

float value(uint32_t word) {
    return std::bit_cast<float>(word);
}

struct Wave {
    float x;
    float baseX;
    float counter;
    uint8_t status;
};

struct Cloud {
    float x;
    float alpha;
    uint8_t status;
};

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    EnvironmentInterpolation interpolation() {
        words[EnvironmentInterpolation::kEnvLight + EnvironmentInterpolation::kWavePacket] =
            kWavePacket;
        words[EnvironmentInterpolation::kEnvLight + EnvironmentInterpolation::kCloudPacket] =
            kCloudPacket;
        words[EnvironmentInterpolation::kEnvLight + EnvironmentInterpolation::kWaveCount] = 1U
                                                                                            << 16;
        words[EnvironmentInterpolation::kEnvLight + EnvironmentInterpolation::kWaveSpawnRadius] =
            kSpawnRadius;
        return EnvironmentInterpolation{
            {.readWords =
                 [this](uint32_t address, uint32_t* out, uint32_t count) {
                     for (uint32_t i = 0; i < count; ++i) {
                         out[i] = words[address + (4 * i)];
                     }
                     return true;
                 },
             .writeWords =
                 [this](uint32_t address, const uint32_t* in, uint32_t count) {
                     for (uint32_t i = 0; i < count; ++i) {
                         words[address + (4 * i)] = in[i];
                     }
                     return true;
                 }}};
    }

    void wave(Wave at) {
        uint32_t entry = kFirstWave;
        words[entry + (4 * EnvironmentInterpolation::kWavePos)] = word(at.x);
        words[entry + (4 * EnvironmentInterpolation::kWaveBase)] = word(at.baseX);
        words[entry + (4 * EnvironmentInterpolation::kWaveCounter)] = word(at.counter);
        words[entry + (4 * EnvironmentInterpolation::kWaveAlpha)] = word(1.0f);
        words[entry + (4 * EnvironmentInterpolation::kWaveStatus)] = uint32_t{at.status} << 24;
    }

    void cloud(Cloud at) {
        uint32_t entry = kFirstCloud;
        words[entry + (4 * EnvironmentInterpolation::kCloudStatus)] = uint32_t{at.status} << 24;
        words[entry + (4 * EnvironmentInterpolation::kCloudPosition)] = word(at.x);
        words[entry + (4 * EnvironmentInterpolation::kCloudAlpha)] = word(at.alpha);
    }

    float waveWord(size_t index) {
        return value(words[kFirstWave + (4 * index)]);
    }

    float cloudWord(size_t index) {
        return value(words[kFirstCloud + (4 * index)]);
    }
};

void aWaveAndACloudArePaintedAtTheirMidpointsAndPutBack() {
    Guest guest;
    EnvironmentInterpolation environment = guest.interpolation();
    guest.wave({.x = 0.0f, .baseX = 500.0f, .counter = 1.0f, .status = 1});
    guest.cloud({.x = 100.0f, .alpha = 0.25f, .status = 1});
    environment.onMidPaintBegin(1);
    check::isTrue(guest.waveWord(EnvironmentInterpolation::kWavePos) == 0.0f,
                  "a wave with no tick before it is painted as it is");
    environment.onMidPaintEnd();
    guest.wave({.x = 10.0f, .baseX = 500.0f, .counter = 2.0f, .status = 1});
    guest.cloud({.x = 120.0f, .alpha = 0.75f, .status = 1});
    environment.onMidPaintBegin(2);
    check::isTrue(guest.waveWord(EnvironmentInterpolation::kWavePos) == 5.0f &&
                      guest.waveWord(EnvironmentInterpolation::kWaveCounter) == 1.5f,
                  "the next one is painted between its two ticks, its animation too");
    check::isTrue(guest.waveWord(EnvironmentInterpolation::kWaveBase) == 500.0f,
                  "about the base it moves from");
    check::isTrue(guest.cloudWord(EnvironmentInterpolation::kCloudPosition) == 110.0f &&
                      guest.cloudWord(EnvironmentInterpolation::kCloudAlpha) == 0.5f,
                  "and a cloud between its two places, half faded in");
    environment.onMidPaintEnd();
    check::isTrue(guest.waveWord(EnvironmentInterpolation::kWavePos) == 10.0f &&
                      guest.cloudWord(EnvironmentInterpolation::kCloudPosition) == 120.0f,
                  "and the next draw phase sees the tick's own");
}

void aRespawnedWaveAndAWrappedCloudArePaintedAsTheyAre() {
    Guest guest;
    EnvironmentInterpolation environment = guest.interpolation();
    guest.wave({.x = 0.0f, .baseX = 500.0f, .counter = 1.0f, .status = 1});
    guest.cloud({.x = 15001.0f, .alpha = 0.5f, .status = 1});
    environment.onMidPaintBegin(1);
    environment.onMidPaintEnd();
    guest.wave({.x = 10.0f, .baseX = 900.0f, .counter = 2.0f, .status = 1});
    guest.cloud({.x = -15001.0f, .alpha = 0.0f, .status = 1});
    environment.onMidPaintBegin(2);
    check::isTrue(guest.waveWord(EnvironmentInterpolation::kWavePos) == 10.0f,
                  "a wave at a new base respawned there");
    check::isTrue(guest.cloudWord(EnvironmentInterpolation::kCloudPosition) == -15001.0f,
                  "and a cloud across the sky wrapped round");
    environment.onMidPaintEnd();
    guest.wave({.x = 2500.0f, .baseX = 900.0f, .counter = 3.0f, .status = 1});
    environment.onMidPaintBegin(3);
    check::isTrue(guest.waveWord(EnvironmentInterpolation::kWavePos) == 2500.0f,
                  "and a wave that moved further than its spawn radius wrapped");
    environment.onMidPaintEnd();
}

} // namespace

void wiiuport::tests::runEnvironmentInterpolationTests() {
    aWaveAndACloudArePaintedAtTheirMidpointsAndPutBack();
    aRespawnedWaveAndAWrappedCloudArePaintedAsTheyAre();
}
