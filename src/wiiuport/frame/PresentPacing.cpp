#include "wiiuport/frame/PresentPacing.h"

#include <algorithm>

namespace wiiuport::frame {

namespace {

using std::chrono::microseconds;

// The value below which `fraction` of `lengths` fall. Reorders them.
microseconds percentile(std::vector<microseconds>& lengths, double fraction) {
    if (lengths.empty()) {
        return microseconds{0};
    }
    auto rank = static_cast<size_t>(fraction * static_cast<double>(lengths.size() - 1));
    std::nth_element(lengths.begin(), lengths.begin() + static_cast<std::ptrdiff_t>(rank),
                     lengths.end());
    return lengths[rank];
}

} // namespace

PresentPacing::PresentPacing(Now now) : m_now(now) {
}

void PresentPacing::onDisplayed(bool fromRuntime) {
    record(fromRuntime, m_now().time_since_epoch(), 0);
}

void PresentPacing::onScannedOut(const LatteFrameHooks::ShownFrame& shown) {
    {
        std::lock_guard lock(m_mutex);
        m_stage = shown.stage;
    }
    record(shown.fromRuntime, std::chrono::nanoseconds{shown.timeNanoseconds}, shown.timeDomainId);
}

void PresentPacing::record(bool fromRuntime, std::chrono::nanoseconds at, uint64_t clock) {
    std::lock_guard lock(m_mutex);
    ++(fromRuntime ? m_runtimeFrames : m_guestFrames);
    if (m_seenOne && clock == m_lastClock) {
        ++m_intervals;
        if (m_kept.size() < kMaxIntervals) {
            m_kept.push_back({std::chrono::duration_cast<microseconds>(at - m_last),
                              m_lastFromRuntime, fromRuntime});
        }
    }
    m_seenOne = true;
    m_last = at;
    m_lastClock = clock;
    m_lastFromRuntime = fromRuntime;
}

void PresentPacing::restart() {
    std::lock_guard lock(m_mutex);
    m_seenOne = false;
    m_guestFrames = 0;
    m_runtimeFrames = 0;
    m_intervals = 0;
    m_kept.clear();
}

PresentPacing::Summary PresentPacing::summary() const {
    std::vector<Interval> kept;
    Summary summary;
    {
        std::lock_guard lock(m_mutex);
        kept = m_kept;
        summary.guestFrames = m_guestFrames;
        summary.runtimeFrames = m_runtimeFrames;
        summary.intervals = m_intervals;
        summary.stage = m_stage;
    }
    summary.intervalsKept = kept.size();
    std::vector<microseconds> all;
    std::vector<microseconds> guestToRuntime;
    std::vector<microseconds> runtimeToGuest;
    for (const Interval& interval : kept) {
        all.push_back(interval.length);
        if (!interval.fromRuntime && interval.toRuntime) {
            guestToRuntime.push_back(interval.length);
        }
        if (interval.fromRuntime && !interval.toRuntime) {
            runtimeToGuest.push_back(interval.length);
        }
    }
    summary.p50 = percentile(all, 0.50);
    summary.p95 = percentile(all, 0.95);
    summary.p99 = percentile(all, 0.99);
    summary.longest = percentile(all, 1.0);
    summary.guestToRuntimeMedian = percentile(guestToRuntime, 0.5);
    summary.runtimeToGuestMedian = percentile(runtimeToGuest, 0.5);
    return summary;
}

} // namespace wiiuport::frame
