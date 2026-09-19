#pragma once

#include "input/VPADInputHooks.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wiiuport::input {

// One button the channel can name, and the bit it holds.
struct NamedButton {
    const char* name;
    uint32_t mask;
};

// A press that lasts a bounded number of gamepad reads.
//
// Duration is counted in reads rather than seconds because a press has to
// survive being seen: a title that samples the pad once a frame can miss a
// press measured in milliseconds on a run that is not paced to real time,
// and a press that is missed looks exactly like a press that was never sent.
struct HeldPress {
    uint32_t mask{0};
    uint32_t readsRemaining{0};
};

// Supplies gamepad input to the running title on behalf of a tool.
//
// It is the reason a maintainer run can reach gameplay at all. Off until
// something asks: with no press queued and no stick offset it reports that
// this player is not driven, so the physical controllers keep working and a
// build that never uses it behaves exactly as upstream.
class InputDriver final : public VPADInputHooks::Source {
  public:
    // Every button the control channel will accept by name, and the only
    // place those names are defined.
    static const std::vector<NamedButton>& buttons();

    // Null when the name is not one of them, so a typo refuses rather than
    // silently pressing nothing.
    static const NamedButton* buttonNamed(const std::string& name);

    bool Poll(std::size_t playerIndex, VPADInputHooks::Injection& injection) override;

    // Hold `mask` for the next `reads` gamepad reads. Presses accumulate, so
    // two buttons can be held at once.
    void press(uint32_t mask, uint32_t reads);

    // Held until changed. Zero on both axes means the stick is centred, which
    // is not the same as this player not being driven.
    void setLeftStick(float x, float y);
    void setRightStick(float x, float y);

    // Stop driving entirely and hand the player back to real controllers.
    void release();

    // Denominators. A run where `pollsAnswered` stays zero never reached a
    // gamepad read, which is a different failure from a press that was sent
    // and ignored.
    uint64_t pollsSeen() const;
    uint64_t pollsAnswered() const;
    uint64_t pressesQueued() const;
    uint32_t currentMask() const;

  private:
    mutable std::mutex m_mutex;
    std::vector<HeldPress> m_pressed;
    float m_leftStickX{0.0f};
    float m_leftStickY{0.0f};
    float m_rightStickX{0.0f};
    float m_rightStickY{0.0f};
    bool m_driving{false};
    uint64_t m_pollsSeen{0};
    uint64_t m_pollsAnswered{0};
    uint64_t m_pressesQueued{0};
    uint32_t m_lastMask{0};
};

} // namespace wiiuport::input
