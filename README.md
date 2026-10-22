### Multilingual code playground with code samples and low level understanding of code
Needs setups and benchmarks for (not limited to):
- Profiler
- Debugger
- Assembler
- Process Explorer
- System calls
- Deterministic simulation tester

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
