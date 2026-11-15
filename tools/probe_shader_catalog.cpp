// CPU-only verification of the production disc shader discovery path. No guest or GPU starts.
#include "pipeline_warmup.h"
#include "shader_cache.h"
#include <rex/ppc/context.h>
#include <rex/cvar.h>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <set>

// The linked objects contain optional guest hooks / specialization cache methods.
// This CPU probe must never enter those paths; trap if it does.
extern "C" void __imp__sub_8239D2F8(PPCContext&, uint8_t*) { std::abort(); }
extern "C" void __imp__sub_8239B898(PPCContext&, uint8_t*) { std::abort(); }
extern "C" void __imp__sub_8239B6A0(PPCContext&, uint8_t*) { std::abort(); }

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: probe_shader_catalog <pack.sdf> <pipelines.manifest> [expected]\n");
    return 2;
  }
  if (!rex::cvar::SetFlagByName("lost_native_pipeline_manifest", argv[2])) {
    std::fprintf(stderr, "could not configure manifest\n");
    return 2;
  }
  const auto started = std::chrono::steady_clock::now();
  lost::native::DiscoverDiscShaders(argv[1]);
  const auto shaders = lost::native::TakeCreatedShaders();
  std::printf("Production catalog reconstructed %zu manifest shaders without starting the game.\n",
              shaders.size());
  std::unordered_set<uint64_t> available;
  for (const auto& shader : shaders) available.insert(shader.first);
  const auto pipelines = lost::native::ManifestPipelines(112, 3);
  size_t ready = 0;
  for (const auto& pipeline : pipelines)
    if (available.count(pipeline.vs_hash) && (!pipeline.ps_hash || available.count(pipeline.ps_hash))) ++ready;
  std::printf("%zu / %zu pipeline descriptions have every shader ready; reconstruction took %.1f ms.\n",
              ready, pipelines.size(), std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - started).count());
  if (argc > 4 && std::string_view(argv[4]) == "--bindings") {
    lost::native::ShaderCache cache(0x10DE);
    std::unordered_map<uint64_t, lost::native::DxbcShader*> loaded;
    for (const auto& [hash, source] : shaders)
      loaded.emplace(hash, cache.Load(rex::graphics::xenos::ShaderType(source.type),
                                      source.ucode.data(), uint32_t(source.ucode.size())));
    uint32_t max_index = 0;
    size_t overflow = 0, failures = 0, layout_changes = 0;
    std::set<std::pair<uint64_t, uint64_t>> checked;
    auto check = [&](uint64_t hash, uint64_t modification) {
      const auto it = loaded.find(hash);
      if (it == loaded.end()) return;
      auto* shader = it->second;
      if (!cache.Translate(*shader, modification)) { ++failures; return; }
      if (!checked.emplace(hash, modification).second) return;
      // DxbcShader retains the bindings of its first translation. Independently
      // translate each modification to verify that this shared layout is valid.
      lost::native::DxbcShader isolated(shader->type(), hash, shader->ucode_dwords(),
                                      shader->ucode_dword_count(), std::endian::native);
      rex::string::StringBuffer disassembly;
      isolated.AnalyzeUcode(disassembly);
      bool is_new = false;
      auto* translation = isolated.GetOrCreateTranslation(modification, &is_new);
      const_cast<lost::native::DxbcShaderTranslator&>(cache.translator())
          .TranslateAnalyzedShader(*translation);
      auto same = [](const auto& a, const auto& b) {
        return a.size() == b.size() &&
               (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])));
      };
      if (!same(shader->GetTextureBindingsAfterTranslation(), isolated.GetTextureBindingsAfterTranslation()) ||
          !same(shader->GetSamplerBindingsAfterTranslation(), isolated.GetSamplerBindingsAfterTranslation())) {
        ++layout_changes;
        std::printf("Binding layout changes: %016llX modification %016llX\n",
                    (unsigned long long)hash, (unsigned long long)modification);
      }
      uint32_t index = 0;
      for (const auto& binding : shader->GetTextureBindingsAfterTranslation())
        index = std::max(index, binding.bindless_descriptor_index);
      for (const auto& binding : shader->GetSamplerBindingsAfterTranslation())
        index = std::max(index, binding.bindless_descriptor_index);
      max_index = std::max(max_index, index);
      if (index >= 64) {
        ++overflow;
        std::printf("Descriptor overflow: %016llX modification %016llX needs index %u\n",
                    (unsigned long long)hash, (unsigned long long)modification, index);
      }
    };
    for (const auto& pipeline : pipelines) {
      check(pipeline.vs_hash, pipeline.vs_mod);
      if (pipeline.ps_hash) check(pipeline.ps_hash, pipeline.ps_mod);
    }
    std::printf("Production translations: %llu, max descriptor index %u; %zu overflows, %zu failures, %zu binding layout changes.\n",
                (unsigned long long)cache.stats().translations, max_index, overflow, failures, layout_changes);
    if (overflow || failures || layout_changes) return 1;
  }
  if (argc > 3 && shaders.size() != std::strtoul(argv[3], nullptr, 10)) return 1;
  return shaders.empty() ? 1 : 0;
}
