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

    bool hasObservedPresent() const {
        return m_presentsObserved > 0;
    }

    const LatteFrameHooks::PresentArguments& lastPresent() const {
        return m_lastPresent;
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
    LatteFrameHooks::PresentArguments m_lastPresent{};
    bool m_armed{false};
    uint64_t m_presentsObserved{0};
    uint64_t m_presentsSubmitted{0};
    uint64_t m_presentsRefusedUnobserved{0};
    uint64_t m_presentsRefusedBySubmit{0};
};

} // namespace wiiuport::frame
