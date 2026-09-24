#pragma once

#include "wiiuport/interp/Blendable.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wiiuport::interp {

// Whether a candidate in N-1 is the one an object passed through between N-2
// and N: over the values it moved in, its midpoint lands on the candidate.
//
// Three frames of one object identify it where its keys cannot: a different
// object, or the same one standing at either end, lands at half its step; the
// object itself, passing through, near zero. Fed value by value, so uniforms
// and vertices -- read out of their own layouts -- are judged alike.
class Midpoint {
  public:
    // How far the midpoint may land from the candidate, against how far the
    // object moved over the two frames: half way between the object itself
    // (0) and anything else (0.5). Measured on the sea: the median lands at
    // 0.024 and 92% of moving objects within it.
    static constexpr float kTolerance = 0.25f;

    // Whether the object moved in a value between N-2 and N: a number at both
    // ends with other bits. Only such a value says where it passed through;
    // one the same at both ends is drawn at N whatever N-1 holds, and one N-1
    // holds otherwise is flipping with the title's double buffering.
    static bool movedIn(float before, float after) {
        return isNumber(before) && isNumber(after) && !sameBits(before, after);
    }

    // How far the title's own rounding can set a value off its path, in
    // units in its last place. A uniform is typically a four-term dot
    // product (a matrix row times a vector), each term and each sum rounded,
    // so it can stand up to about four units off the exact value: that much
    // at each end moves the midpoint as far, and that much at N-1 again.
    static constexpr double kRoundingUnits = 8.0;

    // How far rounding alone can set a value of this size off its path.
    static double rounding(float value) {
        return kRoundingUnits * unitInLastPlace(value);
    }

    // The distance from a float to the next one away from zero.
    static double unitInLastPlace(float value) {
        float magnitude = std::abs(value);
        return static_cast<double>(
                   std::nextafter(magnitude, std::numeric_limits<float>::infinity())) -
               magnitude;
    }

    // One value at N-2, its candidate's at N-1, and N.
    void add(float before, float between, float after) {
        addExpected({.before = before, .between = between, .after = after},
                    (static_cast<double>(before) + after) / 2.0);
    }

    // The same, where the candidate's own value at N-3 is known too: a path
    // that turns passes at N-1 through the curve over N-3, N-2 and N, not
    // through the midpoint of N-2 and N. A different object's N-3 bends that
    // curve away from its N-1 as surely as its N-1 is off the midpoint. A
    // value with no number at N-3 has no curve, and is judged by its
    // midpoint.
    void addCurved(float threeBack, float before, float between, float after) {
        if (!isNumber(threeBack)) {
            add(before, between, after);
            return;
        }
        addExpected({.before = before, .between = between, .after = after},
                    (-static_cast<double>(threeBack) / 3.0) + before +
                        (static_cast<double>(after) / 3.0));
    }

    // Values it moved in, and of those the candidate had a number at.
    size_t moved() const {
        return m_moved;
    }

    size_t compared() const {
        return m_compared;
    }

    // The candidate holds every value the object moved in bit for bit as it
    // was at N-2: the object stood still until N, and its move has no step
    // before it to be checked against.
    bool stoodAtStart() const {
        return m_moved > 0 && m_stood == m_moved;
    }

    // The candidate is the object's partner: what moved passes through it,
    // at one number of it at least, or the check would pass on anything. An
    // object that moved in nothing draws the same with any candidate, so any
    // is.
    bool landsOn() const {
        if (m_moved == 0) {
            return true;
        }
        return m_compared > 0 &&
               std::sqrt(m_residualSquared) <= kTolerance * std::sqrt(m_stepSquared);
    }

  private:
    // One value at N-2, its candidate's at N-1, and N.
    struct Sample {
        float before;
        float between;
        float after;
    };

    // `expected` is where the object would stand at N-1 on its path.
    void addExpected(Sample sample, double expected) {
        auto [before, between, after] = sample;
        if (!movedIn(before, after)) {
            return;
        }
        ++m_moved;
        if (!isNumber(between)) {
            return;
        }
        if (sameBits(between, before)) {
            ++m_stood;
        }
        double moved = static_cast<double>(after) - before;
        // The title rounds what it computes to a float: within its rounding
        // of the candidate, the object is on it. Far from the origin a move
        // is a few units in the last place, and rounding alone would
        // otherwise set the object off its own path.
        double off = std::max(0.0, std::abs(expected - between) - rounding(between));
        m_stepSquared += moved * moved;
        m_residualSquared += off * off;
        ++m_compared;
    }

    double m_stepSquared{0.0};
    double m_residualSquared{0.0};
    size_t m_compared{0};
    size_t m_moved{0};
    size_t m_stood{0};
};

} // namespace wiiuport::interp
