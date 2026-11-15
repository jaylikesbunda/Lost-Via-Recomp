#pragma once
#include <bitset>
#include <cstdint>
#include <utility>
#include <vector>

namespace lost::native {
// Remember the value at a draw, rather than a later write in the same D3D call.
class DrawInputHistory {
 public:
  static constexpr uint32_t kFirst = 0x4000, kEnd = 0x4930;
  void Begin() { active_ = false; seen_.reset(); changes_.clear(); }
  void Capture() { active_ = true; }
  void BeforeWrite(uint32_t reg, uint32_t old_value) {
    if (!active_ || reg < kFirst || reg >= kEnd || seen_[reg - kFirst]) return;
    seen_.set(reg - kFirst);
    changes_.emplace_back(reg, old_value);
  }
  const auto& changes() const { return changes_; }
 private:
  bool active_ = false;
  std::bitset<kEnd - kFirst> seen_;
  std::vector<std::pair<uint32_t, uint32_t>> changes_;
};
}
