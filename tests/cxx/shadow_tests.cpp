#include "check.h"
#include "suites.h"
#include "wiiuport/frame/GuestMemorySnapshot.h"
#include "wiiuport/interp/ShadowCheck.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using wiiuport::frame::GuestMemorySnapshot;
using wiiuport::frame::kGuestPageSize;
using wiiuport::interp::ShadowCheck;

namespace {

// Two mapped spans with a gap between them, as the guest's are.
struct FakeGuest {
    std::vector<uint8_t> low = std::vector<uint8_t>(size_t{3} * kGuestPageSize, 0x11);
    std::vector<uint8_t> high = std::vector<uint8_t>(size_t{2} * kGuestPageSize + 100, 0x22);

    static constexpr uint32_t kLow = 0x10000000;
    static constexpr uint32_t kHigh = 0xF4000000;

    std::vector<GuestMemorySnapshot::Region> regions() const {
        return {{kLow, low.data(), static_cast<uint32_t>(low.size())},
                {kHigh, high.data(), static_cast<uint32_t>(high.size())}};
    }
};

void aSnapshotNamesExactlyThePagesWhoseBytesChanged() {
    FakeGuest guest;
    GuestMemorySnapshot snapshot;
    snapshot.take(guest.regions());
    check::equal(snapshot.changedPages(guest.regions()).size(), size_t{0},
                 "nothing written, nothing changed");

    guest.low[kGuestPageSize + 7] ^= 1;
    // The last byte of the short final page.
    guest.high.back() ^= 1;
    std::vector<uint32_t> changed = snapshot.changedPages(guest.regions());
    check::equal(changed.size(), size_t{2}, "one bit in each of two pages");
    check::equal(changed[0], FakeGuest::kLow + kGuestPageSize, "the low span's second page");
    check::equal(changed[1], FakeGuest::kHigh + 2 * kGuestPageSize, "the high span's short page");
}

void memoryMappedSinceTheSnapshotIsRefusedNotCompared() {
    FakeGuest guest;
    GuestMemorySnapshot snapshot;
    snapshot.take(guest.regions());
    std::vector<GuestMemorySnapshot::Region> fewer = guest.regions();
    fewer.pop_back();
    bool refused = false;
    try {
        snapshot.changedPages(fewer);
    } catch (const std::logic_error&) {
        refused = true;
    }
    check::isTrue(refused, "an unmapped span is refused");
}

ShadowCheck checkOn(FakeGuest& guest, ShadowCheck::DrawInBetween draw, ShadowCheck::Wait wait) {
    return ShadowCheck(
        [&guest] {
            return guest.regions();
        },
        std::move(draw),
        [] {
            return ShadowCheck::Clock::time_point{};
        },
        std::move(wait));
}

void anInBetweenFrameThatLeavesGuestMemoryAloneIsClean() {
    FakeGuest guest;
    uint32_t tick = 0;
    ShadowCheck check = checkOn(
        guest,
        [](const wiiuport::frame::FrameRecording&) {
            return true;
        },
        // The guest's audio thread, writing one page in every window.
        [&guest, &tick](ShadowCheck::Clock::duration) {
            guest.high[0] = static_cast<uint8_t>(++tick);
        });
    wiiuport::frame::FrameRecording held;
    ShadowCheck::Result result = check.run(held, 3);
    check::isTrue(result.clean(), "clean");
    check::equal(result.inBetweensDrawn, uint32_t{3}, "every round drew");
    check::equal(result.controlPages.size(), size_t{1}, "the background page is seen");
    check::equal(result.bytesCompared, uint64_t{3} * (guest.low.size() + guest.high.size()),
                 "every byte of every round compared");
}

void anInBetweenFrameThatWritesGuestMemoryIsCaught() {
    FakeGuest guest;
    ShadowCheck check = checkOn(
        guest,
        [&guest](const wiiuport::frame::FrameRecording&) {
            guest.low[size_t{2} * kGuestPageSize] ^= 0xFF;
            return true;
        },
        [](ShadowCheck::Clock::duration) {
        });
    wiiuport::frame::FrameRecording held;
    ShadowCheck::Result result = check.run(held, 2);
    check::isTrue(!result.clean(), "not clean");
    check::equal(result.charged(), size_t{1}, "the page is charged");
    check::equal(result.inBetweenOnly.size(), size_t{1}, "one page charged to the frame");
    check::equal(result.inBetweenOnly.front(), FakeGuest::kLow + 2 * kGuestPageSize,
                 "the page it wrote");
    std::string json = ShadowCheck::toJson(result);
    check::equal(result.inBetweenOnlyWindows.front(), uint32_t{2}, "in both rounds");
    check::isTrue(json.find("\"inBetweenOnly\":[{\"page\":\"10002000\",\"windows\":2}]") !=
                      std::string::npos,
                  "and the report names it");
}

void aPageChangedInOnlySomeInBetweenWindowsIsListedNotCharged() {
    FakeGuest guest;
    uint32_t round = 0;
    ShadowCheck check = checkOn(
        guest,
        // A background writer that happened to land in the first in-between
        // window and in no control window.
        [&guest, &round](const wiiuport::frame::FrameRecording&) {
            if (round++ == 0) {
                guest.low[0] ^= 0xFF;
            }
            return true;
        },
        [](ShadowCheck::Clock::duration) {
        });
    wiiuport::frame::FrameRecording held;
    ShadowCheck::Result result = check.run(held, 3);
    check::equal(result.inBetweenOnly.size(), size_t{1}, "listed");
    check::equal(result.charged(), size_t{0}, "but not charged to the frame");
    check::isTrue(result.clean(), "so the check is clean");
}

void aCheckWhoseFramesWereNotDrawnIsNotClean() {
    FakeGuest guest;
    ShadowCheck check = checkOn(
        guest,
        [](const wiiuport::frame::FrameRecording&) {
            return false;
        },
        [](ShadowCheck::Clock::duration) {
        });
    wiiuport::frame::FrameRecording held;
    ShadowCheck::Result result = check.run(held, 2);
    check::isTrue(result.inBetweenOnly.empty(), "nothing drawn, nothing written");
    check::isTrue(!result.clean(), "but a check that drew nothing proves nothing");
}

} // namespace

namespace wiiuport::tests {

void runShadowTests() {
    aSnapshotNamesExactlyThePagesWhoseBytesChanged();
    memoryMappedSinceTheSnapshotIsRefusedNotCompared();
    anInBetweenFrameThatLeavesGuestMemoryAloneIsClean();
    anInBetweenFrameThatWritesGuestMemoryIsCaught();
    aPageChangedInOnlySomeInBetweenWindowsIsListedNotCharged();
    aCheckWhoseFramesWereNotDrawnIsNotClean();
}

} // namespace wiiuport::tests
