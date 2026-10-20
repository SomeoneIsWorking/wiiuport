#include "wiiuport/title/EnvironmentInterpolation.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/title/JsonBody.h"

#include <bit>
#include <cmath>
#include <span>
#include <utility>

namespace wiiuport::title {

namespace {

float asFloat(uint32_t word) {
    return std::bit_cast<float>(word);
}

uint32_t statusOf(uint32_t word) {
    return word >> 24;
}

int16_t highHalf(uint32_t word) {
    return static_cast<int16_t>(word >> 16);
}

// The midpoint of `count` floats from two entries, written into `out`; false when one is not a
// number.
bool blendFloats(std::span<const uint32_t> from, std::span<const uint32_t> to,
                 std::span<uint32_t> out) {
    std::vector<float> a(from.size());
    std::vector<float> b(to.size());
    std::vector<float> mid(to.size());
    for (size_t i = 0; i < from.size(); ++i) {
        a[i] = asFloat(from[i]);
        b[i] = asFloat(to[i]);
    }
    if (!interp::midpoint(a, b, mid)) {
        return false;
    }
    for (size_t i = 0; i < mid.size(); ++i) {
        out[i] = std::bit_cast<uint32_t>(mid[i]);
    }
    return true;
}

float distance(std::span<const uint32_t> from, std::span<const uint32_t> to, size_t axes) {
    float sum = 0.0f;
    for (size_t i = 0; i < axes; ++i) {
        float delta = asFloat(to[i]) - asFloat(from[i]);
        sum += delta * delta;
    }
    return std::sqrt(sum);
}

} // namespace

EnvironmentInterpolation::EnvironmentInterpolation(Seams seams)
    : m_readWords(std::move(seams.readWords)), m_writeWords(std::move(seams.writeWords)) {
}

void EnvironmentInterpolation::onMidPaintBegin(uint64_t tick) {
    std::scoped_lock lock(m_mutex);
    m_tick = tick;
    uint32_t wavePacket = 0;
    uint32_t cloudPacket = 0;
    uint32_t snowPacket = 0;
    uint32_t countWord = 0;
    uint32_t radiusWord = 0;
    if (!m_readWords(kEnvLight + kWavePacket, &wavePacket, 1) ||
        !m_readWords(kEnvLight + kCloudPacket, &cloudPacket, 1) ||
        !m_readWords(kEnvLight + kSnowPacket, &snowPacket, 1) ||
        !m_readWords(kEnvLight + kWaveCount, &countWord, 1) ||
        !m_readWords(kEnvLight + kWaveSpawnRadius, &radiusWord, 1)) {
        m_unreadable++;
        return;
    }
    int16_t count = highHalf(countWord);
    if (wavePacket != 0 && count > 0) {
        blendWaves({.packet = wavePacket,
                    .count = std::min<uint32_t>(static_cast<uint32_t>(count), kMaxWaves),
                    .spawnRadius = asFloat(radiusWord)});
    }
    if (cloudPacket != 0) {
        blendClouds(cloudPacket);
    }
    if (snowPacket != 0) {
        blendSnow(snowPacket);
    }
}

void EnvironmentInterpolation::blendWaves(Waves waves) {
    uint32_t packet = waves.packet;
    uint32_t count = waves.count;
    std::vector<uint32_t> words(size_t{count} * kWaveWords);
    if (!m_readWords(packet + kWaves, words.data(), static_cast<uint32_t>(words.size()))) {
        m_unreadable++;
        return;
    }
    Seen previous = std::exchange(m_waves, {.tick = m_tick, .packet = packet, .words = words});
    if (previous.tick + 1 != m_tick || previous.packet != packet) {
        return;
    }
    std::vector<uint32_t> blended = words;
    for (size_t wave = 0; wave < count && (wave + 1) * kWaveWords <= previous.words.size();
         ++wave) {
        std::span<const uint32_t> from =
            std::span(previous.words).subspan(wave * kWaveWords, kWaveWords);
        std::span<const uint32_t> to = std::span(words).subspan(wave * kWaveWords, kWaveWords);
        std::span<uint32_t> out = std::span(blended).subspan(wave * kWaveWords, kWaveWords);
        // Respawned at a new base, or wrapped to the other side of the spawn circle.
        bool sameBase = std::equal(from.begin() + kWaveBase, from.begin() + kWaveBase + 3,
                                   to.begin() + kWaveBase);
        if (statusOf(to[kWaveStatus]) == 0 || statusOf(from[kWaveStatus]) == 0 || !sameBase ||
            !(distance(from.subspan(kWavePos), to.subspan(kWavePos), 3) < waves.spawnRadius)) {
            m_wavesRespawned++;
            continue;
        }
        if (!blendFloats(from.subspan(kWavePos, 3), to.subspan(kWavePos, 3),
                         out.subspan(kWavePos, 3)) ||
            !blendFloats(from.subspan(kWaveCounter, 2), to.subspan(kWaveCounter, 2),
                         out.subspan(kWaveCounter, 2))) {
            std::copy(to.begin(), to.end(), out.begin());
            m_unblendable++;
            continue;
        }
        m_waveBlends++;
    }
    write(packet + kWaves, blended, words);
}

void EnvironmentInterpolation::blendClouds(uint32_t packet) {
    std::vector<uint32_t> words(size_t{kCloudCount} * kCloudWords);
    if (!m_readWords(packet + kClouds, words.data(), static_cast<uint32_t>(words.size()))) {
        m_unreadable++;
        return;
    }
    Seen previous = std::exchange(m_clouds, {.tick = m_tick, .packet = packet, .words = words});
    if (previous.tick + 1 != m_tick || previous.packet != packet) {
        return;
    }
    std::vector<uint32_t> blended = words;
    for (size_t cloud = 0; cloud < kCloudCount; ++cloud) {
        std::span<const uint32_t> from =
            std::span(previous.words).subspan(cloud * kCloudWords, kCloudWords);
        std::span<const uint32_t> to = std::span(words).subspan(cloud * kCloudWords, kCloudWords);
        std::span<uint32_t> out = std::span(blended).subspan(cloud * kCloudWords, kCloudWords);
        if (statusOf(from[kCloudStatus]) != statusOf(to[kCloudStatus]) ||
            !(distance(from.subspan(kCloudPosition), to.subspan(kCloudPosition), 3) <
              kCloudWrapRadius)) {
            m_cloudsWrapped++;
            continue;
        }
        if (!blendFloats(from.subspan(kCloudPosition, 3), to.subspan(kCloudPosition, 3),
                         out.subspan(kCloudPosition, 3)) ||
            !blendFloats(from.subspan(kCloudAlpha, 1), to.subspan(kCloudAlpha, 1),
                         out.subspan(kCloudAlpha, 1))) {
            std::copy(to.begin(), to.end(), out.begin());
            m_unblendable++;
            continue;
        }
        m_cloudBlends++;
    }
    write(packet + kClouds, blended, words);
}

void EnvironmentInterpolation::blendSnow(uint32_t packet) {
    uint32_t countWord = 0;
    if (!m_readWords(packet + kSnowCount, &countWord, 1)) {
        m_unreadable++;
        return;
    }
    int16_t count = highHalf(countWord);
    if (count <= 0) {
        return;
    }
    uint32_t flakes = std::min<uint32_t>(static_cast<uint32_t>(count), kMaxSnow);
    std::vector<uint32_t> words(size_t{flakes} * kSnowWords);
    if (!m_readWords(packet + kSnow, words.data(), static_cast<uint32_t>(words.size()))) {
        m_unreadable++;
        return;
    }
    Seen previous = std::exchange(m_snow, {.tick = m_tick, .packet = packet, .words = words});
    if (previous.tick + 1 != m_tick || previous.packet != packet) {
        return;
    }
    std::vector<uint32_t> blended = words;
    for (size_t flake = 0; flake < flakes && (flake + 1) * kSnowWords <= previous.words.size();
         ++flake) {
        std::span<const uint32_t> from =
            std::span(previous.words).subspan(flake * kSnowWords, kSnowWords);
        std::span<const uint32_t> to = std::span(words).subspan(flake * kSnowWords, kSnowWords);
        std::span<uint32_t> out = std::span(blended).subspan(flake * kSnowWords, kSnowWords);
        // Spawned, landed or put back near the camera: the move restarts its timer.
        bool sameBase = std::equal(from.begin() + kSnowBase, from.begin() + kSnowBase + 3,
                                   to.begin() + kSnowBase);
        if (statusOf(from[kSnowStatus]) == 0 ||
            statusOf(from[kSnowStatus]) != statusOf(to[kSnowStatus]) || !sameBase ||
            highHalf(to[kSnowTimer]) > highHalf(from[kSnowTimer])) {
            m_snowRespawned++;
            continue;
        }
        if (!blendFloats(from.subspan(kSnowPos, 3), to.subspan(kSnowPos, 3),
                         out.subspan(kSnowPos, 3)) ||
            !blendFloats(from.subspan(kSnowScale, 1), to.subspan(kSnowScale, 1),
                         out.subspan(kSnowScale, 1))) {
            std::copy(to.begin(), to.end(), out.begin());
            m_unblendable++;
            continue;
        }
        m_snowBlends++;
    }
    write(packet + kSnow, blended, words);
}

void EnvironmentInterpolation::write(uint32_t address, const std::vector<uint32_t>& blended,
                                     const std::vector<uint32_t>& original) {
    if (blended == original) {
        return;
    }
    if (!m_writeWords(address, blended.data(), static_cast<uint32_t>(blended.size()))) {
        m_writeFailures++;
        return;
    }
    m_restores.push_back({.address = address, .words = original});
}

void EnvironmentInterpolation::onMidPaintEnd() {
    std::scoped_lock lock(m_mutex);
    for (const Restore& restore : m_restores) {
        if (m_writeWords(restore.address, restore.words.data(),
                         static_cast<uint32_t>(restore.words.size()))) {
            m_restored++;
        } else {
            m_writeFailures++;
        }
    }
    m_restores.clear();
}

std::string EnvironmentInterpolation::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.number("waveBlends", m_waveBlends);
    body.number("wavesRespawned", m_wavesRespawned);
    body.number("cloudBlends", m_cloudBlends);
    body.number("cloudsWrapped", m_cloudsWrapped);
    body.number("snowBlends", m_snowBlends);
    body.number("snowRespawned", m_snowRespawned);
    body.number("unblendable", m_unblendable);
    body.number("unreadable", m_unreadable);
    body.number("writeFailures", m_writeFailures);
    body.number("restored", m_restored);
    return body.finish();
}

} // namespace wiiuport::title
