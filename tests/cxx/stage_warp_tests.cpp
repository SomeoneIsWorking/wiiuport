// The stage warp: the title's own next-stage request, written as dComIfGp_setNextStage writes it.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/StageWarp.h"

#include <cstdint>
#include <map>
#include <string>

namespace {

using wiiuport::title::StageWarp;

constexpr uint32_t kSaveInfo = 0x3e000000;
constexpr uint32_t kPlayer = 0x3f000000;

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    StageWarp warp() {
        words[StageWarp::kSaveInfo] = kSaveInfo;
        words[StageWarp::kPlayer] = kPlayer;
        return StageWarp{{.readWords =
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
};

void aQueryNamesAStageAPointAndARoom() {
    std::string refusal;
    auto request = StageWarp::parse("stage=sea&point=0&room=44", refusal);
    check::isTrue(request.has_value() && request->stage == "sea" && request->room == 44 &&
                      request->layer == -1,
                  "a stage, point and room, the layer the stage's own");
    check::isTrue(!StageWarp::parse("stage=toolongname&point=0&room=1", refusal).has_value(),
                  "a name longer than the request holds is refused");
    check::isTrue(!StageWarp::parse("stage=sea&room=1", refusal).has_value(),
                  "and so is a request with no point");
    check::isTrue(!StageWarp::parse("stage=sea&point=0&room=300", refusal).has_value(),
                  "or a room past s8");
}

void theRequestIsWrittenAsTheTitleWritesIt() {
    Guest guest;
    StageWarp warp = guest.warp();
    // Bytes after the request, and the restart word's low half, are not the request's.
    guest.words[StageWarp::kEnableWord] = 0x0000abcd;
    guest.words[kSaveInfo + StageWarp::kRestartPoint] = 0x00001234;
    guest.words[kPlayer + StageWarp::kPlayerFlags] = 0x8001;
    guest.words[kPlayer + 0x699c] = 0x00000002;
    std::string refusal;
    check::isTrue(warp.warp({.stage = "sea", .point = 3, .room = 44, .layer = -1}, refusal),
                  "a warp with nothing pending is made");
    check::isTrue(guest.words[StageWarp::kNextStage] == 0x73656100U &&
                      guest.words[StageWarp::kNextStage + 4] == 0,
                  "the stage named, zero-padded");
    check::isTrue(guest.words[StageWarp::kPlaceWord] == 0x00032cffU, "point, room and layer");
    check::isTrue(guest.words[StageWarp::kEnableWord] == 0x0100abcdU,
                  "enabled with no wipe, the bytes after it kept");
    check::isTrue(guest.words[kSaveInfo + StageWarp::kRestartPoint] == 0x00031234U,
                  "the restart point set to the point");
    check::isTrue(guest.words[kSaveInfo + StageWarp::kLastMode] == 0x0002c000U,
                  "and the mode carrying both shields and the tinkle state");
    check::isTrue(!warp.warp({.stage = "sea", .point = 0, .room = 1, .layer = -1}, refusal),
                  "a second warp while the first is pending is refused");
}

} // namespace

void wiiuport::tests::runStageWarpTests() {
    aQueryNamesAStageAPointAndARoom();
    theRequestIsWrittenAsTheTitleWritesIt();
}
