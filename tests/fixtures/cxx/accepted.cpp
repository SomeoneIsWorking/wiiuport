// Every declaration here is legal under the three ownership rules, including
// the const-correct forms the rules deliberately leave alone.
#include <cstdint>

extern "C" void wiiuport_guest_entry(std::uint32_t address);

namespace wiiuport::fixture {

inline constexpr int kSlotCount = 4;

class Recorder {
public:
    explicit Recorder(int capacity) : capacity_(capacity) {}

    [[nodiscard]] int capacity() const { return capacity_; }

    void note(const int& value, const int* pointee) {
        int running = value;
        const int* still_fine = pointee;
        running += (still_fine == nullptr) ? 0 : 1;
        capacity_ = running;
    }

private:
    int capacity_;
};

int slots(const Recorder& recorder) { return recorder.capacity() + kSlotCount; }

}  // namespace wiiuport::fixture

int main() { return 0; }
