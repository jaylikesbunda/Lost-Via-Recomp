"""CPU regressions for production shader and SRV memo identities; no game/GPU.

Compile the actual narrow methods/lookup block from the renderer, with fake guest
memory and a descriptor allocation counter. Run from a VS x64 developer shell.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    first = source.index('{', start)
    depth = 1
    end = first + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


state = (ROOT / 'port/native/d3d_state.cpp').read_text()
header = (ROOT / 'port/native/d3d_state.h').read_text()
draw = (ROOT / 'port/native/draw.cpp').read_text()
shader_memo = block(header, 'struct ShaderBindingMemo')
shader_method = block(state, 'uint64_t GuestStateTracker::ShaderBinding(')
view_memo = block(draw, 'struct SrvMemo')
view_lookup = draw[draw.index('      const uint32_t mapping = b.srv.Shader4ComponentMapping;'):]
view_lookup = view_lookup[:view_lookup.index('      signs[tb.fetch_constant]')]
source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <bit>
#include <d3d12.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
bool hash_check = false;
#define REXCVAR_GET(x) hash_check
#define REXLOG_WARN(...) ((void)0)
std::array<uint32_t, 1024> guest{};
const void* Physical(uint32_t address) { return guest.data() + address / 4; }
class GuestStateTracker {
public:
SHADER_MEMO;
ShaderBindingMemo binding_memo_[256]{};
uint64_t ShaderBinding(uint32_t, uint32_t);
};
SHADER_METHOD
VIEW_MEMO;
constexpr unsigned kSrvMemoBits = 12;
std::array<SrvMemo, 1 << kSrvMemoBits> srv_memo{};
struct TextureBinding {
  ID3D12Resource* resource;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  DXGI_FORMAT srv_signed_format = DXGI_FORMAT_UNKNOWN;
};
struct Binding { uint8_t is_signed = 0, dimension = 0; };
namespace profile { constexpr int kSrvMemoHit = 0; void Count(int) {} }
uint32_t descriptor_calls = 0;
uint32_t TextureDescriptor(const TextureBinding&, const Binding&) { return ++descriptor_calls; }
uint32_t Lookup(const TextureBinding& b, const Binding& tb) {
  uint32_t index;
VIEW_LOOKUP
  return index;
}
int main() {
  GuestStateTracker tracker;
  for (uint32_t i = 0; i < guest.size(); ++i) guest[i] = i * 17;
  const auto original = tracker.ShaderBinding(16, 128);
  assert(tracker.ShaderBinding(16, 128) == original);
  guest[4 + 64] ^= 0x12345678; // preserve address, size and both sampled ends
  const auto replaced = tracker.ShaderBinding(16, 128);
  assert(replaced != original); // must work with diagnostic logging disabled
  guest[4 + 64] ^= 0x12345678;
  assert(tracker.ShaderBinding(16, 128) == original);

  TextureBinding texture{reinterpret_cast<ID3D12Resource*>(uintptr_t(1))};
  texture.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  texture.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  texture.srv.Texture2DArray.MipLevels = 1;
  texture.srv.Texture2DArray.ArraySize = 1;
  Binding binding;
  auto previous = Lookup(texture, binding);
  assert(Lookup(texture, binding) == previous && descriptor_calls == 1);
  auto must_miss = [&] {
    const auto next = Lookup(texture, binding);
    assert(next != previous);
    assert(Lookup(texture, binding) == next);
    previous = next;
  };
  texture.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  must_miss(); // same resource and swizzle, different compatible view
  texture.srv.Texture2DArray.MostDetailedMip = 1;
  must_miss();
  texture.srv.Texture2DArray.FirstArraySlice = 1;
  must_miss();
  binding.is_signed = 1;
  texture.srv_signed_format = DXGI_FORMAT_R8G8B8A8_SNORM;
  must_miss();
  texture.srv_signed_format = DXGI_FORMAT_UNKNOWN;
  must_miss();
  std::puts("Renderer identities: middle-only shader replacements and distinct SRV formats/ranges cannot reuse stale cache entries.");
}
'''
for marker, value in [('SHADER_MEMO', shader_memo), ('SHADER_METHOD', shader_method),
                      ('VIEW_MEMO', view_memo), ('VIEW_LOOKUP', view_lookup)]:
    source = source.replace(marker, value)

with tempfile.TemporaryDirectory(prefix='lost-renderer-identity-') as directory:
    directory = pathlib.Path(directory)
    cpp = directory / 'test.cpp'
    exe = directory / 'test.exe'
    cpp.write_text(source)
    subprocess.run([str(ROOT / '.tools/llvm/bin/clang++.exe'), '-std=c++20', '-O2',
                    '-DNOMINMAX',
                    '-I', str(ROOT / '.tools/rexglue-sdk/win-amd64/include'),
                    str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
