// Lost native renderer: the draw flight recorder's request channel (flight_recorder.h).
#include "flight_recorder.h"

#include <deque>
#include <mutex>

namespace lost::native::flight {
namespace {

struct Pending {
  uint64_t from, to, label;
};
std::mutex g_mutex;
std::deque<Pending> g_pending;

}  // namespace

void Request(uint64_t from, uint64_t to, uint64_t label) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pending.size() < 16) g_pending.push_back({from, to, label});
}

bool Take(uint64_t current_frame, uint64_t* from, uint64_t* to, uint64_t* label) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pending.empty() || current_frame <= g_pending.front().to) return false;
  const Pending p = g_pending.front();
  g_pending.pop_front();
  *from = p.from;
  *to = p.to;
  *label = p.label;
  return true;
}

}  // namespace lost::native::flight
