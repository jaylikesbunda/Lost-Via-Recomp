"""Exercise the production overflow hooks against a guest-memory/freelist fixture.
Run from a VS developer shell with clang++ on PATH.
"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='lost-thread-records-') as directory:
    tmp = Path(directory)
    header = r"""
#pragma once
#include <cstdint>
#include <cstddef>
struct Reg { uint64_t u64 = 0; uint32_t& u32 = *reinterpret_cast<uint32_t*>(&u64); Reg()=default; Reg(const Reg& o):u64(o.u64){} Reg& operator=(const Reg& o){u64=o.u64;return *this;} };
struct PPCContext { Reg r3,r4; uint64_t lr=0; };
#define REX_EXTERN(name) void name(PPCContext&,uint8_t*)
#define REX_HOOK_RAW(name) void name(PPCContext& ctx,uint8_t* base)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
namespace rex::system {
struct Memory { uint32_t next=0x10000; unsigned allocations=0; uint32_t SystemHeapAlloc(uint32_t){++allocations;auto p=next;next+=64;return p;} };
inline Memory memory;
inline Memory* kernel_memory(){return &memory;}
}
"""
    for name in ['hook.h','logging.h','ppc/context.h','system/kernel_state.h','system/xmemory.h']:
        p=tmp/'rex'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "fixture.h"\n')
    (tmp/'fixture.h').write_text(header)
    source = r"""
#include "fixture.h"
#include <cassert>
#include <vector>
#include <cstring>
uint32_t free_head=0, free_tail=0; unsigned removals=0;
void __imp__sub_82179450(PPCContext& c,uint8_t* b){
 c.r3.u64=free_head;if(free_head){uint32_t n;std::memcpy(&n,b+free_head,4);free_head=n;if(!n)free_tail=0;std::memset(b+c.r3.u32,0,8);}
}
void __imp__sub_82179430(PPCContext& c,uint8_t* b){
 uint32_t p=c.r4.u32;if(free_tail)std::memcpy(b+free_tail,&p,4);else free_head=p;free_tail=p;
}
void __imp__sub_82154E58(PPCContext&,uint8_t*){++removals;}
#include "PRODUCTION"
int main(){
 std::vector<uint8_t> mem(1024*1024);PPCContext c;constexpr uint32_t owner=0x1000;
 c.r3.u64=owner+1664;c.lr=0x82178AB4;sub_82179450(c,mem.data());
 assert(c.r3.u32==0 && rex::system::memory.allocations==0);
 std::vector<uint32_t> records;
 for(int i=0;i<64;++i){c.r3.u64=owner+1664;c.lr=0x82178B24;sub_82179450(c,mem.data());assert(c.r3.u32);records.push_back(c.r3.u32);}
 assert(rex::system::memory.allocations==64);
 for(auto p:records){c.r3.u64=owner;c.r4.u64=p;sub_82154E58(c,mem.data());}
 assert(removals==64);
 for(auto p:records){c.r3.u64=owner+1664;c.lr=0x82178AB4;sub_82179450(c,mem.data());assert(c.r3.u32==p);}
 assert(rex::system::memory.allocations==64 && free_head==0);
 // An unrelated owner must not recycle one of these records into its pool.
 c.r3.u64=owner+64;c.r4.u64=records[0];sub_82154E58(c,mem.data());assert(free_head==0);
}
""".replace('PRODUCTION',(root/'port/src/thread_records.cpp').as_posix())
    (tmp/'test.cpp').write_text(source)
    subprocess.run(['clang++','-std=c++20','-I',str(tmp),str(tmp/'test.cpp'),'-o',str(tmp/'test.exe')],check=True)
    subprocess.run([str(tmp/'test.exe')],check=True)
print('thread records: exhausted retry grows; 64 records recycle without new allocations; ownership isolated')
