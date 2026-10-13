#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace wiiuport::title {

// Turns the product's sixty: once the display has painted, the two-paint stand-in, then the gate
// that holds the logic at thirty, from a thread of its own as the control channel does.
//
// Neither half runs without the other: the stand-in alone doubles the logic, and the gate alone
// halves it. A gate that refuses takes the stand-in back out.
class SixtyHertz {
  public:
    // Each returns empty on success, otherwise the refusal.
    struct Seams {
        std::function<std::string()> enablePaint;
        std::function<std::string()> disablePaint;
        std::function<std::string()> enableGate;
    };

    explicit SixtyHertz(Seams seams);
    ~SixtyHertz();
    SixtyHertz(const SixtyHertz&) = delete;
    SixtyHertz& operator=(const SixtyHertz&) = delete;
    SixtyHertz(SixtyHertz&&) = delete;
    SixtyHertz& operator=(SixtyHertz&&) = delete;

    // Starts the thread that waits for the first paint.
    void start();

    // On the display thread, at each paint.
    void onPaint();

    // Both halves or neither; empty on success, otherwise the refusal.
    std::string switchOn();

  private:
    void run();

    Seams m_seams;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_painted = false;
    bool m_stopping = false;
    std::thread m_thread;
};

} // namespace wiiuport::title
