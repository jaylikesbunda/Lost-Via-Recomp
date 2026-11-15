// GRAW 360 - crash reports. See crash_report.h.

#include "crash_report.h"

#include <rex/logging.h>

#if defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// After windows.h.
#include <dbghelp.h>
#include <tlhelp32.h>

namespace graw::crash_report {
namespace {

std::wstring report_dir;
volatile LONG reporting = 0;

std::wstring Stamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t text[32];
  swprintf(text, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
           t.wSecond);
  return text;
}

// "graw.exe+0x1234" for an address in a loaded module.
std::string ModuleOffset(DWORD64 address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address), &module)) {
    return "?";
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = std::strrchr(path, '\\');
  char text[MAX_PATH + 32];
  std::snprintf(text, sizeof(text), "%s+0x%llX", name ? name + 1 : path,
                static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
  return text;
}

void WriteStack(FILE* out, CONTEXT context, HANDLE thread = GetCurrentThread()) {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(process, nullptr, TRUE);
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  alignas(SYMBOL_INFO) char symbol_storage[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !frame.AddrPC.Offset) {
      break;
    }
    const DWORD64 pc = frame.AddrPC.Offset;
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    const bool named = SymFromAddr(process, pc, &displacement, symbol);
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    const bool has_line = SymGetLineFromAddr64(process, pc, &line_displacement, &line);
    std::fprintf(out, "  #%02d %016llX %s %s+0x%llX%s%s:%lu\n", i,
                 static_cast<unsigned long long>(pc), ModuleOffset(pc).c_str(),
                 named ? symbol->Name : "?", static_cast<unsigned long long>(displacement),
                 has_line ? "  " : "", has_line ? line.FileName : "",
                 has_line ? line.LineNumber : 0ul);
  }
  SymCleanup(process);
}

void WriteDump(const std::wstring& path, EXCEPTION_POINTERS* pointers) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION info = {GetCurrentThreadId(), pointers, FALSE};
  // Not the full memory: the guest's 4 GB address space would make it enormous.
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                    MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                  MiniDumpWithUnloadedModules),
                    pointers ? &info : nullptr, nullptr, nullptr);
  CloseHandle(file);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
  if (InterlockedExchange(&reporting, 1)) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
  const DWORD64 address = reinterpret_cast<DWORD64>(record.ExceptionAddress);
  const std::wstring base = report_dir + L"\\crash-" + Stamp();
  REXLOG_CRITICAL("GRAW: crash: exception {:08X} at {} on thread {}; report in {}.txt",
                  static_cast<unsigned>(record.ExceptionCode), ModuleOffset(address),
                  GetCurrentThreadId(), std::filesystem::path(base).string());
  rex::FlushLogging();
  WriteDump(base + L".dmp", pointers);
  if (FILE* out = _wfopen((base + L".txt").c_str(), L"w")) {
    std::fprintf(out, "Exception %08X at %016llX (%s), thread %lu\n",
                 static_cast<unsigned>(record.ExceptionCode),
                 static_cast<unsigned long long>(address), ModuleOffset(address).c_str(),
                 GetCurrentThreadId());
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
      std::fprintf(out, "Access violation: %s %016llX\n",
                   record.ExceptionInformation[0] == 0   ? "reading"
                   : record.ExceptionInformation[0] == 1 ? "writing"
                                                         : "executing",
                   static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }
    if (record.ExceptionCode == EXCEPTION_BREAKPOINT) {
      std::fprintf(out, "A breakpoint: the SDK's rex::debug::Break(), e.g. the game called "
                        "KeBugCheck, or an assertion.\n");
    }
    const CONTEXT& c = *pointers->ContextRecord;
    std::fprintf(out,
                 "rax %016llX rbx %016llX rcx %016llX rdx %016llX\n"
                 "rsi %016llX rdi %016llX rbp %016llX rsp %016llX\n"
                 "r8  %016llX r9  %016llX r10 %016llX r11 %016llX\n"
                 "r12 %016llX r13 %016llX r14 %016llX r15 %016llX\n",
                 c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10,
                 c.R11, c.R12, c.R13, c.R14, c.R15);
    // The callee-saved vector registers: the generated code keeps values in them across its host
    // calls (docs/HANDOFF-REGOPTS.md, section 7), and under Wine the dump viewer is not at hand.
    const M128A* xmm = &c.Xmm6;
    for (int i = 0; i < 10; i += 2) {
      std::fprintf(out, "xmm%-2d %016llX_%016llX xmm%-2d %016llX_%016llX\n", 6 + i,
                   static_cast<unsigned long long>(xmm[i].High),
                   static_cast<unsigned long long>(xmm[i].Low), 7 + i,
                   static_cast<unsigned long long>(xmm[i + 1].High),
                   static_cast<unsigned long long>(xmm[i + 1].Low));
    }
    std::fprintf(out, "mxcsr %08lX\n", static_cast<unsigned long>(c.MxCsr));
    // Wine (Proton) names itself through ntdll; the logs don't say which build ran.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
      using GetVersion = const char*(__cdecl*)();
      auto version = reinterpret_cast<GetVersion>(GetProcAddress(ntdll, "wine_get_version"));
      auto build = reinterpret_cast<GetVersion>(GetProcAddress(ntdll, "wine_get_build_id"));
      if (version) {
        std::fprintf(out, "Wine %s (%s)\n", version(), build ? build() : "no build id");
      }
    }
    std::fprintf(out, "Stack:\n");
    WriteStack(out, c);
    std::fclose(out);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void OnExit() {
  REXLOG_WARN("GRAW: the process is ending through exit() (e.g. the game's "
              "HalReturnToFirmware)");
  rex::FlushLogging();
}

void OnAbort(int) {
  REXLOG_CRITICAL("GRAW: abort() called");
  rex::FlushLogging();
  // Make it an exception, so the report and dump are written.
  RaiseException(0xE0000ABD, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

}  // namespace

void InstallCrashReporter() {
  std::error_code error;
  const auto dir = std::filesystem::current_path(error) / "logs";
  std::filesystem::create_directories(dir, error);
  report_dir = dir.wstring();
  SetUnhandledExceptionFilter(OnUnhandledException);
  std::atexit(OnExit);
  std::signal(SIGABRT, OnAbort);
  REXLOG_INFO("GRAW: crash reports go to {}", dir.string());
}

// Every other thread's stack (for a hang): suspend, read its context, walk, resume.
std::string DumpAllThreads() {
  const std::wstring path = report_dir + L"\\hang-" + Stamp() + L".txt";
  FILE* out = _wfopen(path.c_str(), L"w");
  if (!out) return {};
  const DWORD self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, te.th32ThreadID);
    if (!t) continue;
    if (SuspendThread(t) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t, &c)) {
        std::fprintf(out, "thread %lu\n", te.th32ThreadID);
        WriteStack(out, c, t);
      }
      ResumeThread(t);
    }
    CloseHandle(t);
  }
  CloseHandle(snap);
  std::fclose(out);
  return std::filesystem::path(path).string();
}

// A sampling profiler (no tools needed on the player's or developer's machine):
//   1. 2 s at ~200 Hz over every thread, top of stack only, to find the busy ones (top frame not
//      a kernel wait);
//   2. `seconds` at ~1 kHz over the 6 busiest, 16-frame stacks.
// Writes logs/profile-<time>.txt: per thread, the hottest functions by self and inclusive time.
namespace {

bool IsWaitSymbol(const std::string& name) {
  static const char* kWaits[] = {"Wait", "Delay", "Sleep", "NtRemoveIoCompletion",
                                 "ZwRemoveIoCompletion", "NtReplyWaitReceivePort", "Alert",
                                 "NtGdiDdDDI", "select"};
  for (const char* w : kWaits) {
    if (name.find(w) != std::string::npos) return true;
  }
  return false;
}

struct Symbolizer {
  HANDLE process = GetCurrentProcess();
  std::map<DWORD64, std::string> cache;
  Symbolizer() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
  }
  ~Symbolizer() { SymCleanup(process); }
  const std::string& Name(DWORD64 pc) {
    auto it = cache.find(pc);
    if (it != cache.end()) return it->second;
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string name = SymFromAddr(process, pc, &displacement, symbol)
                           ? std::string(symbol->Name)
                           : ModuleOffset(pc);
    const std::string module = ModuleOffset(pc);
    const size_t plus = module.find('+');
    name = module.substr(0, plus) + "!" + name;
    return cache.emplace(pc, std::move(name)).first->second;
  }
};

// Up to `depth` return addresses of a suspended thread, top first.
std::vector<DWORD64> Walk(HANDLE thread, CONTEXT context, int depth) {
  std::vector<DWORD64> pcs;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < depth; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &context,
                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !frame.AddrPC.Offset) {
      break;
    }
    pcs.push_back(frame.AddrPC.Offset);
  }
  return pcs;
}

struct Sampled {
  HANDLE handle = nullptr;
  uint64_t samples = 0, busy = 0;
  std::map<std::string, uint64_t> self, inclusive;
};

bool Sample(HANDLE t, int depth, std::vector<DWORD64>& pcs) {
  if (SuspendThread(t) == DWORD(-1)) return false;
  CONTEXT c{};
  c.ContextFlags = CONTEXT_FULL;
  const bool ok = GetThreadContext(t, &c);
  if (ok) pcs = depth <= 1 ? std::vector<DWORD64>{c.Rip} : Walk(t, c, depth);
  ResumeThread(t);
  return ok && !pcs.empty();
}

}  // namespace

std::string ProfileThreads(int seconds) {
  Symbolizer sym;
  const DWORD self_id = GetCurrentThreadId();
  std::map<DWORD, Sampled> threads;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self_id) continue;
    if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, te.th32ThreadID)) {
      threads[te.th32ThreadID].handle = t;
    }
  }
  CloseHandle(snap);

  // Phase 1: which threads are busy.
  std::vector<DWORD64> pcs;
  const auto phase1_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < phase1_end) {
    for (auto& [id, s] : threads) {
      if (!Sample(s.handle, 1, pcs)) continue;
      ++s.samples;
      if (!IsWaitSymbol(sym.Name(pcs[0]))) ++s.busy;
    }
    Sleep(5);
  }
  std::vector<std::pair<uint64_t, DWORD>> order;
  for (auto& [id, s] : threads) order.emplace_back(s.busy, id);
  std::sort(order.rbegin(), order.rend());
  if (order.size() > 6) order.resize(6);

  // Phase 2: stacks of the busy threads.
  for (auto& [busy, id] : order) {
    threads[id].samples = threads[id].busy = 0;
  }
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < end) {
    for (auto& [busy, id] : order) {
      Sampled& s = threads[id];
      if (!Sample(s.handle, 16, pcs)) continue;
      ++s.samples;
      const std::string& top = sym.Name(pcs[0]);
      if (IsWaitSymbol(top)) continue;
      ++s.busy;
      ++s.self[top];
      std::vector<std::string> seen;
      for (DWORD64 pc : pcs) {
        const std::string& n = sym.Name(pc);
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        ++s.inclusive[n];
      }
    }
    Sleep(1);
  }

  const std::wstring path = report_dir + L"\\profile-" + Stamp() + L".txt";
  FILE* out = _wfopen(path.c_str(), L"w");
  if (out) {
    std::fprintf(out, "Sampling profile, %d s. Per busy thread: %% of its samples not in a wait.\n",
                 seconds);
    for (auto& [busy0, id] : order) {
      Sampled& s = threads[id];
      if (!s.samples) continue;
      std::fprintf(out, "\n== thread %lu: busy %.1f%% (%llu of %llu samples)\n", id,
                   100.0 * double(s.busy) / double(s.samples),
                   static_cast<unsigned long long>(s.busy),
                   static_cast<unsigned long long>(s.samples));
      for (const char* kind : {"self", "inclusive"}) {
        auto& m = std::strcmp(kind, "self") == 0 ? s.self : s.inclusive;
        std::vector<std::pair<uint64_t, std::string>> top;
        for (auto& [n, c] : m) top.emplace_back(c, n);
        std::sort(top.rbegin(), top.rend());
        std::fprintf(out, "  -- %s\n", kind);
        for (size_t i = 0; i < top.size() && i < 40; ++i) {
          std::fprintf(out, "  %5.1f%%  %s\n", 100.0 * double(top[i].first) / double(s.samples),
                       top[i].second.c_str());
        }
      }
    }
    std::fclose(out);
  }
  for (auto& [id, s] : threads) CloseHandle(s.handle);
  return out ? std::filesystem::path(path).string() : std::string();
}

}  // namespace graw::crash_report

#else

namespace graw::crash_report {
void InstallCrashReporter() {}
std::string DumpAllThreads() { return {}; }
std::string ProfileThreads(int) { return {}; }
}  // namespace graw::crash_report

#endif
