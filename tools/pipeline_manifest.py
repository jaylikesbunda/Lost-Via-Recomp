"""Export only hashes, lengths and pipeline state from a local native cache.
No shader microcode or driver binaries are written to the manifest.
Usage: python -I tools/pipeline_manifest.py pipelines.bin pipelines.manifest
"""
import argparse
import importlib.util
import struct
from pathlib import Path

def export(source, target, pack=None):
    recipes = {}
    if pack is not None:
        spec = importlib.util.spec_from_file_location('shader_match', Path(__file__).with_name('shader_match.py'))
        matcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matcher)
        result = matcher.match(pack, source)
        for match in result['matches']:
            if match['reconstruction_recipes']:
                # Equivalent source candidates reconstruct to the same full target hash.
                recipes[int(match['cache_hash'], 16)] = match['reconstruction_recipes'][0]
        print(f"Verified reconstruction recipes for {len(recipes)} / {result['cached_shaders']} shaders")
    data = source.read_bytes()
    magic, version, key_size, key_version = struct.unpack_from('<4sIII', data)
    if magic != b'P4PS' or version != 1 or not 0 < key_size <= 256:
        raise ValueError('Unsupported pipeline cache')
    pos, shaders, pipelines = 16, {}, []
    while pos < len(data):
        tag, = struct.unpack_from('<I', data, pos); pos += 4
        if tag == ord('S'):
            kind, hash_value, words = struct.unpack_from('<IQI', data, pos); pos += 16
            if kind > 1 or not 0 < words <= 16384 or pos + words * 4 > len(data):
                raise ValueError('Invalid shader record')
            prefix = 14695981039346656037
            for byte in data[pos:pos + min(words * 4, 16)]:
                prefix = ((prefix ^ byte) * 1099511628211) & 0xffffffffffffffff
            shaders[hash_value] = (kind, words, prefix)
            pos += words * 4
        elif tag == ord('P'):
            end = pos + 32 + key_size
            if end > len(data): raise ValueError('Truncated pipeline record')
            if key_size < 16 or any(data[pos + 32:pos + 48]):
                raise ValueError('Pipeline key contains shader pointers')
            pipelines.append(data[pos:end]); pos = end
        else:
            raise ValueError('Unknown cache record')
    out = bytearray(struct.pack('<4sIIIII', b'LPWM', 2 if pack is not None else 1, key_size, key_version,
                                len(shaders), len(pipelines)))
    for hash_value, (kind, words, prefix) in shaders.items():
        out.extend(struct.pack('<QIIQ', hash_value, kind, words, prefix))
        if pack is not None:
            recipe = recipes.get(hash_value)
            encoded = bytearray()
            if recipe:
                bools = [0] * 8
                for bit, value in recipe.get('bool_values', {}).items():
                    bit = int(bit)
                    if value: bools[bit // 32] |= 1 << (bit % 32)
                layouts = recipe.get('vertex_inputs', [])
                encoded.extend(struct.pack('<III8II', recipe['record'], 24,
                                           int(recipe['specialized']), *bools, len(layouts)))
                for layout in layouts:
                    encoded.extend(struct.pack('<I11i', layout['slot'], *layout['fields']))
            out.extend(struct.pack('<I', len(encoded)))
            out.extend(encoded)
    out.extend(b''.join(pipelines))
    target.write_bytes(out)
    print(f'{len(shaders)} shader hashes, {len(pipelines)} pipeline states; {len(out)} bytes; no shader code')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('target', type=Path)
    parser.add_argument('--pack', type=Path, help='Include disc-source, boolean and vertex-layout recipes')
    args = parser.parse_args()
    export(args.source, args.target, args.pack)
