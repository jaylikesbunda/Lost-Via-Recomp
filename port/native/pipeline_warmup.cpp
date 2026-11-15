#include "pipeline_warmup.h"
#include "guest_memory.h"
#include "shader_specialize.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>

REXCVAR_DEFINE_STRING(lost_native_pipeline_manifest, "", "Lost native",
                      "Code-free pipeline manifest (empty: pipelines.manifest "
                      "beside the executable)");
namespace lost::native {
namespace {
struct ShaderInfo {
  uint64_t hash;
  uint32_t type, words;
};
// Typed input-layout properties, never shader opcodes or instruction words.
struct VertexInputOverride {
  uint32_t slot;
  std::array<uint32_t, 11> fields;
};
struct ShaderRecipe {
  ShaderInfo target;
  uint32_t program, specialized;
  std::array<uint32_t, 8> bools;
  std::vector<VertexInputOverride> inputs;
};
struct Manifest {
  uint32_t key_size = 0, key_version = 0;
  std::unordered_multimap<uint64_t, ShaderInfo> prefixes;
  std::unordered_map<uint64_t, ShaderInfo> hashes;
  std::unordered_set<uint64_t> sizes;
  std::vector<PipelineStore::Pipeline> pipelines;
  bool recipe_format = false;
  std::unordered_multimap<uint32_t, ShaderRecipe> recipes;
};
const Manifest &GetManifest() {
  static const Manifest manifest = [] {
    Manifest result;
    std::filesystem::path path = REXCVAR_GET(lost_native_pipeline_manifest);
    if (path.empty()) {
      std::wstring executable(32768, L'\0');
      const DWORD size = GetModuleFileNameW(nullptr, executable.data(),
                                            DWORD(executable.size()));
      if (!size || size >= executable.size())
        return result;
      executable.resize(size);
      path = std::filesystem::path(executable).parent_path() /
             "pipelines.manifest";
      std::error_code ec;
      if (!std::filesystem::exists(path, ec))
        return result;
    }
    const auto data = ReadWholeFile(path);
    size_t pos = 0;
    auto take = [&](void *out, size_t n) {
      if (n > data.size() - pos)
        return false;
      std::memcpy(out, data.data() + pos, n);
      pos += n;
      return true;
    };
    char magic[4];
    uint32_t version, shaders, pipelines;
    if (!take(magic, 4) || std::memcmp(magic, "LPWM", 4) ||
        !take(&version, 4) || (version != 1 && version != 2) ||
        !take(&result.key_size, 4) || !take(&result.key_version, 4) ||
        !take(&shaders, 4) || !take(&pipelines, 4) || !result.key_size ||
        result.key_size > PipelineStore::kMaxKeySize || shaders > 100000 ||
        pipelines > 100000 ||
        data.size() < 24ull + shaders * (version == 1 ? 24ull : 28ull) +
                          pipelines * (32ull + result.key_size)) {
      REXLOG_WARN("Lost native warmup: invalid manifest {}", path.string());
      return Manifest{};
    }
    result.recipe_format = version >= 2;
    for (uint32_t i = 0; i < shaders; ++i) {
      ShaderInfo info;
      uint64_t prefix;
      if (!take(&info.hash, 8) || !take(&info.type, 4) ||
          !take(&info.words, 4) || !take(&prefix, 8) || info.type > 1 ||
          info.words < 4 || info.words > 16384)
        return Manifest{};
      result.prefixes.emplace(prefix, info);
      result.hashes.emplace(info.hash, info);
      result.sizes.insert(uint64_t(info.type) << 32 | info.words);
      if (version == 1)
        continue;
      uint32_t bytes;
      if (!take(&bytes, 4) || bytes > data.size() - pos)
        return Manifest{};
      if (!bytes)
        continue;
      const size_t end = pos + bytes;
      ShaderRecipe recipe;
      recipe.target = info;
      uint32_t record, count;
      if (bytes < 48 || !take(&record, 4) || !take(&recipe.program, 4) ||
          !take(&recipe.specialized, 4) || !take(recipe.bools.data(), 32) ||
          !take(&count, 4) || count > info.words / 3 ||
          bytes != 48ull + count * 48ull ||
          (recipe.program != 24 && recipe.program != 32) ||
          recipe.specialized > 1)
        return Manifest{};
      recipe.inputs.resize(count);
      for (auto &input : recipe.inputs)
        if (!take(&input.slot, 4) || !take(input.fields.data(), 44) ||
            input.slot >= info.words / 3)
          return Manifest{};
      if (pos != end)
        return Manifest{};
      result.recipes.emplace(record, std::move(recipe));
    }
    for (uint32_t i = 0; i < pipelines; ++i) {
      PipelineStore::Pipeline pipeline;
      if (!take(&pipeline.vs_hash, 8) || !take(&pipeline.vs_mod, 8) ||
          !take(&pipeline.ps_hash, 8) || !take(&pipeline.ps_mod, 8))
        return Manifest{};
      pipeline.key.resize(result.key_size);
      if (!take(pipeline.key.data(), result.key_size))
        return Manifest{};
      result.pipelines.push_back(std::move(pipeline));
    }
    if (pos != data.size())
      return Manifest{};
    REXLOG_INFO(
        "Lost native warmup: manifest has {} shader hashes, {} pipelines",
        shaders, pipelines);
    return result;
  }();
  return manifest;
}
std::mutex g_mutex;
std::unordered_set<uint64_t> g_seen;
std::vector<std::pair<uint64_t, PipelineStore::Shader>> g_created;
} // namespace
std::vector<PipelineStore::Pipeline> ManifestPipelines(uint32_t key_size,
                                                       uint32_t key_version) {
  const Manifest &m = GetManifest();
  if (m.key_size != key_size || m.key_version != key_version)
    return {};
  return m.pipelines;
}
namespace {
uint32_t OfferProgram(const Manifest &manifest, uint32_t type,
                      const uint32_t *bytes, uint32_t count) {
  const uint64_t hash = XXH3_64bits(bytes, size_t(count) * 4);
  auto it = manifest.hashes.find(hash);
  if (it == manifest.hashes.end() || it->second.type != type ||
      it->second.words != count)
    return 0;
  std::lock_guard lock(g_mutex);
  if (!g_seen.insert(hash).second)
    return 0;
  PipelineStore::Shader shader{type,
                               std::vector<uint32_t>(bytes, bytes + count)};
  g_created.emplace_back(hash, std::move(shader));
  return 1;
}

uint32_t DiscoverProgram(uint32_t type, const uint8_t *bytes, uint32_t length) {
  const Manifest &manifest = GetManifest();
  const uint32_t count = length / 4;
  if (length % 12 || !manifest.sizes.count(uint64_t(type) << 32 | count))
    return 0;
  uint32_t matched = 0;
  namespace ucode = rex::graphics::ucode;
  using Op = ucode::ControlFlowOpcode;
  std::vector<uint32_t> guest(count), host(count);
  std::memcpy(guest.data(), bytes, length);
  matched += OfferProgram(manifest, type, guest.data(), count);
  for (size_t i = 0; i < count; ++i)
    host[i] = __builtin_bswap32(guest[i]);
  uint32_t bound = count / 3;
  std::vector<uint32_t> bits;
  for (uint32_t i = 0; i < bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(host.data() + i * 3, pair);
    for (const auto &cf : pair)
      if (ucode::IsControlFlowOpcodeExec(cf.opcode()))
        bound = std::min(bound, cf.exec.address());
  }
  for (uint32_t i = 0; i < bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(host.data() + i * 3, pair);
    for (const auto &cf : pair) {
      if (cf.opcode() == Op::kCondExec || cf.opcode() == Op::kCondExecEnd ||
          cf.opcode() == Op::kCondExecPredClean ||
          cf.opcode() == Op::kCondExecPredCleanEnd)
        bits.push_back(cf.cond_exec.bool_address());
      else if ((cf.opcode() == Op::kCondJmp || cf.opcode() == Op::kCondCall) &&
               !cf.cond_jmp.is_unconditional() && !cf.cond_jmp.is_predicated())
        bits.push_back(cf.cond_jmp.bool_address());
    }
  }
  std::sort(bits.begin(), bits.end());
  bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
  // The supported disc has at most six. Bound work for foreign packs.
  if (bits.empty() || bits.size() > 12)
    return matched;
  for (uint32_t assignment = 0; assignment < (1u << bits.size());
       ++assignment) {
    uint32_t bools[8] = {};
    for (size_t i = 0; i < bits.size(); ++i)
      if (assignment & (1u << i))
        bools[bits[i] / 32] |= 1u << (bits[i] % 32);
    std::vector<uint32_t> variant = host;
    if (!SpecializeBoolControlFlow(variant.data(), bound, bools))
      continue;
    for (size_t i = 0; i < count; ++i)
      guest[i] = __builtin_bswap32(variant[i]);
    // Accept only an exact target hash: output compatibility was established
    // when that shader was used to record its manifest pipeline.
    matched += OfferProgram(manifest, type, guest.data(), count);
  }
  return matched;
}
} // namespace

namespace {
uint32_t ReconstructProgram(const Manifest &manifest,
                            const ShaderRecipe &recipe, uint32_t type,
                            const uint8_t *bytes, uint32_t length) {
  if (type != recipe.target.type || length != recipe.target.words * 4)
    return 0;
  const uint32_t count = length / 4;
  std::vector<uint32_t> host(count), guest(count);
  std::memcpy(guest.data(), bytes, length);
  for (uint32_t i = 0; i < count; ++i)
    host[i] = __builtin_bswap32(guest[i]);
  namespace ucode = rex::graphics::ucode;
  uint32_t bound = count / 3;
  for (uint32_t i = 0; i < bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(host.data() + i * 3, pair);
    for (const auto &cf : pair)
      if (ucode::IsControlFlowOpcodeExec(cf.opcode()))
        bound = std::min(bound, cf.exec.address());
  }
  std::vector<bool> fetch(count / 3, false);
  for (uint32_t i = 0; i < bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(host.data() + i * 3, pair);
    for (const auto &cf : pair) {
      if (!ucode::IsControlFlowOpcodeExec(cf.opcode()))
        continue;
      for (uint32_t j = 0; j < cf.exec.count(); ++j) {
        const uint32_t slot = cf.exec.address() + j;
        if (slot < fetch.size() && (cf.exec.sequence() >> (j * 2) & 1) &&
            (host[slot * 3] & 31) == 0)
          fetch[slot] = true;
      }
    }
  }
  // destination, constant index/selector, prefetch count, output swizzle,
  // signed/normalized flags, format, mini-fetch, stride, signed dword offset.
  constexpr uint32_t fields[11][3] = {
      {0, 12, 6}, {0, 20, 5}, {0, 25, 2}, {0, 27, 3}, {1, 0, 12}, {1, 12, 1},
      {1, 13, 1}, {1, 16, 6}, {1, 30, 1}, {2, 0, 8},  {2, 8, 23}};
  for (const auto &input : recipe.inputs) {
    if (type != 0 || input.slot >= fetch.size() || !fetch[input.slot])
      return 0;
    for (size_t i = 0; i < 11; ++i) {
      const uint32_t word = fields[i][0], shift = fields[i][1],
                     bits = fields[i][2];
      const uint32_t mask = ((1u << bits) - 1) << shift;
      uint32_t &value = host[input.slot * 3 + word];
      value = (value & ~mask) | ((input.fields[i] << shift) & mask);
    }
  }
  if (recipe.specialized)
    SpecializeBoolControlFlow(host.data(), bound, recipe.bools.data());
  for (uint32_t i = 0; i < count; ++i)
    guest[i] = __builtin_bswap32(host[i]);
  if (XXH3_64bits(guest.data(), length) != recipe.target.hash)
    return 0;
  return OfferProgram(manifest, type, guest.data(), count);
}
} // namespace

void DiscoverDiscShaders(const std::filesystem::path &pack) {
  const Manifest &manifest = GetManifest();
  if (manifest.hashes.empty())
    return;
  const auto started = std::chrono::steady_clock::now();
  const auto data = ReadWholeFile(pack);
  auto word = [&](size_t at) {
    uint32_t value;
    std::memcpy(&value, data.data() + at, 4);
    return __builtin_bswap32(value);
  };
  if (data.size() < 24 || word(0) != 0x53444658 || word(4) != 4) {
    REXLOG_WARN("Lost native warmup: no supported disc shader pack at {}",
                pack.string());
    return;
  }
  const uint32_t records = word(16);
  size_t pos = 24;
  uint32_t programs = 0, matched = 0;
  for (uint32_t record = 0; record < records; ++record) {
    if (data.size() - pos < 32)
      break;
    const uint32_t size = word(pos + 24);
    pos += 32;
    if (size > data.size() - pos || size < 40)
      break;
    const size_t blob = pos;
    pos += size;
    const uint32_t header = word(blob + 4), code = word(blob + 8);
    if (uint64_t(header) + code != size || header < 40)
      continue;
    const uint32_t type = word(blob) & 1 ? 0 : 1;
    for (size_t field : {24u, 32u}) {
      const uint32_t descriptor = word(blob + field);
      if (!descriptor || descriptor > header - 8)
        continue;
      const uint32_t begin = word(blob + descriptor),
                     length = word(blob + descriptor + 4);
      if (begin > code || length > code - begin || length % 12)
        continue;
      ++programs;
      const uint32_t count = length / 4;
      if (!manifest.sizes.count(uint64_t(type) << 32 | count))
        continue;
      if (manifest.recipe_format) {
        auto [first, last] = manifest.recipes.equal_range(record);
        for (auto it = first; it != last; ++it)
          if (it->second.program == field)
            matched +=
                ReconstructProgram(manifest, it->second, type,
                                   data.data() + blob + header + begin, length);
      } else {
        matched +=
            DiscoverProgram(type, data.data() + blob + header + begin, length);
      }
    }
  }
  REXLOG_INFO("Lost native warmup: {} disc programs, {} manifest shaders "
              "reconstructed in {:.1f} ms",
              programs, matched,
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - started)
                  .count());
}

void ObserveShaderBlob(const uint8_t *data, uint32_t size) {
  const Manifest &m = GetManifest();
  if (m.prefixes.empty() || size < 16 || size > 4 * 1024 * 1024)
    return;
  static std::atomic<uint32_t> observed{0};
  const uint32_t count = ++observed;
  if (count <= 8)
    REXLOG_INFO("Lost native warmup: shader blob {} ({} bytes)", count, size);
  // The blob can contain multiple shader programs and declaration variants.
  // Prefix fingerprints select candidates; only a full microcode hash match is
  // accepted.
  for (uint32_t offset = 0; offset <= size - 16; offset += 4) {
    uint64_t prefix = 14695981039346656037ull;
    for (uint32_t j = 0; j < 16; ++j)
      prefix = (prefix ^ data[offset + j]) * 1099511628211ull;
    auto [first, last] = m.prefixes.equal_range(prefix);
    for (auto it = first; it != last; ++it) {
      const ShaderInfo &info = it->second;
      if (info.words * 4 > size - offset ||
          XXH3_64bits(data + offset, info.words * 4) != info.hash)
        continue;
      std::lock_guard lock(g_mutex);
      if (!g_seen.insert(info.hash).second)
        continue;
      PipelineStore::Shader shader;
      shader.type = info.type;
      shader.ucode.resize(info.words);
      std::memcpy(shader.ucode.data(), data + offset, info.words * 4);
      g_created.emplace_back(info.hash, std::move(shader));
    }
  }
}
void ObserveShaderObject(const uint8_t *base, uint32_t object, bool vertex) {
  if (GetManifest().hashes.empty() || !object)
    return;
  const uint32_t h = object + (vertex ? 872 : 40);
  const uint32_t header = Load32(base, h + 4), code_size = Load32(base, h + 8);
  const uint32_t code = Load32(base, object + (vertex ? 32 : 24));
  if (header < 40 || header > 1024 * 1024 || !code ||
      code_size > 4 * 1024 * 1024 || uint64_t(code) + code_size > 0xffffffffull)
    return;
  uint32_t matched = 0;
  for (uint32_t field : {24u, 32u}) {
    const uint32_t descriptor = Load32(base, h + field);
    if (!descriptor || descriptor > header - 8)
      continue;
    const uint32_t begin = Load32(base, h + descriptor),
                   length = Load32(base, h + descriptor + 4);
    if (begin <= code_size && length <= code_size - begin)
      matched +=
          DiscoverProgram(vertex ? 0 : 1, HostPtr(base, code + begin), length);
  }
  if (matched)
    REXLOG_INFO("Lost native warmup: {} {} shaders from object creation",
                matched, vertex ? "vertex" : "pixel");
}
std::vector<std::pair<uint64_t, PipelineStore::Shader>> TakeCreatedShaders() {
  std::vector<std::pair<uint64_t, PipelineStore::Shader>> result;
  std::lock_guard lock(g_mutex);
  result.swap(g_created);
  return result;
}
} // namespace lost::native

// sub_8239D2F8 allocates and copies a compiled shader blob. Header +4 is the
// header byte size; +8 is the code byte size. B898 applies declaration patches.
REX_EXTERN(__imp__sub_8239D2F8);
REX_HOOK_RAW(sub_8239D2F8) {
  const uint32_t blob = ctx.r3.u32;
  const uint32_t header_size = lost::native::Load32(base, blob + 4);
  const uint32_t code_size = lost::native::Load32(base, blob + 8);
  __imp__sub_8239D2F8(ctx, base);
  if (ctx.r3.u32 && header_size <= 1024 * 1024 &&
      uint64_t(blob) + header_size + code_size <= 0xffffffffull)
    lost::native::ObserveShaderBlob(
        lost::native::HostPtr(base, blob + header_size), code_size);
}
REX_EXTERN(__imp__sub_8239B898);
REX_HOOK_RAW(sub_8239B898) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_8239B898(ctx, base);
  lost::native::ObserveShaderObject(base, object, true);
}

REX_EXTERN(__imp__sub_8239B6A0);
REX_HOOK_RAW(sub_8239B6A0) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_8239B6A0(ctx, base);
  lost::native::ObserveShaderObject(base, object, false);
}
