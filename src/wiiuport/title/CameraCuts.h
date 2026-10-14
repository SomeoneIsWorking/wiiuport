#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace wiiuport::title {

// Whether the camera's view this tick starts a new shot, from the title's own state: a blend across
// a cut draws a view neither shot has.
//
// A cutscene shot is a JStudio paragraph that sets the demo camera's variables; an event camera
// shot starts with the event camera's per-cut counter at one.
class CameraCuts {
  public:
    // JStudio::TAdaptor::adaptor_setVariableValue(adaptor, object, index, operation, data, size):
    // every paragraph's value passes through it.
    static constexpr uint32_t kSetVariableValue = 0x02839fac;
    static constexpr uint32_t kSetVariableValueFirst = 0x28060001; // cmplwi r6,1
    static constexpr size_t kAdaptorRegister = 3;
    // JStudio_JStage camera adaptor: its JStage camera (do_begin 0x02840d54 reads the view through
    // it).
    static constexpr uint32_t kAdaptorStageObject = 0xec;
    // dDemo's m_object, and its active camera (getActiveCamera 0x025283f8).
    static constexpr uint32_t kDemoObject = 0x101d5ffc;
    static constexpr uint32_t kActiveCamera = 0x84;
    // camera_class's dCamera_c, its per-cut counter (eventCamera 0x024ff164 zeroes it on a new cut,
    // NotRun 0x024ff6c0 counts it up after), and the event manager's cameraPlay.
    static constexpr uint32_t kCameraBody = 0x248;
    static constexpr uint32_t kCutTicks = 0x11c;
    static constexpr uint32_t kEventCameraPlay = 0x1046f0b0 + 0x52e4;

    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using ReadWords = std::function<bool(uint32_t address, uint32_t* words, uint32_t count)>;

    struct Seams {
        Register registerProbe;
        ReadWords readWords;
    };

    explicit CameraCuts(Seams seams);

    void install();

    // At the tick's camera draw: whether `camera` (camera_class) starts a shot this tick. A
    // cutscene paragraph is consumed by the camera draw that sees it.
    bool startsShot(uint32_t camera);

    // A paragraph set a variable on `adaptor`.
    void onSetVariableValue(uint32_t adaptor);

    std::string json() const;

  private:
    class Paragraphs final : public GuestCallProbes::Probe {
      public:
        explicit Paragraphs(CameraCuts& owner) : m_owner(owner) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::atomic<std::optional<GuestCallProbes::Installation>> installation;

      private:
        CameraCuts& m_owner;
    };

    std::optional<uint32_t> readWord(uint32_t address) const;
    std::optional<uint32_t> activeDemoCamera() const;

    Register m_register;
    ReadWords m_readWords;
    Paragraphs m_paragraphs{*this};
    std::atomic<bool> m_demoParagraph{false};
    std::atomic<uint64_t> m_demoParagraphs{0};
    std::atomic<uint64_t> m_demoShots{0};
    std::atomic<uint64_t> m_eventShots{0};
};

} // namespace wiiuport::title
