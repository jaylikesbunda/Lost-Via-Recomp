"""Match cached native shaders to disc programs without relying on their full hashes.
Reads local game data. Reports metadata only; never exports shader code.
"""
import argparse
import collections
import hashlib
import json
import struct
from pathlib import Path

EXEC = {1, 2, 3, 4, 5, 6, 13, 14}

def control_flow(words):
    bound = len(words) // 3
    instructions = []
    i = 0
    while i < bound:
        a, b, c = words[i * 3:i * 3 + 3]
        pair = [(a, b & 65535), ((b >> 16 | c << 16) & 0xffffffff, c >> 16)]
        instructions.extend(pair)
        for lo, hi in pair:
            if hi >> 12 in EXEC:
                bound = min(bound, lo & 4095)
        i += 1
    return instructions[:bound * 2], bound

def unpack(data):
    return struct.unpack('>' + 'I' * (len(data) // 4), data)

def fingerprint(kind, data, mask_fetch=False):
    words = unpack(data)
    cf, bound = control_flow(words)
    body = list(words[bound * 3:])
    if mask_fetch and kind == 0:
        for i in range(0, len(body) - 2, 3):
            if body[i] & 31 == 0:
                body[i:i + 3] = [0, 0, 0]
    payload = struct.pack('>' + 'I' * len(body), *body)
    return kind, len(data), bound, hashlib.sha256(payload).hexdigest()

def pack_programs(path):
    data = path.read_bytes()
    magic, version, _, _, count = struct.unpack_from('>5I', data)
    if magic != 0x53444658 or version != 4:
        raise ValueError('Expected SDFX version 4')
    pos = 24
    for index in range(count):
        key = data[pos:pos + 24].hex()
        size, crc = struct.unpack_from('>II', data, pos + 24)
        blob = pos + 32
        flags, header, code = struct.unpack_from('>III', data, blob)
        if header + code != size or blob + size > len(data):
            raise ValueError('Invalid blob sizes')
        for field in (24, 32):
            offset, = struct.unpack_from('>I', data, blob + field)
            if not offset:
                continue
            begin, length = struct.unpack_from('>II', data, blob + offset)
            if begin + length > code or length % 12:
                raise ValueError('Invalid shader program bounds')
            kind = 0 if flags & 1 else 1
            yield {'key': key, 'record': index, 'program': field,
                   'kind': kind, 'data': data[blob + header + begin:blob + header + begin + length]}
        pos = blob + size
    if pos != len(data):
        raise ValueError('Unparsed SDF tail')

def cached_shaders(path):
    data = path.read_bytes()
    magic, version, key_size, _ = struct.unpack_from('<4sIII', data)
    if magic != b'P4PS' or version != 1 or not 0 < key_size <= 256:
        raise ValueError('Unsupported native cache')
    pos = 16
    while pos < len(data):
        tag, = struct.unpack_from('<I', data, pos)
        pos += 4
        if tag == 83:
            kind, shader_hash, count = struct.unpack_from('<IQI', data, pos)
            pos += 16
            end = pos + count * 4
            if end > len(data): raise ValueError('Truncated shader')
            yield {'hash': f'{shader_hash:016X}', 'kind': kind, 'data': data[pos:end]}
            pos = end
        elif tag == 80:
            pos += 32 + key_size
        else:
            raise ValueError('Unknown native cache record')
    if pos != len(data): raise ValueError('Truncated cache')

def vertex_fetch_slots(words):
    cf, _ = control_flow(words)
    slots = set()
    for lo, hi in cf:
        if hi >> 12 not in EXEC: continue
        address, count, sequence = lo & 4095, (lo >> 12) & 7, (lo >> 16) & 4095
        for i in range(count):
            slot = address + i
            if sequence >> (i * 2) & 1 and slot * 3 < len(words) and words[slot * 3] & 31 == 0:
                slots.add(slot)
    return slots

def tested_bools(words):
    cf, _ = control_flow(words)
    return sorted({(hi >> 2) & 255 for lo, hi in cf
                   if hi >> 12 in (3, 4, 13, 14) or
                   hi >> 12 in (9, 11) and not lo & 0x6000})

def specialize(words, values):
    """Replay port/native/shader_specialize.cpp, including reachability cleanup."""
    cf, bound = control_flow(words)
    cf = [list(x) for x in cf]
    def empty(c):
        c[0] &= 4095
        c[1] = (c[1] & 0x803) | 0x1000
    for c in cf:
        lo, hi = c
        op = hi >> 12
        if op in (3, 4, 13, 14):
            if values.get((hi >> 2) & 255, 0) == ((hi >> 10) & 1):
                c[1] = (hi & 0x803) | ((2 if op in (4, 14) else 1) << 12)
            else:
                empty(c)
        elif op in (9, 11) and not lo & 0x6000:
            if values.get((hi >> 2) & 255, 0) == ((hi >> 10) & 1):
                c[0] |= 0x2000
                c[1] &= ~0x7fc
            else:
                c[:] = [0, 0]
    reached, pending = set(), [0]
    while pending:
        i = pending.pop()
        if i in reached or i >= len(cf): continue
        reached.add(i)
        lo, hi = cf[i]
        op = hi >> 12
        if op == 2: continue
        if op == 11:
            pending.append(lo & 8191)
            if not lo & 0x2000: pending.append(i + 1)
        else:
            if op in (7, 8, 9): pending.append(lo & 8191)
            pending.append(i + 1)
    for i, c in enumerate(cf):
        if i in reached: continue
        if c[1] >> 12 in EXEC: empty(c)
        else: c[:] = [0, 0]
    for i, c in enumerate(cf):
        if i not in reached or c[1] >> 12 != 11 or not c[0] & 0x2000: continue
        target = c[0] & 8191
        if i < target <= len(cf) and not any(j in reached for j in range(i + 1, target)):
            c[:] = [0, 0]
    result = list(words)
    for i in range(bound):
        a, b = cf[i * 2:i * 2 + 2]
        result[i * 3:i * 3 + 3] = [a[0], (a[1] & 65535) | ((b[0] << 16) & 0xffffffff),
                                    (b[0] >> 16) | (b[1] << 16)]
    return result

# Semantic vertex input properties, not encoded shader instruction words.
FETCH_FIELDS = [(0, 12, 6), (0, 20, 5), (0, 25, 2), (0, 27, 3),
                (1, 0, 12), (1, 12, 1), (1, 13, 1), (1, 16, 6),
                (1, 30, 1), (2, 0, 8), (2, 8, 23)]

def fetch_layout(words, slot):
    fields = [(words[slot * 3 + word] >> shift) & ((1 << bits) - 1)
              for word, shift, bits in FETCH_FIELDS]
    if fields[-1] & (1 << 22): fields[-1] -= 1 << 23
    return fields

def apply_fetch_layout(words, slot, fields):
    for (word, shift, bits), value in zip(FETCH_FIELDS, fields):
        mask = ((1 << bits) - 1) << shift
        i = slot * 3 + word
        words[i] = (words[i] & ~mask) | ((value << shift) & mask)

def reconstruct(source, target, max_bits=12):
    original, wanted = unpack(source['data']), unpack(target['data'])
    valid, patches = allowed_body_patch(source, target)
    if not valid: return None
    base = list(original)
    layouts = [{'slot': slot, 'fields': fetch_layout(wanted, slot)} for slot in patches]
    for layout in layouts: apply_fetch_layout(base, layout['slot'], layout['fields'])
    # Reject changes that these actual declaration fields cannot explain.
    if any(base[i] != wanted[i] for i in range(control_flow(original)[1] * 3, len(base))):
        return None
    bits = tested_bools(original)
    if len(bits) > max_bits: return None
    # First permit unchanged control flow: these may be unspecialized shader uses.
    if base == list(wanted):
        return {'specialized': False, 'vertex_fetch_slots': patches, 'vertex_inputs': layouts}
    for assignment in range(1 << len(bits)):
        values = {bit: (assignment >> i) & 1 for i, bit in enumerate(bits)}
        if specialize(base, values) == list(wanted):
            return {'specialized': True, 'bool_values': values, 'vertex_fetch_slots': patches, 'vertex_inputs': layouts}
    return None

def allowed_body_patch(source, target):
    a, b = unpack(source['data']), unpack(target['data'])
    _, bound = control_flow(a)
    slots = vertex_fetch_slots(a) if source['kind'] == 0 else set()
    changes = [i for i in range(bound * 3, len(a)) if a[i] != b[i]]
    return all(i // 3 in slots for i in changes), sorted({i // 3 for i in changes})

def match(pack, cache):
    programs = list(pack_programs(pack))
    exact, body, fetch = collections.defaultdict(list), collections.defaultdict(list), collections.defaultdict(list)
    for i, shader in enumerate(programs):
        exact[(shader['kind'], hashlib.sha256(shader['data']).digest())].append(i)
        body[fingerprint(shader['kind'], shader['data'])].append(i)
        fetch[fingerprint(shader['kind'], shader['data'], True)].append(i)
    counts = collections.Counter()
    matches = []
    for shader in cached_shaders(cache):
        candidates = exact.get((shader['kind'], hashlib.sha256(shader['data']).digest()), [])
        category = 'exact'
        if not candidates:
            candidates = body.get(fingerprint(shader['kind'], shader['data']), [])
            category = 'control_flow_only'
        if not candidates:
            candidates = [i for i in fetch.get(fingerprint(shader['kind'], shader['data'], True), [])
                          if allowed_body_patch(programs[i], shader)[0]]
            category = 'control_flow_and_vertex_fetch'
        if not candidates: category = 'unmatched'
        counts[category] += 1
        verified = []
        for i in candidates:
            recipe = reconstruct(programs[i], shader)
            if recipe is not None:
                verified.append({'record': i, 'key': programs[i]['key'], **recipe})
        keys = sorted({programs[i]['key'] for i in candidates})
        matches.append({'cache_hash': shader['hash'], 'stage': shader['kind'], 'category': category,
                        'candidate_keys': keys, 'candidate_records': candidates,
                        'reconstruction_recipes': verified})
    return {'pack_programs': len(programs), 'cached_shaders': len(matches),
            'counts': dict(counts),
            'reconstructed': sum(bool(x['reconstruction_recipes']) for x in matches),
            'matches': matches}

def inventory(pack):
    """Enumerate every boolean combination from the shipped programs, not a trace.
    These are candidate variants: the runtime must retain its same-output eligibility
    check and apply the mesh's actual vertex declaration before pipeline creation.
    """
    variants = {}
    distribution = collections.Counter()
    assignments = 0
    for source in pack_programs(pack):
        words = unpack(source['data'])
        bits = tested_bools(words)
        distribution[len(bits)] += 1
        if len(bits) > 16:
            raise ValueError('Boolean space exceeds the offline enumeration limit')
        candidates = [(False, {}, list(words))]
        for assignment in range(1 << len(bits)):
            values = {bit: (assignment >> i) & 1 for i, bit in enumerate(bits)}
            candidates.append((True, values, specialize(words, values)))
            assignments += 1
        for specialized, values, result in candidates:
            binary = struct.pack('>' + 'I' * len(result), *result)
            digest = hashlib.sha256(binary).hexdigest()
            identifier = f"{source['kind']}:{digest}"
            if identifier in variants: continue
            variants[identifier] = {'sha256': digest, 'stage': source['kind'],
                                    'source_record': source['record'], 'source_key': source['key'],
                                    'specialized': specialized, 'bool_values': values}
    return {'scope': 'all disc programs; candidate variants before runtime output/declaration checks',
            'pack_programs': sum(distribution.values()),
            'tested_boolean_distribution': dict(sorted(distribution.items())),
            'boolean_assignments': assignments, 'unique_candidate_variants': len(variants),
            'variants': list(variants.values())}

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pack', type=Path)
    p.add_argument('cache', type=Path, nargs='?')
    p.add_argument('--inventory', action='store_true', help='Enumerate all disc boolean variants')
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    if not args.inventory and args.cache is None: p.error('cache is required unless --inventory is set')
    result = inventory(args.pack) if args.inventory else match(args.pack, args.cache)
    args.out.write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k not in ('matches', 'variants')}, indent=2))
