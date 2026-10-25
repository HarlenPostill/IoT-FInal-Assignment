#pragma once
#include <cmath>
#include <cstring>
#include <stdint.h>

namespace Cabinet {
class SequenceGuard {
 public:
  bool accept(const char *boot, uint32_t sequence, bool &restarted) {
    if (!boot || std::strlen(boot) != 16) return false;
    for (int i = 0; i < 16; ++i)
      if (!((boot[i] >= '0' && boot[i] <= '9') || (boot[i] >= 'a' && boot[i] <= 'f') ||
            (boot[i] >= 'A' && boot[i] <= 'F'))) return false;
    restarted = !seen || std::strcmp(boot, previousBoot) != 0;
    if (!restarted && int32_t(sequence - previousSequence) <= 0) return false;
    std::memcpy(previousBoot, boot, 17); previousSequence = sequence; seen = true;
    return true;
  }
 private:
  char previousBoot[17]{};
  uint32_t previousSequence = 0;
  bool seen = false;
};
struct Settings {
  float triggerDb = 90, restAngle = 110, pressAngle = 70, doorThresholdCm = 20;
  uint32_t sustainMs = 3000, travelMs = 500, holdMs = 500;
};
inline bool valid(const Settings &s) {
  return std::isfinite(s.triggerDb) && s.triggerDb >= 30 && s.triggerDb <= 120 &&
      std::isfinite(s.restAngle) && s.restAngle >= 0 && s.restAngle <= 180 &&
      std::isfinite(s.pressAngle) && s.pressAngle >= 0 && s.pressAngle <= 180 &&
      std::fabs(s.restAngle - s.pressAngle) >= 5 &&
      std::isfinite(s.doorThresholdCm) && s.doorThresholdCm >= 5 && s.doorThresholdCm <= 350 &&
      s.sustainMs >= 1000 && s.sustainMs <= 30000 &&
      s.travelMs >= 200 && s.travelMs <= 3000 && s.holdMs >= 100 && s.holdMs <= 3000;
}

// Only a newly accepted, fresh source sample can complete qualification.
class NoiseGate {
 public:
  bool armed = true, fresh = false;
  float db = NAN;
  uint32_t lastAt = 0, qualifiedMs = 0;
  void invalidate() { fresh = false; high = low = false; qualifiedMs = 0; }
  void latch() { armed = false; high = low = false; qualifiedMs = 0; }
  void expire(uint32_t now, uint32_t maxGap) {
    if (fresh && uint32_t(now - lastAt) > maxGap) invalidate();
  }
  bool sample(float value, uint32_t now, const Settings &s, uint32_t maxGap) {
    expire(now, maxGap);
    if (!std::isfinite(value)) { invalidate(); return false; }
    fresh = true; lastAt = now; db = value;
    if (!armed) {
      high = false; qualifiedMs = 0;
      if (value < s.triggerDb - 2) {
        if (!low) { low = true; lowAt = now; }
        if (uint32_t(now - lowAt) >= 3000) { armed = true; low = false; }
      } else low = false;
      return false;
    }
    if (value < s.triggerDb) { high = false; qualifiedMs = 0; return false; }
    if (!high) { high = true; highAt = now; qualifiedMs = 0; }
    qualifiedMs = uint32_t(now - highAt);
    if (qualifiedMs >= s.sustainMs) { latch(); return true; }
    return false;
  }
 private:
  bool high = false, low = false;
  uint32_t highAt = 0, lowAt = 0;
};

enum class Phase : uint8_t { Idle, Down, Hold, Return };
inline const char *phaseName(Phase p) {
  switch (p) { case Phase::Down: return "pressing"; case Phase::Hold: return "holding";
    case Phase::Return: return "returning"; default: return "idle"; }
}
class ServoCycle {
 public:
  Phase phase = Phase::Idle;
  float angle = Settings{}.restAngle;
  uint32_t pressCount = 0;
  bool start(uint32_t now, const Settings &s) {
    if (phase != Phase::Idle) return false;
    active = s; at = now; phase = Phase::Down; angle = s.restAngle; ++pressCount;
    return true;
  }
  void tick(uint32_t now) {
    const uint32_t elapsed = now - at;
    if (phase == Phase::Down || phase == Phase::Return) {
      const float fraction = elapsed >= active.travelMs ? 1 : float(elapsed) / active.travelMs;
      angle = phase == Phase::Down ? active.restAngle + fraction * (active.pressAngle - active.restAngle)
                                  : active.pressAngle + fraction * (active.restAngle - active.pressAngle);
      if (elapsed >= active.travelMs) { phase = phase == Phase::Down ? Phase::Hold : Phase::Idle; at = now; }
    } else if (phase == Phase::Hold && elapsed >= active.holdMs) { phase = Phase::Return; at = now; }
  }
 private:
  Settings active;
  uint32_t at = 0;
};

enum class Door : uint8_t { Closed, Open, Unknown };
inline const char *doorName(Door d) {
  return d == Door::Open ? "open" : d == Door::Closed ? "closed" : "unknown";
}
class DoorMonitor {
 public:
  Door state = Door::Unknown;
  float distance = NAN;
  void reset() { state = candidate = Door::Unknown; distance = NAN; count = index = 0; }
  void sample(float cm, uint32_t now, float threshold, float hysteresis, uint32_t settle) {
    if (!std::isfinite(cm) || cm < 2 || cm > 400) { reset(); return; }
    values[index] = cm; index = (index + 1) % 5;
    if (count < 5) ++count;
    if (count < 5) return;
    float sorted[5];
    for (int i = 0; i < 5; ++i) sorted[i] = values[i];
    for (int i = 1; i < 5; ++i) for (int j = i; j > 0 && sorted[j] < sorted[j-1]; --j) {
      const float temp = sorted[j]; sorted[j] = sorted[j-1]; sorted[j-1] = temp;
    }
    distance = sorted[2];
    Door next = state;
    if (distance > threshold) next = Door::Open;
    else if (state != Door::Open || distance <= threshold - hysteresis) next = Door::Closed;
    if (candidate != next) { candidate = next; candidateAt = now; }
    if (uint32_t(now - candidateAt) >= settle) state = candidate;
  }
 private:
  float values[5]{};
  uint8_t count = 0, index = 0;
  Door candidate = Door::Unknown;
  uint32_t candidateAt = 0;
};
}
