// Code-free pipeline manifest and shader discovery during guest shader
// construction.
#pragma once
#include "pipeline_store.h"
#include <utility>
namespace lost::native {
std::vector<PipelineStore::Pipeline> ManifestPipelines(uint32_t key_size,
                                                       uint32_t key_version);
std::vector<std::pair<uint64_t, PipelineStore::Shader>> TakeCreatedShaders();
void DiscoverDiscShaders(const std::filesystem::path &pack);
void ObserveShaderBlob(const uint8_t *data, uint32_t size);
} // namespace lost::native
