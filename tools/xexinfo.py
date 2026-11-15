"""Read XEX2 headers (plaintext part only): title id, base/entry, format, static libs, imports."""
import struct, sys

d = open(sys.argv[1], "rb").read()
be32 = lambda o: struct.unpack_from(">I", d, o)[0]
assert d[:4] == b"XEX2", d[:4]
mod_flags, pe_off, _, sec_off, nhdr = struct.unpack_from(">IIIII", d, 4)
print(f"module_flags=0x{mod_flags:X} pe_data_offset=0x{pe_off:X} security_offset=0x{sec_off:X} headers={nhdr}")
image_size = be32(sec_off + 4)
print(f"image_size=0x{image_size:X}")
# security info: load address at +0x110 in XEX2
print(f"load_address(sec)=0x{be32(sec_off + 0x110):X}")
for i in range(nhdr):
    key, val = struct.unpack_from(">II", d, 24 + i * 8)
    size = key & 0xFF
    tag = f"0x{key:08X}"
    if key == 0x00010100:
        print(f"{tag} entry_point=0x{val:X}")
    elif key == 0x00010201:
        print(f"{tag} base_address=0x{val:X}")
    elif key == 0x00018002:
        print(f"{tag} image_base=0x{val:X}")
    elif key == 0x00040006:
        media_id, ver, base_ver, title_id = struct.unpack_from(">IIII", d, val)
        plat, ety, disc, ndisc = d[val + 16: val + 20]
        print(f"{tag} title_id={title_id:08X} media_id={media_id:08X} version=0x{ver:08X} base=0x{base_ver:08X} disc {disc}/{ndisc}")
    elif key == 0x000003FF:
        hsz = be32(val)
        enc, comp = struct.unpack_from(">HH", d, val + 4)
        print(f"{tag} file_format: encryption={enc} ({'none' if enc == 0 else 'normal'}) compression={comp} (0 none,1 basic,2 normal/lzx,3 delta)")
    elif key == 0x000200FF:
        n = be32(val)
        cnt = (n - 4) // 16
        print(f"{tag} static libraries ({cnt}):")
        for j in range(cnt):
            o = val + 4 + j * 16
            name = d[o:o + 8].rstrip(b"\0").decode()
            maj, mnr, bld, qfe = struct.unpack_from(">HHHH", d, o + 8)
            print(f"    {name:<10} {maj}.{mnr}.{bld}.{qfe & 0xFF}")
    elif key == 0x000103FF:
        hsz = be32(val)
        strsz, nlib = struct.unpack_from(">II", d, val + 4)
        names = d[val + 12: val + 12 + strsz].split(b"\0")
        print(f"{tag} import libraries: {[n.decode() for n in names if n]}")
        o = val + 12 + strsz
        for _ in range(nlib):
            lsz = be32(o)
            ver, minver = be32(o + 0x24 - 8 + 8), 0
            nimp = struct.unpack_from(">H", d, o + 0x26)[0]
            print(f"    lib entry size={lsz} records={nimp}")
            o += lsz
    elif key == 0x00040310:
        print(f"{tag} game_ratings")
    elif key == 0x000183FF:
        print(f"{tag} original_pe_name={d[val + 4:val + be32(val)].rstrip(b'\\0').decode(errors='replace')}")
    else:
        print(f"{tag} value/offset=0x{val:X}")
