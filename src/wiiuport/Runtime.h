#pragma once

#include <mutex>

#include "Cafe/HW/Espresso/GuestPatching.h"
#include "wiiuport/control/ControlChannel.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FrameGate.h"
#include "wiiuport/frame/FrameShapeLog.h"
#include "wiiuport/frame/PresentPacing.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/RecordingSnapshot.h"
#include "wiiuport/guest/BufferWriters.h"
#include "wiiuport/guest/CallerCensus.h"
#include "wiiuport/guest/EnvironmentProbe.h"
#include "wiiuport/guest/LineProbe.h"
#include "wiiuport/guest/ParticleProbe.h"
#include "wiiuport/input/InputDriver.h"
#include "wiiuport/title/BufferedBlocks.h"
#include "wiiuport/title/CommandStreamIdentity.h"
#include "wiiuport/title/DrawAttributeCensus.h"
#include "wiiuport/title/DrawInterpolation.h"
#include "wiiuport/title/GlobalPoseCensus.h"
#include "wiiuport/title/LogicGate.h"
#include "wiiuport/title/NodePoseLocator.h"
#include "wiiuport/title/ObjectPoseLocator.h"
#include "wiiuport/title/PoseBlend.h"
#include "wiiuport/title/PoseByShader.h"
#include "wiiuport/title/QuadBlend.h"
#include "wiiuport/title/UniformBlockCensus.h"
#include "wiiuport/title/WindWakerPaint.h"

namespace wiiuport {

// The one long-lived owner of everything first-party in a running process.
//
// The fork's hook registry holds a raw pointer, so whatever it points at has
// to outlive every frame. Keeping that here, in one object with one lifetime,
// is the alternative to each subsystem arranging its own global.
class Runtime {
  public:
    // Constructible directly so a test can drive one without touching the
    // process-wide instance. Not copyable: the hook registry holds a pointer
    // to whichever one was installed.
    Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // The process-wide instance. Never destroyed, and leaked on purpose: a
    // static destroyed at exit leaves the hook registry pointing at a dead
    // object and the control channel's server torn down underneath its own
    // thread, which ends a failed run in std::terminate rather than with the
    // exit code that says what went wrong.
    static Runtime& instance();

    // Idempotent: installing twice is a programming error at the call site,
    // not a reason to replace a registration while a frame is in flight.
    void installHooks();

    bool hooksInstalled() const {
        return m_hooksInstalled;
    }

    frame::RecordingObserver& recorder() {
        return m_recorder;
    }

    control::ControlChannel& control() {
        return m_control;
    }

    // The display paint mod. It is what presents more than once per title frame now, which is why
    // it replaced the continuous interpolator in the shell's presentation-mode decision.
    title::WindWakerPaint& paint() {
        return m_paint;
    }

    input::InputDriver& input() {
        return m_input;
    }

    frame::FrameCapture& capture() {
        return m_capture;
    }

    const frame::FrameShapeLog& shapeLog() const {
        return m_shapeLog;
    }

    frame::FrameGate& frameGate() {
        return m_gate;
    }

  private:
    // GX2's write position on the calling core, for the binder probe.
    static title::CommandPosition gx2WritePosition();

    inline static Runtime* s_instance{nullptr};
    inline static std::once_flag s_created;

    frame::RecordingObserver m_recorder;
    frame::FrameCapture m_capture;
    frame::FrameShapeLog m_shapeLog;
    // Where in an assembled uniform buffer the pose is, found by shape. Registered
    // before the blends so its counts are the same draws they see.
    title::ObjectPoseLocator m_poseLocator;
    // Where a draw's pose is, in words, per shader -- fed from the locator's own candidates by
    // `POST /pose` and read by a blend. It is here because the measurement that fills it is the
    // locator's, and a table fed by anything else is a table of guesses.
    title::PoseByShader m_poseByShader;
    // The in-between frame, written at the title's own draw from the table above. It is armed here
    // and only armed here: a blend that is not fed by `POST /pose` finds no offset for any shader
    // and leaves every draw exactly as the title wrote it, so an unarmed blend is a no-op rather
    // than a blend at a guess.
    title::PoseBlend m_poseBlend{m_poseByShader};
    // Which field of a node holds its pose. Registered first of the three, because it is
    // the one two measurements point at.
    title::NodePoseLocator m_nodePose{&GuestCallProbes::Register, &GuestPatching::ReadWords};
    // Which node each draw belongs to, joined by the binder's position in the command stream.
    title::CommandStreamIdentity m_objectIdentity{&Runtime::gx2WritePosition};
    // Which of a draw's attributes is the position, measured from the title's own attribute
    // tables. The pose is not a transform anywhere, so the blend writes vertex bytes at the
    // game's own draw, and this is what tells it where they are.
    title::DrawAttributeCensus m_drawAttributes{&m_objectIdentity};
    // Whether two ticks' position bytes exist to be blended, per node. The falsifier for the
    // vertex-stream blend, and it is a separate class because a history that also did the
    // blending could not be asked whether blending was possible.
    title::VertexPoseHistory m_vertexHistory{&m_objectIdentity, &m_drawAttributes, nullptr};
    // Whether tick N-1's uniform block contents are still there when tick N paints. The
    // objective's own second question, and the census is the one place that knows which block a
    // binding names.
    title::UniformBlockRing m_blockRing{&GuestPatching::ReadWords, nullptr};
    // The base the relative offset is relative to, measured against the draw's real block
    // addresses. The last thing standing between the objective's second question and an answer.
    guest::BufferWriters m_writers;
    guest::ParticleProbe m_particleProbe{m_writers};
    guest::EnvironmentProbe m_environmentProbe{m_writers};
    guest::LineProbe m_lineProbe{m_writers};
    guest::CallerCensus m_callers{&GuestCallProbes::Register, &GuestCallProbes::GuestBytes};
    title::BufferedBlocks m_bufferedBlocks{&GuestCallProbes::Register,
                                           &GuestCallProbes::GuestBytes};
    title::LogicGate m_logic{{.registerProbe = &GuestCallProbes::Register,
                              .allocateCode = &GuestPatching::AllocateCode,
                              .allocateData = &GuestPatching::AllocateData,
                              .writeWord = &GuestPatching::WriteWord,
                              .readWord = &GuestPatching::ReadWord}};
    title::UniformBlockCensus m_blocks{&GuestCallProbes::Register, &GuestPatching::ReadWord,
                                       &GuestPatching::ReadWords, &m_poseLocator, &m_nodePose};
    // The data-area scan: two snapshots of the title's `.data` and `.bss` a frame apart, looking
    // for a transform that changed. It is the one instrument that can find a *global* uniform, and
    // a camera is a global uniform -- which is why the per-draw assembly scan could not.
    title::GlobalPoseCensus m_globalPose{&GuestPatching::ReadWords, [this]() {
                                             // The paint counter, because a frame here means a
                                             // presented frame -- the same counter the paint mod
                                             // counts with, so "one frame apart" means one present
                                             // apart and not one interpreter iteration apart.
                                             return m_paint.paintCounter().load();
                                         }};
    title::WindWakerPaint m_paint{&GuestCallProbes::Register,      &GuestPatching::AllocateCode,
                                  &GuestPatching::WriteWord,       &GuestPatching::ReadWord,
                                  &GuestPatching::SetSwapInterval, &GuestPatching::SwapInterval};
    title::DrawInterpolation m_drawInterpolation{{.registerProbe = &GuestCallProbes::Register,
                                                  .readWords = &GuestPatching::ReadWords,
                                                  .writeWords = &GuestPatching::WriteDataWords,
                                                  .gated = [this] {
                                                      return m_logic.enabled();
                                                  }}};
    // The CPU-written quads, blended on the first paint of each gated tick.
    title::QuadBlend m_quadBlend{m_writers, [this] {
                                     return m_logic.enabled();
                                 }};
    frame::RecordingSnapshot m_snapshot;
    frame::PresentPacing m_pacing{&frame::PresentPacing::Clock::now};
    frame::PresentPacing m_scanOut{&frame::PresentPacing::Clock::now};
    input::InputDriver m_input;
    frame::FrameGate m_gate;
    control::ControlChannel m_control{control::ControlChannel::Sources{
        .recorder = m_recorder,
        .input = m_input,
        .capture = m_capture,
        .shapeLog = m_shapeLog,
        .callers = m_callers,
        .paint = m_paint,
        .blocks = m_blocks,
        .poses = m_poseLocator,
        .globalPose = m_globalPose,
        .poseByShader = m_poseByShader,
        .poseBlend = m_poseBlend,
        .quadBlend = m_quadBlend,
        .bufferedBlocks = m_bufferedBlocks,
        .drawInterpolation = m_drawInterpolation,
        .logic = m_logic,
        .guestBytes = &GuestCallProbes::GuestBytes,
        .snapshot = m_snapshot,
        .pacing = m_pacing,
        .scanOut = m_scanOut,
        .vertexChanges = m_recorder.vertexChanges(),
        .gate = m_gate,
    }};
    bool m_hooksInstalled{false};
};

} // namespace wiiuport

// The fork's startup calls this, and names nothing first-party in doing so.
//
// Registration cannot be left to a static initialiser: this is a static
// library, and a linker drops an object file nothing references, taking the
// initialiser with it. The failure would be silent -- a build that records
// nothing and reports no error.
extern "C" void wiiuport_install_hooks(void);
