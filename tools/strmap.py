"""Map every string in an image range to the functions that build its address (lis+addi).
    python -I tools/strmap.py <image.bin> <hex lo> <hex hi>
Used to find Magma (UI library) functions by their assert/source-file strings."""
import re, struct, sys
img=open(sys.argv[1],'rb').read();B=0x82000000;LO,HI=0x82150000,0x8288C86C
lo,hi=int(sys.argv[2],16),int(sys.argv[3],16)
strs={}
for m in re.finditer(rb'[\x20-\x7e]{4,120}',img[lo-B:hi-B]):
    strs[lo+m.start()]=m.group().decode()
def w(a): return struct.unpack_from('>I',img,a-B)[0]
def fs(a):
    while a>LO and w(a-4)!=0x4E800020: a-=4
    return a
out={}
his={}
for a in range(LO,HI,4):
    x=w(a);op=x>>26
    if op==15 and (x>>16&31)==0: his[x>>21&31]=(x&0xFFFF)<<16;continue
    if op==14:
        ra=x>>16&31
        if ra in his:
            imm=x&0xFFFF; imm-=0x10000 if imm&0x8000 else 0
            v=(his[ra]+imm)&0xFFFFFFFF
            if v in strs: out.setdefault(v,set()).add(fs(a))
    if x==0x4E800020: his={}
for v in sorted(out):
    print(f'{v:08X} {strs[v][:70]:70s} ' + ' '.join(f'{f:08X}' for f in sorted(out[v])[:8]))
