### Multilingual code playground with code samples and low level understanding of code
Needs setups and benchmarks for  executables running on linux to understand what's exactly going on with the process, how does it specifically behaves with the system from high level visual/functional changes upto low level assembly, CPU, memory, dma, cache and even hardware interactioons (not limited to) going on for the process on all fronts. Using pure linux commands/open source softwares:
- Profiler
- Resource usage explorer ( cpu, io, memory etc )
- File handles/open connenctions threads
- Debugger
- disassembler
- Process Explorer
- Thread dump, core dump, thread explorer
- System calls bifurcater
- Deterministic simulation tester to test the executable(process) under concurrent load
- strace
- Anything else
Later, we extend it to programs in JS, Python and Java. For now, we can keep them as it is for langauges which generate an executable artifact like rust, zig, c, go etc
( Can refer to this for an intro: https://www.youtube.com/watch?v=qFEuYUfauNY )

## Commands:

### Build and run:
#### C
```bash
# build
cc -O3 langs/src-c/main.c -o langs/src-c/main_c

# file info
file langs/src-c/main_c

# ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked, interpreter /lib64/ld-linux-x86-64.so.2, BuildID[sha1]=7f81b204ea50a295314f3693c0e19bb357c7dc7d, for GNU/Linux 3.2.0, not stripped

# objdump ( assembly )
objdump -d langs/src-c/main_c

```
