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
        // Computed values are ordinary locals, const or not. The rule flagged every block-scope
        // const and so found 376 of them across the tree, all of them correct.
        const int doubled = value * 2;
        const int* followed = still_fine + 1;
        running += (still_fine == nullptr) ? 0 : 1;
        running += doubled + (followed == nullptr ? 0 : 1);
        capacity_ = running;
    }

private:
    int capacity_;
};

int slots(const Recorder& recorder) { return recorder.capacity() + kSlotCount; }

}  // namespace wiiuport::fixture

int main() { return 0; }
