"""Read a guest heap dump written by --lost_debug_heap_dump (records: u32 addr, u32 size, bytes;
little-endian header, big-endian guest data). Import it from an analysis script:
    from heapdump import Heap; h = Heap("heap.bin", "default.image.bin"); h.find(b"..."); h.refs(addr)"""
import struct,re,bisect
class Heap:
    def __init__(s,path,image=None):
        d=open(path,'rb').read(); s.regs=[]; o=0
        while o<len(d):
            a,n=struct.unpack_from('<II',d,o); o+=8; s.regs.append((a,d[o:o+n])); o+=n
        if image: s.regs.append((0x82000000,open(image,'rb').read()))
        s.regs.sort(); s.starts=[r[0] for r in s.regs]
    def get(s,a,n):
        i=bisect.bisect_right(s.starts,a)-1
        if i<0: return None
        b,d=s.regs[i]
        if a-b+n>len(d): return None
        return d[a-b:a-b+n]
    def u32(s,a):
        x=s.get(a,4); return struct.unpack('>I',x)[0] if x else None
    def find(s,pat):
        for b,d in s.regs:
            for m in re.finditer(re.escape(pat),d): yield b+m.start()
    def refs(s,v):
        p=struct.pack('>I',v)
        for b,d in s.regs:
            i=d.find(p)
            while i>=0:
                if i%4==0: yield b+i
                i=d.find(p,i+1)
    def words(s,a,n): return ' '.join(f'{s.u32(a+4*i) or 0:08X}' for i in range(n))
