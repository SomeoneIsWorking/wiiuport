#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include "wiiuport/frame/RecordingObserver.h"

#include <cstdint>

namespace wiiuport::frame {

// Decides when the runtime presents a frame of its own, and remembers what a
// present is made of.
//
// The encoding belongs to the fork, which already owns the packet format the
// guest writes; this owns only policy. That split is why the arguments have
// to be observed rather than invented: a present built from plausible values
// would address a scan buffer the title never wrote.
//
// A title copies to a scan buffer twice per frame -- once for the TV and once
// for the GamePad -- which is what the main window and every capture show in
// turn. Measured on Wind Waker HD: 922 presents across 460 frames. Keeping
// only the last one made the runtime re-present the GamePad's buffer, and a
// null-diff control that should have been byte-identical came back with a
// GamePad item icon drawn across it. The TV present is the one kept, because
// it is the one the user is looking at.
//
// Armed one present at a time, for the same reason a replay is: an extra
// present that fired continuously would make a changed image impossible to
// attribute to one submission.
class FramePresenter final : public PresentListener {
  public:
    // Returns false when nothing was submitted, which is counted rather than
    // treated as a present that happened.
    using Submit = bool (*)(const LatteFrameHooks::PresentArguments& present);

    explicit FramePresenter(Submit submit);

    // Called for every present the title makes, so the arguments stay current
    // with the scan buffer the title is actually filling.
    void onPresentObserved(const LatteFrameHooks::PresentArguments& present) override;

    // Whether the screen the user watches has been seen. A GamePad-only
    // observation is not one: presenting it would show the wrong screen.
    bool hasObservedPresent() const {
        return m_presentsObservedTv > 0;
    }

    const LatteFrameHooks::PresentArguments& lastPresent() const {
        return m_lastTvPresent;
    }

    void armOnce() {
        m_armed = true;
    }

    bool isArmed() const {
        return m_armed;
    }

    // Present now. Refuses -- and says so by returning false -- when no
    // present has been observed yet, rather than submitting a packet full of
    // zeroes that would read as a present of nothing.
    bool presentNow();

    // Does nothing unless armed, and disarms whether or not it succeeded.
    bool presentIfArmed();

    uint64_t presentsObserved() const {
        return m_presentsObserved;
    }

    // Split out, because "the title never presented" and "the title only
    // presented to the GamePad" are different reasons for the same silence.
    uint64_t presentsObservedTv() const {
        return m_presentsObservedTv;
    }

    uint64_t presentsObservedDrc() const {
        return m_presentsObservedDrc;
    }

    uint64_t presentsSubmitted() const {
        return m_presentsSubmitted;
    }

    uint64_t presentsRefusedUnobserved() const {
        return m_presentsRefusedUnobserved;
    }

    uint64_t presentsRefusedBySubmit() const {
        return m_presentsRefusedBySubmit;
    }

  private:
    Submit m_submit;
    LatteFrameHooks::PresentArguments m_lastTvPresent{};
    bool m_armed{false};
    uint64_t m_presentsObserved{0};
    uint64_t m_presentsObservedTv{0};
    uint64_t m_presentsObservedDrc{0};
    uint64_t m_presentsSubmitted{0};
    uint64_t m_presentsRefusedUnobserved{0};
    uint64_t m_presentsRefusedBySubmit{0};
};

} // namespace wiiuport::frame
