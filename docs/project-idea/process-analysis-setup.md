# Process Analysis and Observability Setup

This document is the hands-on companion to [process-observability-platform.md](process-observability-platform.md). It covers the Linux mechanisms, the toolchain, the benchmark workflow, container adaptation, and the validation needed to understand a running process from high-level behavior down to hardware events.

The target is an executable artifact (C, Rust, Zig, Go) and, later, managed runtimes (Java, Python, JavaScript). A container is treated as what it really is: a set of Linux processes isolated by namespaces, constrained by cgroups, and rooted in an overlay filesystem.

Conventions used below:

- `$HOST_PID` is the PID as seen by the host kernel. This is the only PID that profiling tools on the host understand.
- `$TARGET` is the benchmark binary or process name.
- Commands that attach to another process generally need root or the capabilities described in Section 1.5.
- Prefer a disposable benchmark container or an authorized test process. Profiling changes timing, and traces can contain secrets.

---

## 0. Coverage Map

| Requirement | Primary material |
|---|---|
| Profiler (CPU, cache, branches, memory, hardware) | Sections 2C, 2J, 4.5 |
| Resource usage explorer (CPU, memory, I/O, scheduling) | Sections 2A, 4.3, 7 |
| File handles, open connections, threads | Sections 2A, 2E, 2F, 2G |
| Process explorer | Section 2A |
| Debugger, thread dumps, core dumps, thread explorer | Sections 2G, 2H |
| Disassembler and reverse engineering | Sections 2H, 4.7 |
| System-call bifurcation and tracing | Sections 2B, 4.4 |
| Deterministic simulation and concurrency | Sections 2G, 4.6 |
| DMA, cache, interrupts, NUMA, power, hardware interactions | Sections 2J, 4.5 |
| Containers, namespaces, cgroups, overlayfs, runtimes | Sections 1.4, 5 |
| JS, Python, Java, Go runtime extensions | Section 6 |
| Validation, overhead, and data quality | Sections 1.3, 7 |

The architecture and productization of these lenses are in the companion platform blueprint.

---

## 1. First Principles

### 1.1 The Five Kernel Interfaces

Every observability tool is a consumer of one or more of these interfaces. Understanding the interface explains the tool's power, overhead, and failure modes.

| Interface | What it exposes | Typical tools | Cost | Privilege |
|---|---|---|---|---|
| `/proc` and `/sys` | Kernel-maintained state: task status, CPU time, memory maps, FDs, cgroups, sockets, devices | `ps`, `top`, `htop`, `pidstat`, `lsof`, `ss`, `pmap`, `smem` | Low, but polling is not free | Usually readable; some files root-only |
| `ptrace` | Stops/controls another process, reads registers and memory, intercepts syscalls/signals | `strace`, `gdb`, `lldb`, `rr` (record), `gcore` | High: frequent context switches and cache pollution | `CAP_SYS_PTRACE` or a ptrace relationship |
| Hardware performance counters (PMCs) | Cycles, instructions, cache events, branch events, stalls, uncore/memory-controller counters | `perf stat`, `perf record`, `perf mem`, `perf c2c` | Low to moderate; multiplexing and sampling have costs | `perf_event_open` policy or `CAP_PERFMON` |
| eBPF | Sandboxed programs attached to kernel/user probes, tracepoints, and network hooks | `bpftrace`, BCC tools, `bpftool`, Cilium/Tetragon | Low to moderate; ring-buffer pressure and probes have costs | `CAP_BPF` + `CAP_PERFMON` (or root) on modern kernels |
| Ftrace/tracefs | Kernel function tracing, tracepoints, kprobes, uprobes, function graph | `trace-cmd`, `perf trace`, `perf probe`, direct `tracefs` writes | Variable; function tracing can be very heavy | Root or tracefs permissions |

Key consequences:

1. `/proc` is a snapshot interface. It is cheap per read but can miss events between reads. It is ideal for state (thread states, memory maps, FDs) and weak for transient events.
2. `ptrace` is a control interface. It can answer "what exactly happened?" but it can slow the target by orders of magnitude.
3. PMCs answer "where did the CPU spend cycles and why?" without source changes, but they are sampled or counted and depend on CPU support.
4. eBPF is the best fit for low-overhead, event-driven questions at both kernel and user level, but it requires a modern kernel, often BTF for portable probes, and usually root or BPF-related capabilities.
5. Ftrace is the kernel's own tracing facility and is the plumbing behind many `perf` and eBPF features. It is excellent when you need exact tracepoint/kprobe events and do not need a custom aggregator.

### 1.2 Signals and Artifacts

The toolchain produces five kinds of evidence. Do not confuse them:

- **Metrics:** numeric time series (CPU time, RSS, context switches, cache misses, bytes read). Good for trends and comparisons.
- **Events:** timestamped discrete records (syscall entry/exit, scheduler switch, packet, page fault, lock acquisition). Good for causality and timelines.
- **Logs:** textual records from the kernel, runtime, or application. Good for errors and state transitions.
- **Traces:** correlated spans or call paths (syscall with stack, distributed trace, function graph). Good for "what led to this?"
- **Profiles:** sampled stacks aggregated by symbol or address (CPU flame graph, off-CPU flame graph, allocation profile). Good for finding hotspots and bottlenecks.

The platform must preserve which kind of evidence each data point is. A syscall count and a perf sample are not interchangeable.

### 1.3 The Observer Effect and Data Quality

Measuring a system changes it. The effect ranges from negligible to catastrophic:

- Reading `/proc`, cgroup files, and `ss` is close to passive but still allocates, copies, and schedules work.
- `perf stat` programs hardware counters and may multiplex events; `perf record` copies samples through a ring buffer.
- eBPF programs run in kernel context; badly written probes can add latency to hot paths.
- `strace` stops the tracee on every syscall; slowdowns of 10x-100x are normal.
- `gdb` attach stops the process; `rr` record adds substantial overhead.
- `nsenter`/`docker exec` creates a process and perturbs scheduling, page tables, and caches.

Data-quality checks to apply everywhere:

- **Lost samples/events:** perf reports lost events; BPF ring buffers can drop records. Expose a dropped-event counter rather than silently returning a partial result.
- **Sampling bias:** CPU samples over-represent running code, not blocked code. Use off-CPU analysis and thread-state inspection for blocking questions.
- **Clock domains:** `CLOCK_MONOTONIC` is not wall-clock time; container time namespaces can further offset clocks. Convert deliberately and record the source clock.
- **Counter semantics:** cumulative vs per-interval, 32-bit wrap, scaled/multiplexed perf counts, and cgroup counter resets after container restart.
- **Target lifetime:** PIDs are reused. Read the process start time from `/proc/PID/stat` (field 22) and verify it before and after attaching. `pidfd_open`/`pidfd_send_signal` avoid some PID-reuse races.
- **Locale:** set `LC_ALL=C` for parsers. Tool output, decimal separators, and column layouts can change with locale and version.
- **Version drift:** `strace`, `perf`, `pidstat`, and `lsof` output formats vary across distributions. Prefer machine-readable modes where available and keep golden fixtures per version.

### 1.4 What a Container Really Is

A container is not a virtual machine. It is a group of ordinary Linux processes that have:

1. **Namespaces:** PID, mount, network, IPC, UTS, user, cgroup, and time namespaces virtualize what the processes can see. `/proc/$HOST_PID/status` shows both the host PID and the in-namespace PID in `NSpid`. The container's init process normally appears as PID 1 inside its PID namespace.
2. **Cgroups:** CPU, memory, I/O, PIDs, and other resources are accounted and limited per cgroup. On cgroup v2 the container's cgroup is usually under `/sys/fs/cgroup/system.slice/docker-<id>.scope` or a similar runtime-specific path. Read the path from `/proc/$HOST_PID/cgroup`.
3. **Overlay filesystem:** the root filesystem is a union of read-only image layers and a writable layer. The host path can be found through the runtime (for Docker: `.GraphDriver.Data.MergedDir`), or safely through `/proc/$HOST_PID/root` when permissions allow.
4. **Runtime metadata and shims:** Docker/containerd/Podman processes, `runc`/`crun` shims, and the container init process are all part of the picture. `docker top <name>` shows host PIDs; the runtime API is the authoritative mapping.

Critical observability consequences:

- A container may contain **many processes and threads**, not just the PID returned by `docker inspect .State.Pid`. Resolve the full cgroup scope instead of profiling only the init process.
- Profiling the host PID works even when the container image has no tools (distroless images). Use host tools first.
- `docker exec` and `nsenter` do not remove the observer effect; they reduce namespace/tool-environment mismatches. `nsenter` is needed when the tool must see the container's mount/network/user view.
- A container that is stopped has host PID 0 or no live process. Re-resolve the target after restarts; do not cache a stale PID.

### 1.5 Permissions and Kernel Prerequisites

Check the actual policy of the machine before choosing tools:

```bash
cat /proc/sys/kernel/perf_event_paranoid
cat /proc/sys/kernel/yama/ptrace_scope
id
cat /proc/self/status | grep -E 'CapEff|Seccomp|NSpid'
mount | grep -E 'debugfs|tracefs'
ls /sys/kernel/btf/vmlinux 2>/dev/null
```

Rules of thumb:

- `perf_event_paranoid` is commonly `2`, `3`, or `4` on hardened distributions. Values above `1` block unprivileged CPU event access; `4` blocks unprivileged perf entirely. Use root or grant `CAP_PERFMON`/`CAP_SYS_PTRACE` rather than permanently lowering the sysctl on a shared host.
- `ptrace_scope=1` allows tracing only descendants; attaching to an unrelated process needs root or `CAP_SYS_PTRACE`.
- `bpftrace` and most BCC tools require root or `CAP_BPF` + `CAP_PERFMON` + `CAP_SYS_ADMIN` (depending on kernel and probes). Kernel BTF (`/sys/kernel/btf/vmlinux`) is required for portable CO-RE programs.
- `debugfs` and `tracefs` may be mounted but permission-restricted. On some kernels they live under `/sys/kernel/debug/tracing`; on newer systems they may be at `/sys/kernel/tracing`.
- Secure Boot/kernel lockdown can block unsigned eBPF, kprobes, and some tracing.
- Virtual machines and some cloud instances do not expose a PMU. Hardware events may be unavailable even as root; fall back to software events (`task-clock`, `context-switches`, `page-faults`, `cpu-migrations`).
- `rr` requires `perf_event_paranoid <= 1` for record mode; `rr record -n` is a slower fallback that avoids PMU use. It also depends on CPU feature support and kernel configuration.

Never loosen these controls globally without understanding the security impact. Prefer per-tool capabilities, a dedicated benchmark VM, or a disposable container host.

---

## 2. Toolchain by Lens

### A. Process Explorer and Resource Usage

This is the process explorer lens. It covers process/thread state, file descriptors (FDs), open connections (sockets), and high-level CPU, memory, I/O, and scheduling resource usage.

| Tool | Purpose | Notes |
|---|---|---|
| `/proc/PID/*` | Authoritative process state | `stat`, `status`, `schedstat`, `io`, `smaps_rollup`, `limits`, `cgroup`, `task/`, `wchan`, `stack`, `syscall` |
| `ps`, `pstree` | Snapshot of processes/threads | `ps -eLf`, `ps -T -p PID`, `ps -o pid,tid,psr,pcpu,stat,wchan:30,comm -L -p PID` |
| `top`, `htop`, `atop`, `btop`, `glances` | Interactive/system overview | `top -H -p PID` for threads; `atop` records history |
| `pidstat` | Per-process interval statistics | CPU (`-u`), memory (`-r`), I/O (`-d`), context switches (`-w`), threads (`-t`), stack (`-s`) |
| `vmstat`, `mpstat`, `iostat`, `sar`, `free`, `uptime` | System-wide resource baselines | `vmstat 1`, `mpstat -P ALL 1`, `iostat -x 1`, `sar -n DEV 1`; use as context for per-process data |
| `smem`, `pmap` | Memory accounting | PSS/USS are more honest than RSS when pages are shared |
| `prlimit`, `ulimit` | Resource limits | `prlimit --pid PID`; cgroup limits still apply on top |
| `taskset`, `chrt`, `nice`, `ionice` | Affinity, scheduling, I/O priority | Inspect with `taskset -pc`, `chrt -p`, `ionice -p`; changing them perturbs the measurement |
| `dmesg`, `journalctl` | Kernel/system logs | OOM kills, segfaults, driver errors, and cgroup events |
| `systemd-cgtop`, `docker stats` | Cgroup/container resource view | Compare with raw cgroup files; do not treat them as ground truth without understanding accounting |

Useful `/proc` reads:

```bash
cat /proc/$HOST_PID/status
cat /proc/$HOST_PID/stat
cat /proc/$HOST_PID/schedstat
cat /proc/$HOST_PID/io
cat /proc/$HOST_PID/smaps_rollup
cat /proc/$HOST_PID/limits
cat /proc/$HOST_PID/cgroup
cat /proc/$HOST_PID/wchan
cat /proc/$HOST_PID/stack
cat /proc/$HOST_PID/syscall
grep -E 'SigBlk|SigIgn|SigCgt' /proc/$HOST_PID/status
ls /proc/$HOST_PID/fd
ls /proc/$HOST_PID/task
```

Limits, affinity, and logs:

```bash
prlimit --pid "$HOST_PID"
taskset -pc "$HOST_PID"
chrt -p "$HOST_PID"
ionice -p "$HOST_PID"
sudo dmesg -T | tail -50
journalctl -k --since "10 min ago"
```

`pidstat` examples:

```bash
pidstat -p "$HOST_PID" 1
pidstat -t -p "$HOST_PID" 1
pidstat -d -r -w -p "$HOST_PID" 1
```

For a container-wide view, enumerate the cgroup's processes and pass them explicitly, or use a cgroup-aware engine:

```bash
CGROUP_PATH=$(awk -F: '$1=="0" {print $3}' /proc/$HOST_PID/cgroup)
cat "/sys/fs/cgroup${CGROUP_PATH}/cgroup.procs"
sudo perf stat -a -e task-clock -G "$CGROUP_PATH" -- sleep 5
```

`-G` requires per-CPU mode (`-a`) and a cgroup name/path form accepted by the installed perf version; check `perf stat -h`. `--for-each-cgroup` gives a per-cgroup breakdown. `pidstat -G` filters by command name, not by cgroup. Do not use it as a cgroup filter.

The `awk -F:` extraction above assumes cgroup v2 (a single `0::<path>` line). On cgroup v1, `/proc/PID/cgroup` has separate controller lines such as `cpu,cpuacct`, `memory`, and `blkio`; resolve each controller's mount point from `/proc/PID/mountinfo` instead of concatenating a single path.

On a container host, `lsof` may print warnings about overlay or `nsfs` filesystems. That is expected; verify the target PID's FDs directly instead of assuming the entire output is complete.

### B. System-Call and Library-Call Tracing

| Tool | Mechanism | Best for | Caveats |
|---|---|---|---|
| `strace` | `ptrace` | Exact syscall arguments, errors, timing, signal delivery | Very high overhead; stops the tracee per syscall |
| `perf trace` | perf tracepoints/ring buffer | Lower-overhead syscall timelines and summaries | Less argument decoding than `strace`; permission-sensitive |
| `ltrace` | `ptrace`/dynamic linker | Library calls such as `malloc`, `pthread_*` | High overhead; does not see static/inlined calls |
| `bpftrace` / BCC | eBPF | Aggregated syscall counts, latency, argument filtering, errors | Requires root/CAP_BPF; probes vary by kernel |
| `syscount`, `execsnoop`, `opensnoop` | BCC/eBPF | Short-lived process and file syscall discovery | BCC package and kernel headers/BTF needed |

Examples:

```bash
# Attach from the host; no namespace entry is required for syscall tracing.
sudo strace -c -f -p "$HOST_PID"
sudo strace -f -ttt -T -y -p "$HOST_PID"
sudo strace -f -e trace=%file,%network,%process -p "$HOST_PID"
sudo strace -f -k -p "$HOST_PID"          # syscall + user stack traces

# Wall-clock accounting vs syscall time.
sudo strace -w -c -f -p "$HOST_PID"

# Lower-overhead alternatives.
sudo perf trace -p "$HOST_PID" --duration 5 -s
sudo bpftrace -e 'tracepoint:syscalls:sys_enter_openat { @[comm] = count(); }'
sudo /usr/share/bcc/tools/syscount -p "$HOST_PID" 5

# Signals and process lifecycle (short-lived children are easy to miss with attach-only tools).
sudo strace -f -e trace=signal -p "$HOST_PID"
sudo perf trace -e 'signal:*' -p "$HOST_PID" --duration 5
sudo perf trace -e 'sched:sched_process_fork,sched:sched_process_exec,sched:sched_process_exit' -p "$HOST_PID" --duration 5
sudo /usr/share/bcc/tools/execsnoop
```

Important interpretation notes:

- `strace -c` reports time spent inside syscalls, not user-space CPU time. A high `futex` percentage means threads spent time waiting in `futex`, which is consistent with lock contention but can also include legitimate idle waits.
- `-c` hides ordering and arguments. Use a normal trace plus `-ttt -T` when causality matters.
- `-w` (`--summary-wall-clock`) is usually the right view for "where did wall-clock time go?".
- Attach with `-f` to follow threads and children.
- `-k` can significantly increase overhead because it unwinds stacks.
- `SIGINT` makes `strace` print its summary and detach. `SIGKILL` loses the summary.
- Use `--seccomp-bpf` to reduce filtering overhead when tracing a narrow syscall set.
- For a container, attach directly to `$HOST_PID`; only use `nsenter` when the tracer itself needs the container's mount/network view.

### C. CPU, Cache, and Hardware Profiling

| Tool | Mechanism | Use |
|---|---|---|
| `perf stat` | PMC counting | Cycles, instructions, IPC, cache/branch/TLB events, context switches, migrations |
| `perf record` / `perf report` | PMC sampling + call graphs | Hot functions, call stacks, flame graphs, annotated source/assembly |
| `perf top` | Live sampling | System/process hotspots without writing a perf.data file |
| `perf c2c` | Cache-line contention analysis | False sharing and cross-core cache-line ping-pong |
| `perf mem` | Memory access sampling | Load/store latency and data-source analysis (CPU-dependent) |
| `perf annotate` | Sample-to-instruction mapping | Which instructions in a function are hot |
| `perf diff` | Compare perf.data files | Regression and change analysis |
| `perf sched` / `perf lock` | Scheduler/lock tracepoints | Run-queue latency, context-switch patterns, lock contention |
| Valgrind Callgrind | Dynamic binary instrumentation | Deterministic instruction/call counts when speed is irrelevant |
| Intel PT / LBR | Branch tracing | Precise control-flow and branch history (CPU/kernel dependent) |

Examples:

```bash
# Counting. Attach for a fixed duration and stop with a graceful SIGINT.
sudo timeout -s INT 5 perf stat -d -p "$HOST_PID"
sudo timeout -s INT 5 perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,branches,branch-misses,cache-references,cache-misses -p "$HOST_PID"
perf stat -r 5 --null ./hasher large_file.bin
sudo timeout -s INT 5 perf stat -j -e cycles,instructions -p "$HOST_PID"

# Sampling and flame graphs. Record first; use --symfs when reporting if needed.
sudo timeout -s INT 10 perf record -F 99 -g -p "$HOST_PID"
perf report --stdio
perf script | stackcollapse-perf.pl | flamegraph.pl > cpu.svg
perf annotate --stdio

# Memory-system and concurrency views. c2c/mem require CPU support.
sudo timeout -s INT 10 perf mem record -p "$HOST_PID"
perf mem report
sudo timeout -s INT 10 perf c2c record -p "$HOST_PID"
perf c2c report
sudo timeout -s INT 5 perf sched record -p "$HOST_PID"
perf sched latency
sudo timeout -s INT 5 perf lock record -p "$HOST_PID"
perf lock contention
```

Portability notes:

- `perf stat -d` and named cache events are CPU-dependent. Use `perf list` to discover valid events. On ARM, VMs, and some cloud hosts, use software events and generic `cache-references`/`cache-misses`.
- Hardware counters can be multiplexed when there are more events than counters. Check `perf stat` output for scaling and use fewer events for clean comparisons.
- `perf c2c` and `perf mem` need processor support for precise memory sampling; on unsupported CPUs they fail or produce limited data. Check the error and report the limitation.
- The output of `perf script` can be fed to the FlameGraph tooling; the `stackcollapse-perf.pl` and `flamegraph.pl` scripts come from the separate FlameGraph project (<https://github.com/brendangregg/FlameGraph>) and are not installed by `perf`.
- `perf sched` and `perf lock` rely on tracepoints in tracefs. If tracefs is not readable by the current user, run as root or fix the tracefs mount permissions; `perf lock` also depends on kernel lock tracepoints being available.
- Call graphs need frame pointers, DWARF unwinding, or LBR. Build with `-fno-omit-frame-pointer -g` for reliable stacks; Rust needs `debug = 1` or `2` and should not strip symbols during analysis; Go binaries include runtime metadata but need symbolization care.
- `perf record` does not accept `--symfs`. Record normally, then pass `--symfs /proc/$HOST_PID/root` to `perf report`, `perf annotate`, or `perf script` when the target is a container. `--buildid-all`/`--buildid-mmap` help retain build IDs for later symbolization.
- Keep aside a baseline `perf.data`; `perf diff` and differential flame graphs are more useful than a single profile.

### D. Memory Analysis

| Question | Tools |
|---|---|
| How much memory is resident/shared/private? | `/proc/PID/smaps_rollup`, `pmap -x`, `smem` |
| Which mappings grew? | `smaps_rollup`, `smaps`, `pmap`, `perf record -e page-faults` |
| Is there a leak? | `heaptrack`, Valgrind Massif, `bpftrace`/BCC `memleak`, allocator stats |
| Is the cgroup under pressure or near OOM? | `memory.current`, `memory.stat`, `memory.events`, `memory.pressure` |
| Is NUMA placement bad? | `/proc/PID/numa_maps`, `numastat`, `numactl -H`, `perf mem` |
| Is swap/THP involved? | `/proc/PID/status`, `smaps_rollup`, `/sys/kernel/mm/transparent_hugepage` |

Commands:

```bash
cat /proc/$HOST_PID/smaps_rollup
grep -E 'VmRSS|VmHWM|VmSwap|Threads' /proc/$HOST_PID/status
pmap -x "$HOST_PID"
smem -p -P "$TARGET"
valgrind --tool=massif ./hasher large_file.bin
sudo heaptrack ./hasher large_file.bin
cat /sys/fs/cgroup${CGROUP_PATH}/memory.current
cat /sys/fs/cgroup${CGROUP_PATH}/memory.stat
cat /sys/fs/cgroup${CGROUP_PATH}/memory.events
cat /proc/pressure/memory
```

Remember that RSS double-counts shared pages. PSS is the better per-process signal; cgroup memory accounting is the better container signal. A container can be OOM-killed because of its cgroup limit even when the host has free memory.

### E. I/O and Filesystem Analysis

| Tool | Purpose |
|---|---|
| `/proc/PID/io` | Per-process read/write byte and syscall counters |
| `pidstat -d` | Per-process I/O rates and delay |
| `iotop` | Live per-process I/O |
| `lsof` | Open files, sockets, pipes, deleted files |
| BCC `biolatency`, `biosnoop`, `filetop`, `cachestat` | Block I/O latency, file I/O, page-cache behavior |
| `strace -e trace=%file,%desc` | Exact file operations and errors |
| `bpftrace` tracepoints | `block:*`, `vfs:*`, `io_uring:*`, `filemap:*` |

Commands:

```bash
cat /proc/$HOST_PID/io
pidstat -d -p "$HOST_PID" 1
sudo iotop -p "$HOST_PID"
lsof -p "$HOST_PID"
lsof +L1 -p "$HOST_PID"
cat /proc/$HOST_PID/fdinfo/3
sudo /usr/share/bcc/tools/biolatency -p "$HOST_PID" 10
sudo /usr/share/bcc/tools/filetop -C 5
```

Useful details:

- `rchar`/`wchar` count bytes through the syscall interface, including page-cache hits; `read_bytes`/`write_bytes` count actual block-device I/O.
- `iodelay` from `pidstat` and block-layer latency from BCC answer different questions. One includes queueing and device time; the other may not.
- `lsof +L1` finds deleted-but-open files, a classic cause of disk-space leaks.
- OverlayFS adds a layer of path translation. The host path in `lsof` is not the path the container sees.
- `io_uring` operations may not appear as ordinary blocking syscalls; use its tracepoints or eBPF probes.

### F. Networking Analysis

| Tool | Purpose |
|---|---|
| `ss` | Socket states, queues, processes, TCP information |
| `ss -ti` | TCP congestion, RTT, retransmits, cwnd, delivery rate |
| `nstat` / `netstat -s` | Kernel network counters |
| `tcpdump` | Packet-level evidence |
| `ip -s link`, `ethtool -S` | Interface and NIC counters |
| BCC `tcpconnect`, `tcpaccept`, `tcplife`, `tcpretrans` | Connection lifecycle and retransmit tracing |
| `nsenter -n` | Network namespace view for containers |

Commands:

```bash
ss -tiepm
ss -s
nstat -az
ip -s link
sudo tcpdump -i any -nn -tttt
sudo /usr/share/bcc/tools/tcpretrans -T
sudo /usr/share/bcc/tools/tcplife -T
```

For containers, separate host networking from container networking. A socket visible on the host may be NATed or attached to a `veth`; use the container's network namespace or the runtime's port mapping to interpret it. `ss` inside `nsenter -n` sees the container's sockets, not the host's.

### G. Scheduler, Locks, and Concurrency

This is the lens that answers "why are my threads not making progress?" and "where is the concurrency bug?".

| Tool | Question |
|---|---|
| `/proc/PID/task/*/{stat,status,wchan,stack,schedstat,comm}` | Thread explorer: per-thread state, run/wait time, blocking kernel function |
| `ps -L`, `top -H`, `htop` | Thread list and states |
| `gdb -p PID -batch -ex 'thread apply all bt full'` | Full user-space stacks for all threads |
| `eu-stack -p PID` / `gstack` / `pstack` | Lightweight thread stacks |
| `perf sched record/report/timehist` | Scheduler latency and run-queue behavior |
| `perf lock record/report/contention` | Lock contention and wait times |
| BCC `offcputime`, `wakeuptime`, `runqlat`, `runqlen`, `cpudist` | Off-CPU time, wakeup chains, run-queue latency |
| `perf c2c` | False sharing and cache-line contention |
| ThreadSanitizer, Helgrind, DRD | Data-race detection under instrumentation |
| `rr` | Deterministic record/replay of concurrency |
| Go `go tool trace`, `GODEBUG=schedtrace=1000`; JVM JFR/`jstack`; Python `py-spy`; Node inspector | Runtime-aware concurrency views |

Commands:

```bash
ps -T -p "$HOST_PID" -o pid,tid,psr,stat,wchan:30,comm
for t in /proc/$HOST_PID/task/*; do echo "== $t"; cat "$t/wchan"; cat "$t/stack"; done
sudo gdb -q -p "$HOST_PID" -batch -ex 'set pagination off' -ex 'thread apply all bt full'
sudo eu-stack -p "$HOST_PID"
sudo timeout -s INT 5 perf sched record -p "$HOST_PID"
sudo perf sched latency
sudo timeout -s INT 5 perf lock record -p "$HOST_PID"
sudo perf lock contention
sudo /usr/share/bcc/tools/offcputime -p "$HOST_PID" 5
sudo /usr/share/bcc/tools/runqlat -p "$HOST_PID" 5
valgrind --tool=helgrind ./hasher large_file.bin
```

Interpretation:

- A thread in `D` state is usually blocked in uninterruptible I/O; `wchan` and `/proc/PID/task/TID/stack` show where.
- Large `schedstat` wait time with little run time points to run-queue contention or cgroup CPU throttling. Check `cpu.stat` (`nr_throttled`, `throttled_usec`) and PSI.
- High off-CPU time with a short on-CPU profile can indicate lock waits, futex sleeps, I/O waits, or sleeping synchronization.
- `perf c2c` is essential for "threads are scaling badly but not locking": false sharing shows up as cache-line contention.
- `strace` futex time is a symptom, not proof. Correlate with lock traces, thread stacks, and scheduler data.
- Helgrind/TSAN find races but are not production tools; `rr` is for reproducing a failure deterministically.
- Check affinity and scheduling policy before blaming the code: a process confined to one CPU or throttled by a cgroup will show scheduler symptoms that look like lock contention.

```bash
taskset -pc "$HOST_PID"
chrt -p "$HOST_PID"
nice -n 0 taskset -c 2-5 ./hasher large_file.bin   # controlled launch
```

### H. Debugger, Core Dumps, and Reverse Engineering

| Tool | Purpose |
|---|---|
| `gdb` / `lldb` | Live attach, breakpoints, watchpoints, memory/register inspection, thread stacks |
| `gcore` | Force a core dump of a running process without killing it |
| `systemd-coredump` / `coredumpctl` | Managed core dump collection and inspection |
| `ulimit -c`, `kernel.core_pattern` | Core dump policy |
| `eu-stack`, `pstack`, `gstack` | Quick stack capture |
| `objdump`, `readelf`, `nm`, `addr2line` | ELF structure, symbols, disassembly, source mapping |
| `rizin`, `radare2`, `Ghidra` | Interactive disassembly, control-flow graphs, decompilation |
| `perf annotate` | Hot-instruction/source mapping from real samples |
| `debuginfod`, `debuginfod-find` | Automatic debug-info lookup by build ID |

Commands:

```bash
# Live inspection (stops the process while attached).
sudo gdb -q -p "$HOST_PID"
(gdb) set pagination off
(gdb) thread apply all bt full
(gdb) info proc mappings
(gdb) info registers
(gdb) detach
(gdb) quit

# Batch mode for automation.
sudo gdb -q -p "$HOST_PID" -batch -ex 'set pagination off' -ex 'thread apply all bt full'

# Core dumps.
ulimit -c unlimited
cat /proc/sys/kernel/core_pattern
sudo gcore "$HOST_PID"
coredumpctl list
sudo coredumpctl gdb "$HOST_PID"
gdb ./hasher core

# ELF inspection.
file ./hasher
readelf -h -S -s -d ./hasher
nm -C ./hasher | grep hash_chunk
objdump -d -M intel --disassemble=hash_chunk ./hasher
addr2line -e ./hasher -f -C 0x401234
```

Core dump notes:

- `gcore` and `gdb -p` stop the process while they run. Never do this on a latency-sensitive production process without a plan.
- Core dumps can contain secrets (keys, credentials, customer data). Treat them as sensitive artifacts.
- `core_pattern` on a container host may pipe cores to `systemd-coredump`. Whether a container core reaches that handler depends on the container's namespaces, ulimits, and runtime configuration.
- Stripped binaries make assembly analysis and stack symbolization much harder. Keep an unstripped artifact and its build ID; `debuginfod` can retrieve debug info if the build ID is indexed.

### I. eBPF and Ftrace Ecosystem

| Tool | Purpose |
|---|---|
| `bpftrace` | High-level one-liners and scripts over tracepoints, kprobes, uprobes, USDT |
| BCC tools | Ready-made production probes: `execsnoop`, `opensnoop`, `biolatency`, `filetop`, `cachestat`, `offcputime`, `runqlat`, `tcpretrans`, `memleak`, `syscount` |
| `bpftool` | Inspect programs, maps, BTF, and feature support |
| `perf probe` | Create dynamic kprobes/uprobes without writing a program |
| `trace-cmd` | Record and report ftrace data |
| `LTTng` | Low-overhead system and user-space tracing |
| `SystemTap`, `DTrace` | Alternative dynamic instrumentation (kernel/runtime dependent) |
| `sysdig` | Event capture and filtering (often eBPF-backed) |

Examples:

```bash
sudo bpftrace -l 'tracepoint:syscalls:*'
sudo bpftrace -e 'tracepoint:syscalls:sys_enter_openat { @[comm, str(args->filename)] = count(); }'
sudo bpftrace -e 'kprobe:vfs_read { @[comm] = count(); }'
sudo bpftrace -e 'uprobe:/path/to/hasher:hash_chunk { @[ustack] = count(); }'
sudo bpftrace -l 'usdt:/path/to/application:*'
sudo bpftrace -e 'usdt:/path/to/application:probe_name { @[ustack] = count(); }'
sudo bpftool feature probe kernel
sudo perf probe -x ./hasher hash_chunk
sudo trace-cmd record -p function_graph -P "$HOST_PID" -- sleep 5
```

Use eBPF when you need event-level detail with acceptable overhead. Use `/proc` and metrics when you need cheap state. Do not build a monitoring system only out of `strace`.

### J. Hardware, NUMA, Power, and DMA

This is the lowest layer and the one most often ignored.

| Question | Tools |
|---|---|
| What CPU/topology/cache hierarchy is present? | `lscpu`, `lscpu -e`, `lstopo`/`hwloc`, `/sys/devices/system/cpu` |
| Is the CPU throttling or changing frequency? | `turbostat`, `cpupower frequency-info`, `/sys/class/thermal`, `powertop` |
| Is the VM stealing CPU? | `/proc/stat` steal, `mpstat`, `perf kvm` (host) |
| Is memory bandwidth or the memory controller saturated? | `perf stat -e uncore_imc...`, Intel PCM, `likwid`, `stream` |
| Is NUMA placement wrong? | `numactl -H`, `numastat`, `perf mem`, `/proc/PID/numa_maps` |
| Are interrupts/softirqs adding latency? | `/proc/interrupts`, `/proc/softirqs`, `mpstat -P ALL`, BCC `hardirqs`/`softirqs` |
| Is the device/NIC/GPU/IOMMU the bottleneck? | `lspci -vv`, `ethtool -S`, `nvme smart-log`, `nvidia-smi`/`nvtop`, `/sys/class/iommu`, `/sys/class/dma` |
| Is GPU/accelerator work the bottleneck? | `nvidia-smi dmon`, Nsight Systems/Compute (`nsys`, `ncu`), `rocm-smi`, `rocprof` (vendor-specific) |
| Is DMA visible? | Usually only indirectly; see below |

Commands:

```bash
lscpu
lscpu -e
lstopo --of console
numactl --hardware
numastat -p "$HOST_PID"
sudo turbostat --quiet --interval 1
sudo cpupower frequency-info
sudo powertop --time=10
sudo perf stat -a -e power/energy-pkg/,power/energy-cores/ -- sleep 5
sudo perf stat -a -e uncore_imc_free_running/data_read/,uncore_imc_free_running/data_write/ -- sleep 5
cat /proc/interrupts
cat /proc/softirqs
sudo lspci -vv
ls /sys/class/dma
ls /sys/class/iommu
```

About DMA and device interactions:

- Application code does not usually issue DMA instructions. Devices (NIC, NVMe, GPU, IOMMU-mapped accelerators) perform DMA, often asynchronously. CPU-side profilers see the driver, interrupts, memory-mapped I/O, and cache-coherency effects, not the DMA transfer itself.
- Useful indirect evidence: `/sys/class/dma`, `/sys/class/iommu`, `dma-buf` debugfs (root; `/sys/kernel/debug/dma_buf` when available), `dma_fence` tracepoints (GPU), `iommu` tracepoints, PCIe AER counters (`lspci -vv`), and vendor counters (`ethtool -S`, `nvidia-smi`, NVMe SMART).
- `perf` uncore/memory-controller events measure memory traffic and bandwidth, not device DMA directly. They are the right tool for "is the memory subsystem saturated?". RAPL and uncore events are package/socket-scoped, not per-process; run them in per-CPU/system-wide mode (`-a`) rather than attaching them to a PID. Event names such as `uncore_imc_free_running/*` are platform-specific; confirm with `perf list`.
- On cloud/VMs, many uncore and IOMMU events are unavailable. Record that limitation instead of reporting zero.
- `turbostat`, `cpupower`, `powertop`, and detailed `lspci -vv` output often require root because they read MSRs or privileged PCI config space. Frequency/thermal data without root is usually partial.

### K. Container-Specific Tooling

| Tool | Purpose |
|---|---|
| Runtime APIs (`docker`, `podman`, `nerdctl`, `crictl`) | Container ID, host PID, cgroup path, merged rootfs, image digest |
| `docker top` / `crictl ps` | Process list with host PIDs |
| `nsenter` | Enter selected namespaces for tools that need the container view |
| Cgroup v2 files | Authoritative CPU/memory/I/O/PID accounting and limits |
| `systemd-cgtop`, `docker stats`, `cadvisor` | Higher-level container resource views |
| `/proc/$HOST_PID/root` | Container root filesystem from the host, when permitted |
| `docker diff`, `dive`, `docker history` | Filesystem/image layer inspection |

Commands:

```bash
HOST_PID=$(docker inspect -f '{{.State.Pid}}' <container>)
CONTAINER_ID=$(docker inspect -f '{{.Id}}' <container>)
MERGED=$(docker inspect -f '{{.GraphDriver.Data.MergedDir}}' <container>)
CGROUP_PATH=$(awk -F: '$1=="0" {print $3}' /proc/$HOST_PID/cgroup)

cat /proc/$HOST_PID/status | grep -E 'NSpid|Name|Seccomp'
cat /proc/$HOST_PID/cgroup
docker top <container>
cat "/sys/fs/cgroup${CGROUP_PATH}/cgroup.procs"
cat "/sys/fs/cgroup${CGROUP_PATH}/cpu.stat"
cat "/sys/fs/cgroup${CGROUP_PATH}/memory.current"
cat "/sys/fs/cgroup${CGROUP_PATH}/io.stat"
cat "/sys/fs/cgroup${CGROUP_PATH}/cpu.pressure"
```

For Podman, `podman inspect --format '{{.State.Pid}}' <container>` and `podman top`. For containerd/CRI, `crictl inspect <id>` and `crictl ps -o json`. Do not assume Docker paths or the `overlay2` storage driver.

### L. Language Runtimes

Compiled binaries expose machine code directly. Managed runtimes do not; the OS sees the interpreter or VM, so you need runtime-aware tools.

| Runtime | Tools | Notes |
|---|---|---|
| Go | `go tool pprof`, `go tool trace`, `GODEBUG=schedtrace=1000`, `GODEBUG=asyncpreemptoff=1` | Runtime scheduler and goroutine traces; native perf needs symbolization |
| Rust | `cargo flamegraph`, `pprof-rs`, `tokio-console`, `perf` + symbols | Build with debug info and frame pointers; async tasks need runtime-aware views |
| JVM | JFR, `jcmd`, `jstack`, async-profiler, `jmap`, `jstat` | JIT code is invisible to naive CPU profilers; use safepoint-aware profilers |
| Python | `py-spy`, `scalene`, `cProfile`, `strace`/`perf` for native work | GIL changes concurrency behavior; `py-spy dump` is a low-friction thread dump |
| Node.js | `--cpu-prof`, `--heap-prof`, `--perf-basic-prof`, inspector, `clinic` | V8 JIT; use perf map/JIT dump for native profiles |

The key rule: for interpreted/JIT code, first identify the runtime-level question, then choose a runtime-aware profiler. `perf record` alone will show the VM's compiler or interpreter loop, not your source-level hot path.

---

## 3. Installation and Baseline Setup

The following is for Ubuntu/Debian-like systems. Adapt package names for other distributions and prefer your distribution's kernel-matched packages.

```bash
sudo apt update

# Core process/resource tools.
sudo apt install -y build-essential htop atop lsof sysstat iproute2 procps psmisc

# Tracing, debugging, and ELF tooling.
sudo apt install -y strace ltrace gdb binutils elfutils systemd-coredump

# perf: the meta package may install a perf for a different kernel.
sudo apt install -y linux-tools-common "linux-tools-$(uname -r)"
# If the kernel-specific package is unavailable, use the distribution's
# linux-tools-generic and verify `perf --version` against `uname -r`.

# eBPF and tracing.
sudo apt install -y bpftrace bpfcc-tools bpftool trace-cmd

# Optional but useful.
sudo apt install -y valgrind heaptrack iotop-c smem numactl hwloc linux-cpupower \
  linux-tools-common sysdig hyperfine stress-ng
```

`rr` and Ghidra may need manual installation or distribution-specific packages:

- rr: <https://rr-project.org/>
- Ghidra: <https://ghidra-sre.org/>
- bpftrace: <https://github.com/bpftrace/bpftrace>
- BCC: <https://github.com/iovisor/bcc>

Verify the environment before benchmarking:

```bash
uname -r
perf --version
strace --version
bpftrace --version
rr --version
cat /proc/sys/kernel/perf_event_paranoid
cat /proc/sys/kernel/yama/ptrace_scope
ls /sys/kernel/btf/vmlinux 2>/dev/null || echo "no BTF"
mount | grep -E 'debugfs|tracefs'
```

Expected outcomes:

- `perf stat -e task-clock true` succeeds only if permissions allow it. If it fails, use root/`CAP_PERFMON` or a benchmark VM.
- `bpftrace -l 'tracepoint:syscalls:*'` requires root and a kernel with tracefs/BTF support.
- `rr record /bin/true` requires `perf_event_paranoid <= 1`; otherwise use `rr record -n /bin/true` for a slower fallback.
- `docker` commands require access to the runtime socket. Rootless containers may not expose all host PIDs or cgroup paths to an unprivileged user.

---

## 4. The Benchmark Scenario: "The Hasher"

The reference workload is a C, Rust, Go, or Zig program that reads a large file, hashes it in chunks using a thread pool, and writes the result. It exercises file I/O, memory bandwidth, thread synchronization, and CPU vectorization.

### 4.1 Build for Observability

Build artifacts with symbols and stack unwinding enabled:

```bash
# C
cc -O3 -g -fno-omit-frame-pointer -mavx2 hasher.c -o hasher -pthread

# Rust (Cargo.toml)
# [profile.release]
# debug = 2
# strip = "none"
cargo build --release

# Go
go build -trimpath -o hasher ./cmd/hasher
# Go includes runtime metadata; keep the binary unstripped for analysis.

# Debug-friendly comparison build.
cc -O0 -g -fno-omit-frame-pointer hasher.c -o hasher_O0 -pthread
```

Keep a copy of the unstripped binary and its build ID. If you strip the deployed artifact, archive the debug artifact separately.

### 4.2 Benchmark Discipline

Before comparing runs:

- Warm up the page cache and the thread pool.
- Pin the workload (`taskset -c 2-5`) and avoid running it on a busy host.
- Record CPU model, kernel version, governor, cgroup limits, container image digest, and tool versions.
- Run multiple iterations; report median and spread, not a single sample.
- Use `perf stat -r 5` or `hyperfine` for wall-clock comparisons.
- Do not compare a native run against a container run without accounting for different filesystems, cgroup limits, and runtime processes.

### 4.3 Step 1: High-Level Resource Verification

```bash
pidstat -t -p "$HOST_PID" 1
pidstat -d -r -w -p "$HOST_PID" 1
lsof -p "$HOST_PID"
cat /proc/$HOST_PID/io
cat /proc/$HOST_PID/smaps_rollup
cat /proc/$HOST_PID/cgroup
```

Look for:

- FD count and growth; open file paths; sockets and pipes.
- Voluntary vs involuntary context switches. A CPU-bound process should have few voluntary switches; a spinning lock should show little voluntary sleeping.
- Minor vs major page faults. Major faults suggest I/O or memory pressure.
- I/O delay and block-device bytes vs cache-served bytes.
- RSS/PSS growth, swap, and cgroup memory pressure.

Validation exercises:

- Open 500 files in the target and confirm the FD count increases by 500 plus the initial descriptors. Remember that `lsof` may include memory-mapped files and runtime descriptors.
- Put the target in a spin loop and confirm voluntary context switches stay near zero while CPU time stays high. Use `pidstat -w` and distinguish voluntary (`cswch/s`) from involuntary (`nvcswch/s`).
- Touch a large file randomly and confirm major faults/block reads appear; then rerun with a warm page cache and confirm they drop.

### 4.4 Step 2: System-Call Bifurcation

```bash
sudo strace -c -f -p "$HOST_PID"
sudo strace -w -c -f -p "$HOST_PID"
sudo perf trace -p "$HOST_PID" --duration 5 -s
```

Questions this answers:

- Which syscalls dominate syscall time?
- Is the process blocked in `futex` (locking), `epoll`/`poll` (event waiting), `read`/`write` (I/O), or `nanosleep` (sleeping)?
- Are there unexpected `openat`/`stat` storms, repeated `mmap`, or many failed syscalls?

Validation exercises:

- A process calling `sleep(1)` repeatedly should show most syscall wall time in `clock_nanosleep`/`nanosleep`. Use `strace -w -c` for wall-clock accounting.
- A contended mutex should produce a large `futex` share. Correlate with thread stacks and `perf lock` rather than treating futex time as proof.
- An `openat` loop should show high `openat` counts; check whether the process is reopening a file per chunk instead of reusing a descriptor.

### 4.5 Step 3: Hardware Profiling (The "Why")

Run these with `sudo`/`CAP_PERFMON` if the host blocks unprivileged perf (for example, `perf_event_paranoid` above 1).

```bash
perf stat -d ./hasher large_file.bin
perf stat -r 5 ./hasher large_file.bin
perf record -F 99 -g ./hasher large_file.bin
perf report --stdio
perf mem record ./hasher large_file.bin
perf mem report
perf c2c record ./hasher large_file.bin
perf c2c report
```

Interpretation:

- **IPC** (instructions per cycle) below roughly 1 suggests stalls; inspect cache, branch, and memory events.
- **L1/LLC load misses** can indicate poor locality or working sets larger than cache. Confirm with `perf mem` and `perf c2c`.
- **Branch misses** can come from unpredictable data, indirect calls, or a bad algorithm; check the hot branch in `perf annotate`.
- **dTLB misses** and page walks point to large working sets, THP, or page-size choices.
- **CPU migrations** and context switches indicate scheduler interference; pin the workload for cleaner comparisons.
- **False sharing** shows up in `perf c2c` as cache-line contention between threads even when the code does not use locks.

Validation exercises:

- Compare a contiguous-array traversal with a randomized linked-list traversal. The linked list should show materially higher cache misses. Use multiple runs and normalized counts because raw counters are noisy.
- Compare a predictable loop with a data-dependent branch. The unpredictable version should show higher branch misses.
- Compile the same function with `-O0` and `-O3 -mavx2`; compare instructions, cycles, and vector instruction counts.
- If hardware events are unavailable, use `task-clock`, `context-switches`, `page-faults`, `cpu-migrations`, and software tracing instead of reporting zeros.

### 4.6 Step 4: Concurrency and Determinism

Live inspection:

```bash
ps -T -p "$HOST_PID" -o pid,tid,psr,stat,wchan:30,comm
sudo eu-stack -p "$HOST_PID"
sudo gdb -q -p "$HOST_PID" -batch -ex 'set pagination off' -ex 'thread apply all bt full'
sudo timeout -s INT 5 perf sched record -p "$HOST_PID"
sudo perf sched latency
sudo timeout -s INT 5 perf lock record -p "$HOST_PID"
sudo perf lock contention
sudo /usr/share/bcc/tools/offcputime -p "$HOST_PID" 5
sudo /usr/share/bcc/tools/runqlat -p "$HOST_PID" 5
```

Deterministic record/replay:

```bash
rr record ./hasher large_file.bin
rr record --chaos ./hasher large_file.bin   # randomize scheduling to expose races
rr replay
# Inside the rr replay gdb session:
# (gdb) continue
# (gdb) reverse-continue
# (gdb) break hash_chunk
# (gdb) thread apply all bt
rr pack
```

If the workload is mostly idle, generate controlled concurrent load from another process (for example, `stress-ng --cpu 4 --vm 2 --timeout 30s`) and use `rr record --chaos` to vary scheduling. The replay itself remains deterministic; `--chaos` changes what was recorded, not how it is replayed.

Validation exercises:

- Attach to a deadlocked multithreaded target and confirm `thread apply all bt full` shows the blocked stacks without restarting it. Note that attaching stops the process while gdb is active.
- Create a race condition that fails intermittently natively; confirm it reproduces deterministically under `rr record`/`rr replay`.
- Use `perf c2c` on a false-sharing benchmark and confirm the contended cache line appears. Fix the layout and compare again.
- Use `runqlat` and `offcputime` to distinguish "waiting for CPU" from "waiting for a lock/I/O".

`rr` caveats:

- It records from launch; it cannot attach to an already running process. Attach with gdb for live inspection, launch under rr for deterministic replay.
- It requires `perf_event_paranoid <= 1` for normal record mode; `rr record -n` is a slower fallback.
- It is sensitive to CPU features, kernel version, and container privileges. Record and replay in the same or a compatible environment.
- `rr pack` makes a trace more self-contained, but it does not virtualize the CPU or kernel.

### 4.7 Step 5: Assembly and Source Inspection

Use the symbol, not a hardcoded address, when the binary has symbols:

```bash
nm -C ./hasher | grep hash_chunk
objdump -d -M intel --demangle --disassemble=hash_chunk ./hasher
perf annotate --stdio
addr2line -e ./hasher -f -C 0x401234
readelf -h -S -s -d ./hasher
```

If the binary is stripped, locate the function through the profiler's symbol/address information (or the symbol table from the unstripped artifact) and use `objdump --start-address`/`--stop-address`. For interactive control-flow analysis, use `rizin`, `radare2`, or Ghidra on the extracted binary.

What to check:

- Is the hot loop vectorized (`vmovdqa`, `vaddps`, AVX2/AVX-512 instructions)?
- Are there unexpected calls, spills, or bounds checks in the inner loop?
- Does the compiler use efficient memory access patterns, or is it reloading values?
- Do `perf annotate` hotspots match the source lines and assembly you expect?
- Are debug symbols and source paths consistent with the deployed binary? A different build yields misleading line mappings.

Validation exercise:

- Compile with `-O0` and with `-O3 -mavx2`. The optimized build should contain vector instructions; the unoptimized build should not. If the optimized build still lacks them, inspect aliasing, alignment, and loop structure.

### 4.8 Step 6: Correlate and Compare

A single lens rarely proves a root cause. Combine:

1. A metric anomaly (for example, high CPU or high I/O delay).
2. An event-level confirmation (syscall, scheduler, block I/O, lock).
3. A stack/profile that names the code path.
4. An assembly/source check that explains the machine behavior.
5. A controlled A/B run that shows the change.

Keep baseline artifacts (`perf.data`, flame graphs, syscall summaries, thread dumps) and compare them after changes. `perf diff` and differential flame graphs are the fastest way to see whether a fix moved the bottleneck rather than removing it.

---

## 5. Container Workflow

### 5.1 Resolve the Target Set

```bash
CONTAINER=<name-or-id>
HOST_PID=$(docker inspect -f '{{.State.Pid}}' "$CONTAINER")
CONTAINER_ID=$(docker inspect -f '{{.Id}}' "$CONTAINER")
MERGED=$(docker inspect -f '{{.GraphDriver.Data.MergedDir}}' "$CONTAINER")
CGROUP_PATH=$(awk -F: '$1=="0" {print $3}' "/proc/$HOST_PID/cgroup")

printf 'host_pid=%s\nid=%s\nmerged=%s\ncgroup=%s\n' \
  "$HOST_PID" "$CONTAINER_ID" "$MERGED" "$CGROUP_PATH"

docker top "$CONTAINER"
cat "/sys/fs/cgroup${CGROUP_PATH}/cgroup.procs"
cat "/sys/fs/cgroup${CGROUP_PATH}/cgroup.threads"
```

Guard against stopped containers and stale PIDs:

```bash
test -n "$HOST_PID" && test "$HOST_PID" -gt 0 || { echo "container not running"; exit 1; }
test -r "/proc/$HOST_PID/stat" || { echo "host PID gone"; exit 1; }
# starttime is field 22. comm can contain spaces, so strip through the closing ')' first.
sed 's/^.*) //' "/proc/$HOST_PID/stat" | awk '{print $20}'
```

For Podman use `podman inspect`, for containerd/CRI use `crictl inspect`, and for Kubernetes resolve the container on its node. There is no single portable "container PID" API.

The cgroup extraction above assumes cgroup v2. For cgroup v1, read the controller-specific lines from `/proc/PID/cgroup` and the matching mounts from `/proc/PID/mountinfo`; there is no single cgroup path. Rootless containers may also place the scope under a user slice, and an unprivileged user may not be able to read another user's cgroup files.

### 5.2 Run the Core Steps Against the Host PID

From the host:

```bash
# Step 1.
pidstat -t -p "$HOST_PID" 1
lsof -p "$HOST_PID"

# Step 2: attach directly; no namespace entry needed for syscall tracing.
sudo strace -c -f -p "$HOST_PID"

# Step 3: host perf targeting the host PID and cgroup.
sudo timeout -s INT 5 perf stat -d -p "$HOST_PID"
# -G expects a cgroup name/path form accepted by the installed perf; verify before trusting the result.
sudo perf stat -a -e task-clock -G "$CGROUP_PATH" -- sleep 5
sudo timeout -s INT 10 perf record -F 99 -g -p "$HOST_PID"
sudo perf report --symfs "/proc/$HOST_PID/root" --stdio

# Step 4.
ps -T -p "$HOST_PID" -o pid,tid,psr,stat,wchan:30,comm
sudo gdb -q -p "$HOST_PID" -batch -ex 'set pagination off' -ex 'thread apply all bt full'
```

Why host-side tools:

- They avoid installing tools in a distroless/minimal image.
- They avoid granting the application container `SYS_ADMIN` or `SYS_PTRACE`.
- They see the true host PID and cgroup accounting.

The container's mount namespace still matters for symbols and paths. Pass `--symfs /proc/$HOST_PID/root` to `perf report`, `perf annotate`, or `perf script` (not to `perf record`) or use the runtime's merged directory so perf can resolve the container's user-space files.

### 5.3 Namespace Injection Without the Common Mistakes

`nsenter` is for tools that must see the container's namespaces. It does not remove the observer effect.

```bash
# Enter selected namespaces, not blindly all of them.
# After entering the PID namespace, the container's init process is PID 1.
sudo nsenter -t "$HOST_PID" -m -u -i -n -p -- strace -c -f -p 1
sudo nsenter -t "$HOST_PID" -m -u -i -n -p -- ss -tiepm
sudo nsenter -t "$HOST_PID" -m -u -i -n -p -- cat /etc/os-release
```

Rules:

- `nsenter -a` enters all namespaces, including the user namespace. It can fail on rootless containers or restricted hosts. Prefer the smallest set you need: `-m` for mount, `-u` for UTS, `-i` for IPC, `-n` for network, `-p` for PID, and `-U` only if required.
- Once inside the target PID namespace, host PIDs are not visible. Use PID 1 (or a PID resolved inside the namespace), not `$HOST_PID`.
- `nsenter` requires `CAP_SYS_ADMIN` in the relevant user namespace; run it with `sudo` unless your capabilities are configured otherwise.
- If attaching to PID 1 inside the namespace is blocked by capabilities or seccomp, fall back to attaching to `$HOST_PID` from the host; the syscall trace is the same.
- For pure syscall tracing, `strace -p $HOST_PID` from the host is simpler and equally accurate. Use `nsenter` when the tool needs container files, network state, or a runtime that only exists in the container.

### 5.4 Deterministic Replay in a Container

`rr` must launch the process. The practical options are:

1. Run a dedicated debug container that includes `rr`, with only the capabilities it needs.
2. Record inside the container, then extract and replay a packed trace on a compatible host.

Example debug container:

```bash
docker run --rm -it \
  --cap-add=SYS_PTRACE --cap-add=SYS_ADMIN \
  --security-opt seccomp=unconfined \
  -v "$PWD:/work" -w /work \
  --entrypoint /bin/bash <image>
```

Inside it:

```bash
rr record /work/hasher /work/large_file.bin
rr pack /root/.local/share/rr/<trace>
```

Extraction and replay:

```bash
docker cp <container>:/root/.local/share/rr ./host_rr_trace
rr replay ./host_rr_trace/<trace>
```

Caveats:

- `rr` often needs host-level `perf_event_paranoid <= 1`; `rr record -n` is the fallback.
- The replayed environment must be compatible: same architecture, compatible CPU features, and ideally the same kernel. A trace is not a portable virtual machine.
- Do not grant `--privileged` when `--cap-add=SYS_PTRACE --cap-add=SYS_ADMIN` and a narrow seccomp profile suffice. Even those capabilities are powerful; use a disposable debug container.
- Recording inside a container can be affected by the container's PID/user namespaces and seccomp policy. Validate that `rr` starts before relying on the result.

### 5.5 Binary and Symbols

```bash
# Runtime-aware merged root, not a hardcoded overlay2 path.
MERGED=$(docker inspect -f '{{.GraphDriver.Data.MergedDir}}' "$CONTAINER")
file "$MERGED/app/hasher"
objdump -d -M intel --disassemble=hash_chunk "$MERGED/app/hasher"

# If the merged directory is not accessible, copy the binary out.
docker cp "$CONTAINER:/app/hasher" ./hasher_from_container
objdump -d -M intel --disassemble=hash_chunk ./hasher_from_container

# Or use the live process root.
sudo readelf -n "/proc/$HOST_PID/root/app/hasher"
```

Do not hardcode `/var/lib/docker/overlay2`: storage drivers and runtimes differ. Use the runtime API (`docker inspect`, `podman inspect`, `crictl inspect`) or `/proc/$HOST_PID/root`.

### 5.6 Kubernetes and Other Runtimes

For Kubernetes, run the analysis on the node or in a privileged debug pod that can see host PIDs:

```bash
crictl ps
crictl inspect <container-id>
kubectl get pod <pod> -o jsonpath='{.status.containerStatuses[*].containerID}'
kubectl debug node/<node> -it --image=busybox
```

If the pod uses `shareProcessNamespace: true`, processes are visible across containers in the pod. Otherwise, resolve the container ID on the node, then map it to the host PID and cgroup. Do not assume `kubectl exec` gives you a full process view.

### 5.7 Security Boundary

- Prefer host tools targeting the host PID over running privileged tools inside the application container.
- If you must enter the container, use a dedicated debug container and the minimum capabilities.
- `--cap-add=SYS_ADMIN` is close to root on the host in terms of attack surface. Do not combine it with untrusted workloads.
- Core dumps, traces, environment variables, and syscall arguments can contain secrets. Restrict access and delete artifacts after analysis.

---

## 6. Extending to JavaScript, Python, and Java

### 6.1 The Architectural Difference

- **Compiled (C/Rust/Go/Zig):** the binary contains machine code; DWARF/build metadata maps instructions to source.
- **Interpreted (Python/JS):** the OS sees the interpreter. `perf` sees the interpreter loop; language-level stacks require runtime-aware tools.
- **JIT (Java/Node/V8):** machine code is generated at runtime. Native profilers need JIT maps, perf maps, or runtime safepoint integration to attribute samples to source.

### 6.2 Language Tool Matrix

| Runtime | Runtime-level profiler | Thread/concurrency view | Native cross-check |
|---|---|---|---|
| Java | JFR, async-profiler | `jstack`, `jcmd Thread.print`, JFR events | `perf` with JIT symbols, `strace` |
| Python | `py-spy`, `scalene`, `cProfile` | `py-spy dump`, `faulthandler` | `perf` for C extensions; `strace` for the interpreter |
| Node.js | `--cpu-prof`, `--heap-prof`, inspector | inspector, `clinic doctor` | `--perf-basic-prof` / JIT dump + `perf` |
| Go | `go tool pprof`, `go tool trace` | `GODEBUG=schedtrace=1000`, runtime traces | `perf` with Go symbolization |
| Rust | `cargo flamegraph`, `pprof-rs` | `tokio-console`, `rr`, `perf` | `perf`, `perf c2c`, `gdb` |

### 6.3 Native Frames and JIT Symbols

- Java: start the JVM with `-XX:+PreserveFramePointer` when using perf, and use async-profiler or JFR for accurate Java frames. `jcmd <pid> JFR.start` and `jcmd <pid> Thread.print` are low-friction starting points.
- Node/V8: `node --perf-basic-prof script.js` writes a perf map; `--cpu-prof` writes a V8 CPU profile. Without a JIT map, perf samples may land in generated code with meaningless names.
- Python: `py-spy record -p PID` and `py-spy dump -p PID` read the interpreter's structures. C extensions still require native profiling; the GIL means "CPU-bound" Python threads may be serialized.
- Go: `go tool pprof` and `go tool trace` are usually better than raw perf for goroutine scheduling; keep the binary and use `perf` with Go's symbol tables for native hotspots.

### 6.4 Concurrency Caveats

Runtime schedulers add a layer between OS threads and application tasks. A goroutine, green thread, or async task can migrate between OS threads, so OS-level thread IDs are not stable application identifiers. Correlate:

- OS thread (`tid`) and runtime task/goroutine ID where the runtime exposes it.
- Runtime scheduler traces (Go execution trace, JVM JFR, Node async hooks) with OS scheduler data (`perf sched`, off-CPU).
- Wall-clock blocked time, not just on-CPU samples, for async and I/O-heavy workloads.

---

## 7. Validation and Overhead Budgets

Observability tooling must be validated at the same level of rigor as the application.

| Engine | Typical overhead | Privilege | Container notes |
|---|---|---|---|
| `/proc`, cgroup reads, `ss` | Very low | Usually unprivileged | Host-side reads work for host PIDs |
| `pidstat`, `lsof` | Low | Some fields root-only | Enumerate cgroup processes for container-wide coverage |
| `perf stat` | Low (counting) | `CAP_PERFMON`/root | Use host perf; `-a -G <cgroup>` for cgroup scope |
| `perf record` | Moderate | `CAP_PERFMON`/root | Record with build IDs; report with `--symfs` for container symbols |
| `perf c2c`/`perf mem` | Moderate-high | Root; CPU support | Hardware/VM support varies; uncore/RAPL are not per-process |
| `bpftrace`/BCC | Low-moderate | Root/CAP_BPF | Kernel BTF and probe availability matter |
| `strace` | Very high (10x-100x) | `CAP_SYS_PTRACE` | Attach to host PID; avoid in latency-critical paths |
| `gdb` attach | Stops the process | `CAP_SYS_PTRACE` | Attach via host PID; use batch commands |
| `rr record` | High (often 2x-20x or more) | `CAP_SYS_PTRACE`, perf policy | Prefer a dedicated debug container |
| Valgrind | Very high | Unprivileged | Useful for correctness, not production timing |
| `tcpdump` | Moderate-high | Root/CAP_NET_RAW | Use a narrow filter and short capture |

Acceptance criteria for the tool:

- Parsed FD counts match `lsof` plus known memory-mapped files and runtime descriptors.
- Syscall summaries match an independent `strace`/`perf trace` run on the same workload.
- A known cache-unfriendly workload produces a repeatable increase in cache misses; a known branch-unfriendly workload increases branch misses.
- A known deadlock produces complete thread stacks; a known race reproduces under `rr`.
- Hardware events are reported as unsupported when the CPU/VM does not provide them, never as zero.
- A dropped-event counter is visible whenever perf/BPF data is lost.
- Native vs container runs are compared with the same binary, affinity, cgroup limits, and workload; differences are explained, not assumed to be 5%.
- Tool overhead is measured per engine and stated in the report. Do not claim a 10-15% total budget while `strace` is active.

---

## 8. References

- Linux perf: <https://perf.wiki.kernel.org/> and <https://www.brendangregg.com/perf.html>
- perf security: <https://docs.kernel.org/admin-guide/perf-security.html>
- BPF performance tools: <https://www.brendangregg.com/bpf-performance-tools-book.html>
- bpftrace: <https://github.com/bpftrace/bpftrace>
- BCC: <https://github.com/iovisor/bcc>
- strace: <https://strace.io/>
- rr: <https://rr-project.org/>
- GDB: <https://sourceware.org/gdb/>
- systemd-coredump: <https://www.freedesktop.org/software/systemd/man/latest/systemd-coredump.html>
- Docker runtime and inspect: <https://docs.docker.com/>
- Linux namespaces: <https://man7.org/linux/man-pages/man7/namespaces.7.html>
- cgroup v2: <https://docs.kernel.org/admin-guide/cgroup-v2.html>
- Linux tracing: <https://docs.kernel.org/trace/index.html>
- Playground introduction video: <https://www.youtube.com/watch?v=qFEuYUfauNY>
