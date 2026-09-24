#include "wiiuport/interp/ShadowCheck.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace wiiuport::interp {
namespace {

// The in-between-only pages are the finding and are listed in full up to
// this many; the others are background and are counted.
inline constexpr size_t kPagesListed = 256;

std::string pageList(const ShadowCheck::Result& result) {
    std::string out = "[";
    size_t listed = std::min(result.inBetweenOnly.size(), kPagesListed);
    for (size_t index = 0; index < listed; ++index) {
        out += std::format("{}{{\"page\":\"{:08x}\",\"windows\":{}}}", index == 0 ? "" : ",",
                           result.inBetweenOnly[index], result.inBetweenOnlyWindows[index]);
    }
    return out + "]";
}

} // namespace

ShadowCheck::ShadowCheck(Regions regions, DrawInBetween draw, Now now, Wait wait)
    : m_regions(std::move(regions)), m_draw(std::move(draw)), m_now(std::move(now)),
      m_wait(std::move(wait)) {
}

ShadowCheck::Result ShadowCheck::run(const frame::FrameRecording& held, uint32_t rounds) {
    Result result;
    std::set<uint32_t> control;
    std::map<uint32_t, uint32_t> inBetween;
    try {
        for (uint32_t round = 0; round < rounds; ++round) {
            std::vector<frame::GuestMemorySnapshot::Region> regions = m_regions();
            m_snapshot.take(regions);
            result.bytesCompared += m_snapshot.bytesHeld();
            Clock::time_point started = m_now();
            if (m_draw(held)) {
                ++result.inBetweensDrawn;
            }
            Clock::duration took = m_now() - started;
            std::vector<uint32_t> drawn = m_snapshot.changedPages(regions);
            for (uint32_t page : drawn) {
                ++inBetween[page];
            }

            m_snapshot.take(regions);
            m_wait(took);
            std::vector<uint32_t> idle = m_snapshot.changedPages(regions);
            control.insert(idle.begin(), idle.end());
            ++result.rounds;
        }
    } catch (const std::logic_error& error) {
        // Only the snapshot throws, and only when the memory it compares is
        // not the memory it copied; nothing the check drew depends on it.
        result.refusal = error.what();
    }
    m_snapshot.release();
    result.controlPages.assign(control.begin(), control.end());
    for (const auto& [page, windows] : inBetween) {
        result.inBetweenPages.push_back(page);
        if (!control.contains(page)) {
            result.inBetweenOnly.push_back(page);
            result.inBetweenOnlyWindows.push_back(windows);
        }
    }
    return result;
}

std::string ShadowCheck::toJson(const Result& result) {
    return std::format(
        "{{\"clean\":{},\"rounds\":{},\"inBetweensDrawn\":{},\"bytesCompared\":{},"
        "\"controlPages\":{},\"inBetweenPages\":{},\"charged\":{},\"inBetweenOnlyCount\":{},"
        "\"inBetweenOnly\":{},\"refusal\":\"{}\"}}",
        result.clean() ? "true" : "false", result.rounds, result.inBetweensDrawn,
        result.bytesCompared, result.controlPages.size(), result.inBetweenPages.size(),
        result.charged(), result.inBetweenOnly.size(), pageList(result), result.refusal);
}

} // namespace wiiuport::interp
