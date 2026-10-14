#include "check.h"
#include "suites.h"
#include "wiiuport/title/CameraCuts.h"

#include <map>

using wiiuport::title::CameraCuts;

namespace {

constexpr uint32_t kCamera = 0x47c74d58;
constexpr uint32_t kDemo = 0x45000000;
constexpr uint32_t kDemoCamera = 0x4566fe50;
constexpr uint32_t kCameraAdaptor = 0x46000000;
constexpr uint32_t kActorAdaptor = 0x46100000;

void noRegistration(uint32_t /*entry*/, uint32_t /*firstInstruction*/,
                    GuestCallProbes::Probe& /*probe*/, bool /*holdsEntry*/, uint32_t /*resume*/) {
}

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    CameraCuts cuts() {
        return CameraCuts{{.registerProbe = &noRegistration,
                           .readWords = [this](uint32_t address, uint32_t* out, uint32_t count) {
                               for (uint32_t i = 0; i < count; ++i) {
                                   out[i] = words[address + (4 * i)];
                               }
                               return true;
                           }}};
    }

    void demo(bool running) {
        words[CameraCuts::kDemoObject] = kDemo;
        words[kDemo + CameraCuts::kActiveCamera] = running ? kDemoCamera : 0;
        words[kCameraAdaptor + CameraCuts::kAdaptorStageObject] = kDemoCamera;
        words[kActorAdaptor + CameraCuts::kAdaptorStageObject] = 0x4567a000;
    }

    void event(bool playing, uint32_t cutTicks) {
        words[CameraCuts::kEventCameraPlay] = playing ? 1 : 0;
        words[kCamera + CameraCuts::kCameraBody + CameraCuts::kCutTicks] = cutTicks;
    }
};

void aCutsceneParagraphStartsOneShot() {
    Guest guest;
    guest.demo(true);
    guest.event(true, 300);
    CameraCuts cuts = guest.cuts();
    check::isTrue(!cuts.startsShot(kCamera),
                  "a cutscene tick with no paragraph continues its shot");
    cuts.onSetVariableValue(kActorAdaptor);
    check::isTrue(!cuts.startsShot(kCamera), "nor does an actor's paragraph start one");
    cuts.onSetVariableValue(kCameraAdaptor);
    check::isTrue(cuts.startsShot(kCamera), "the demo camera's paragraph starts a shot");
    check::isTrue(!cuts.startsShot(kCamera), "once: the camera draw that sees it consumes it");
}

void aParagraphWithNoCutsceneIsNotAShot() {
    Guest guest;
    guest.demo(false);
    guest.event(false, 0);
    CameraCuts cuts = guest.cuts();
    cuts.onSetVariableValue(kCameraAdaptor);
    check::isTrue(!cuts.startsShot(kCamera), "with no active demo camera nothing starts a shot");
}

void anEventCameraCutStartsAShot() {
    Guest guest;
    guest.demo(false);
    CameraCuts cuts = guest.cuts();
    guest.event(true, 1);
    check::isTrue(cuts.startsShot(kCamera), "the event camera's first tick of a cut starts a shot");
    guest.event(true, 2);
    check::isTrue(!cuts.startsShot(kCamera), "its second does not");
    guest.event(false, 1);
    check::isTrue(!cuts.startsShot(kCamera), "nor does the counter when no event camera plays");
}

} // namespace

void wiiuport::tests::runCameraCutsTests() {
    aCutsceneParagraphStartsOneShot();
    aParagraphWithNoCutsceneIsNotAShot();
    anEventCameraCutStartsAShot();
}
