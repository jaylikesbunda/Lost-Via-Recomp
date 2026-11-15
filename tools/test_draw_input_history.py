"""Regression: post-draw material writes must not colour an earlier queued draw."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
state = (root / 'port/native/d3d_state.cpp').read_text()
start = state.index('void GuestStateTracker::RecordDrawInputs(')
end = state.index('\n}\n', start) + 2
replay = state[start:end]
source = r'''
#include "draw_input_history.h"
#include <array>
#include <cassert>
#include <cstdio>
using lost::native::DrawInputHistory;
struct Write { uint32_t reg, value; };
struct Log { std::vector<Write> writes; void Append(Write w) { writes.push_back(w); } };
struct GuestStateTracker {
  std::array<uint32_t, 0x5000> regs_{};
  DrawInputHistory draw_inputs_;
  Log* sink_ = nullptr;
  void RecordDrawInputs(bool);
};
REPLAY
int main() {
  GuestStateTracker tracker;
  Log before, after;
  tracker.sink_ = &before;
  auto put = [&](uint32_t reg, uint32_t value) {
    tracker.draw_inputs_.BeforeWrite(reg, tracker.regs_[reg]);
    tracker.regs_[reg] = value;
  };
  tracker.draw_inputs_.Begin();
  put(0x4400, 0x3f800000); // material value before draw
  put(0x4804, 0x1000);    // vertex fetch before draw
  tracker.draw_inputs_.Capture();
  put(0x4400, 0x40000000);
  put(0x4400, 0x40400000); // repeated writes must retain the FIRST old value
  put(0x4804, 0x2000);
  put(0x2081, 123);        // untiled scissor belongs to post-call setup
  assert(tracker.draw_inputs_.changes().size() == 2);
  tracker.RecordDrawInputs(false);
  auto gpu = tracker.regs_;
  for (auto w : before.writes) gpu[w.reg] = w.value;
  assert(gpu[0x4400] == 0x3f800000 && gpu[0x4804] == 0x1000);
  assert(gpu[0x2081] == 123);
  // Publishing the draw may replace the current queue batch and its log sink.
  tracker.sink_ = &after;
  tracker.RecordDrawInputs(true);
  for (auto w : after.writes) gpu[w.reg] = w.value;
  assert(gpu == tracker.regs_);
  tracker.draw_inputs_.Begin();
  assert(tracker.draw_inputs_.changes().empty());
  tracker.draw_inputs_.Capture();
  tracker.RecordDrawInputs(false);
  assert(after.writes.size() == 2); // next draw cannot inherit prior overrides
  puts("Draw input ordering: original material/fetch values preserved, repeated writes and batch publication restored correctly.");
}
'''.replace('REPLAY', replay)
with tempfile.TemporaryDirectory(prefix='lost-draw-inputs-') as directory:
    cpp = pathlib.Path(directory) / 'test.cpp'
    exe = pathlib.Path(directory) / 'test.exe'
    cpp.write_text(source)
    subprocess.run([str(root / '.tools/llvm/bin/clang++.exe'), '-std=c++20', '-O2',
                    '-I', str(root / 'port/native'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
