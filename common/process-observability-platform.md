# Process Observability Platform: Architecture Blueprint

This document specifies an all-in-one process and container observability tool. It is deliberately language-agnostic: the same architecture can be implemented in Rust, Go, C++, or a mixed stack.

The hands-on commands, tool installation, and benchmark scenarios live in [process-analysis-setup.md](process-analysis-setup.md). This blueprint focuses on the architecture, execution model, data model, validation strategy, and the failure modes that make observability tools unreliable.

The product promise is:

> Given a running process or container, explain what it is doing across every relevant lens: resource usage, system calls, scheduling and concurrency, memory, I/O, networking, CPU/cache behavior, hardware interaction, code paths, and static assembly, with a coherent timeline and defensible evidence.

---

## Part 0: Scope and Goals

### Goals

1. **Observe without modifying the target.** No source changes, no injected agents inside the application, no restart when the platform can attach.
2. **Support processes and containers.** Resolve a container to its host PID, cgroup, namespaces, and full process set.
3. **Expose multiple lenses on one target.** Resource metrics, syscalls, CPU hardware events, scheduler/locks, memory, I/O, network, debugger, core dumps, disassembly, and source maps.
4. **Correlate evidence.** A syscall, a stack sample, a thread state, and a disassembled instruction should be navigable as one investigation, not unrelated walls of text.
5. **Preserve data quality.** Report unsupported events, dropped samples, lost records, stale targets, and permission failures explicitly.
6. **Stay usable interactively and in automation.** A TUI/GUI for exploration, JSON/NDJSON and files for CI and agents, and a stable exit-code model.
7. **Keep overhead measurable and bounded.** Each engine declares its cost; the orchestrator enforces budgets and prevents accidental heavy-tool stacking.
8. **Degrade gracefully.** A missing `perf`, blocked `ptrace`, or unavailable PMU should remove a lens, not crash the platform.

### Non-Goals

- Replacing a production APM, distributed tracing backend, or long-term metrics store. The platform can export to them, but it is an investigation tool first.
- Being a container orchestrator or a Kubernetes control plane.
- Supporting every OS. This blueprint assumes Linux; macOS/Windows would need different kernel interfaces.
- Hiding the observer effect. The platform measures and reports its own overhead; it does not claim to be invisible.

### Primary Users and Workflows

| User | Typical workflow |
|---|---|
| Application developer | "Why is this binary slower or racing?" -> baseline, profile, syscalls, threads, source/assembly |
| Performance engineer | "Where do cycles and cache misses go, and what changed?" -> perf, c2c, diff, flame graphs |
| SRE/on-call | "What is this container doing right now?" -> process tree, cgroup pressure, syscalls, thread stacks |
| Security/reliability engineer | "What is this process touching, and what is blocked?" -> FDs, sockets, capabilities, seccomp, core dumps |
| CI/automation/agent | "Give me structured evidence and a pass/fail signal." -> JSON/NDJSON, thresholds, artifacts |

---

## Part 1: First Principles

### 1. The Kernel Is the Single Source of Truth

User-space abstractions such as "Docker containers" do not exist to the CPU scheduler or the memory manager. A container is a set of Linux processes isolated by **namespaces** and constrained by **cgroups**, rooted in a filesystem view such as OverlayFS.

Consequences:

- The kernel recognizes the **host PID**, not the container's internal PID. All host-side engines must target the host PID (and its cgroup/descendants).
- A container is not one process. The runtime init process can have children and worker processes, and every process has threads.
- Namespaces change what a process can see, not what the kernel accounts. `/proc/$HOST_PID/status` exposes both host and namespace PIDs via `NSpid`.
- Cgroup files are the authoritative container resource view. `docker stats` and `systemd-cgtop` are convenience layers over those files.

### 2. The Target Is a Set, Not a PID

The platform's unit of observation is a `TargetSet`, not a single integer:

- **Root process:** host PID, start time, executable, container/runtime identity.
- **Processes:** all members of the target cgroup or process tree.
- **Threads:** every task under each process.
- **Namespaces:** PID, mount, network, IPC, UTS, user, cgroup, time.
- **Cgroup path and limits:** CPU, memory, I/O, PIDs.
- **Provenance:** runtime, image digest, kernel, CPU, tool versions.

A single-PID model fails for multi-process containers, thread pools, `fork`/`exec` workloads, and PID-reuse races. The platform must enumerate and re-enumerate the set as processes appear and disappear.

PID reuse is a correctness issue, not a theoretical one. Verify the process start time from `/proc/PID/stat` (field 22) before and after attach. Prefer `pidfd_open` on kernels that support it, and never cache a PID without its start time.

### 3. Containers Need Runtime Adapters

There is no portable container API. The platform must abstract at least:

| Runtime | Host PID / cgroup / rootfs source |
|---|---|
| Docker | `docker inspect` (`.State.Pid`, `.Id`, `.GraphDriver.Data.MergedDir`, `.HostConfig.CgroupParent`) |
| Podman | `podman inspect` (`.State.Pid`, storage driver data) |
| containerd / CRI | `crictl inspect`, containerd API, runtime shims |
| Kubernetes | pod status -> container ID on node -> runtime inspect -> host PID/cgroup |

The adapter returns a normalized record; the rest of the platform never shells out to a runtime-specific command directly.

### 4. The Observer Effect Is Real and Must Be Quantified

Measuring a system changes the system:

- `docker exec` or `nsenter` creates a process, allocates page tables, perturbs caches, and changes scheduling.
- `strace` stops the tracee on every syscall and can slow it by 10x-100x.
- `gdb` attach stops the process while it is attached.
- `perf record` and BPF programs consume CPU and memory; ring buffers can drop events.
- Reading `/proc` is cheap but not free, especially when polling thousands of threads.

`nsenter` does not eliminate the observer effect. It reduces namespace and tool-environment mismatch. The platform should:

- Prefer attach over exec where the engine supports it.
- Use the minimum namespace set, not `nsenter -a` by default.
- Declare each engine's overhead class: passive, low, moderate, high, stop-the-world.
- Enforce a per-investigation overhead budget and warn when heavy engines are combined.
- Measure and display the platform's own CPU, memory, and dropped-event counts.

### 5. Acquisition and Presentation Must Be Decoupled

Profiling tools are blocking, long-running processes that emit high-volume streams. If the renderer waits on them, the UI freezes and the investigation becomes unreliable.

Rules:

- Each engine runs as a supervised subprocess or a dedicated acquisition task.
- `stdout` and `stderr` are read by independent reader tasks.
- Parsers convert raw records into typed events/metrics.
- A bounded queue protects the platform from a fast producer and a slow renderer.
- The renderer reads immutable snapshots; it never blocks an acquisition task.
- Cancellation is explicit and propagates to the engine, its process group, and its parsers.
- Slow consumers cause controlled drops or downsampling, never unbounded memory growth.

### 6. One Core, Multiple Renderers

A unified CLI and GUI is one **Orchestrator Core** with multiple **Renderers**:

- **Core:** target resolution, capability probing, engine supervision, parsing, normalization, correlation, storage, export.
- **GUI renderer:** immediate-mode rendering, `UI = f(StateSnapshot)`.
- **CLI/TUI renderer:** tables, JSON, NDJSON, live dashboards via ANSI.
- **Artifact renderer:** flame graphs, Perfetto/Chrome traces, reports, diff summaries.

The core must not depend on any renderer. The CLI and GUI are clients of the same state and event APIs.

### 7. Correlation and Data Quality Are First-Class

An observability tool is not useful if it produces plausible but unjoined data. The platform must preserve:

- **Source:** which engine produced the record.
- **Signal type:** metric, event, log, trace, or profile.
- **Time:** timestamp plus clock domain (`MONOTONIC`, `REALTIME`, `BOOTTIME`) and a conversion policy.
- **Identity:** host PID, thread ID, process start time, cgroup, container ID.
- **Units:** bytes, microseconds, cycles, percent; scaled perf counts must be marked.
- **Quality:** unsupported, estimated, sampled, multiplexed, dropped, partial.
- **Provenance:** kernel version, CPU model, tool version, kernel config/BTF availability.

Without these fields, "cache misses went up" or "futex time is high" cannot be defended.

### 8. Security and Least Privilege

The platform is a powerful debugger. Treat it as one:

- Prefer host-side tools targeting the host PID over privileged tools inside the application container.
- Request only the capabilities needed: `CAP_PERFMON` for perf, `CAP_BPF`/`CAP_SYS_ADMIN` for eBPF, `CAP_SYS_PTRACE` for ptrace.
- Avoid `--privileged`; use a dedicated, disposable debug container with a narrow seccomp profile when namespace entry is required.
- Never interpolate untrusted container names or PIDs into a shell command. Use argument vectors, validate numeric PIDs, and resolve names through runtime APIs.
- Traces, core dumps, environment variables, and syscall arguments can contain secrets. Encrypt or restrict artifacts and delete them after analysis.
- Verify the target is authorized. This tool can inspect any process the user can attach to.

### 9. Capability Gating and Graceful Degradation

Before starting an engine, probe the environment:

- Tool presence and version.
- `perf_event_paranoid`, `ptrace_scope`, capabilities, seccomp, kernel lockdown.
- PMU availability and specific event support.
- BTF, tracefs/debugfs, kprobe/uprobe support, kernel config.
- Container runtime socket access, cgroup version, namespace accessibility.
- Disk space for perf.data/core dumps and memory for ring buffers.

The engine registry should expose a fallback path:

| Preferred lens | If unavailable | Fallback |
|---|---|---|
| `perf` hardware events | No PMU/permission | Software events (`task-clock`, `context-switches`, `page-faults`) + `/proc` + `perf trace` if allowed |
| `bpftrace`/BCC | No BTF/permission | `strace`/`perf trace`/`/proc` (higher overhead or less detail) |
| `strace` | ptrace blocked | `perf trace`/eBPF tracepoints if available |
| `gdb` live attach | ptrace blocked | Core dump, `/proc/PID/stack`, `eu-stack`, runtime thread dump |
| Host perf on container symbols | Symbol resolution fails | `--symfs`, container rootfs, or `docker cp` the binary |
| Runtime API | Socket denied | `/proc` cgroup and namespace inspection (limited) |

A missing lens must be visible in the UI and the report, with the reason. Silent zeroes are forbidden.

---

## Part 2: The Universal Execution Model

### Phase A: Target Resolution

#### Inputs

- `--pid <pid>`: a host PID.
- `--container <name-or-id>`: a runtime container.
- `--cgroup <path>`: a cgroup scope.
- `--k8s pod/<name>` or `--k8s node/<name>`: a Kubernetes target (resolved on the node).
- `--launch -- <command> [args...]`: launch a command for record/replay or tracing from startup.
- `--duration`, `--interval`, `--output`, `--format`, `--json`, `--no-color`.

#### Resolution Rules

1. **PID input:** require a positive integer; reject PID 0 and negative values. Verify `/proc/<pid>` exists. Read the start time and executable. Build a `TargetSet` from the process plus its threads and, optionally, descendants.
2. **Container input:** call the runtime adapter. Extract the container ID, host PID, cgroup path, merged rootfs, image digest, and namespace list. If the container is stopped, fail with a clear error. Enumerate all processes from the cgroup rather than trusting the init PID alone.
3. **Cgroup input:** resolve `/sys/fs/cgroup` (v2) or the v1 hierarchy. Enumerate `cgroup.procs` and `cgroup.threads`. Verify the cgroup still exists before and after attach.
4. **Launch input:** create a child process in its own process group; record the PID/start time immediately; attach or trace from the first instruction where supported.
5. **Kubernetes input:** require node-level access. Map pod -> container ID -> runtime -> host PID/cgroup. Do not assume `kubectl exec` provides host visibility.
6. **Re-resolution:** on target exit, container restart, or PID mismatch, invalidate the target set and either stop or re-resolve according to policy. Never silently profile a recycled PID.

The normalized target record should be immutable for the duration of a run, except for the live process/thread set.

### Phase B: Engine Planning and Command Construction

#### Engine Interface

Every engine implements a common contract:

```text
Engine
  name, version
  lenses[]                 # resource, syscall, cpu, memory, io, net, concurrency, static
  signal_types[]           # metric, event, log, trace, profile
  attach_modes[]           # attach, launch, post-mortem
  requires[]               # root, CAP_*, BTF, PMU, tracefs, runtime socket
  overhead_class           # passive, low, moderate, high, stop-the-world
  supports_container       # none, host_target, namespaces, launch_in_container
  probe(environment) -> capability report
  plan(target, options) -> command/API plan
  start(plan, sink) -> session
  stop(session, mode) -> artifact + summary
  parse(stream) -> typed records
  validate(result) -> quality report
```

The orchestrator selects engines from the requested lenses and the user's risk/overhead budget. It must not start every engine by default.

#### Attach vs Launch

| Engine | Attach to running process | Launch from start | Notes |
|---|---|---|---|
| `/proc`/cgroup readers | Yes | Yes | Snapshot/poll; can miss events |
| `pidstat`, `lsof`, `ss` | Yes | Yes | Interval-based |
| `perf stat` | Yes | Yes | Counters; attach needs permissions |
| `perf record` | Yes | Yes | Sampling; attach can miss startup |
| `perf trace` | Yes | Yes | Lower overhead than `strace` |
| `strace` | Yes | Yes | Attach can miss startup; high overhead |
| `bpftrace`/BCC | Yes | Yes | Probe availability varies |
| `gdb` | Yes | Yes | Attach stops the process |
| `rr` | No | Yes | Record requires launch |
| `objdump`/`readelf` | Post-mortem | Post-mortem | Operates on a binary, not a live process |
| core dump | Yes (`gcore`) | Post-mortem | `gcore` stops the target temporarily |

Choosing launch over attach is important for short-lived processes and startup races. Choosing attach is important when the process is already in the failing state.

#### Namespace Injection

Use namespace injection only when the engine or its path resolution needs it:

- `strace -p $HOST_PID` from the host does not need `nsenter`.
- `perf report`, `perf annotate`, and `perf script` from the host can use `--symfs /proc/$HOST_PID/root` for container binaries. `perf record` itself does not accept `--symfs`; record with build IDs and symbolize at report time.
- `ss`/`tcpdump` need `-n` to see container sockets.
- A tool that reads `/etc/...` or a container-relative path needs `-m`.
- A process launched inside the PID namespace sees the container init as PID 1, not the host PID.

Correct pattern (smallest namespace set):

```bash
sudo nsenter -t "$HOST_PID" -m -u -i -n -p -- <command>
```

Use `-a` only when every namespace is required and the host/runtime allows it; it includes the user namespace and often fails on rootless or restricted containers.

#### Command Construction

- Use `execve`/`Command` with an argument vector; never concatenate a shell string.
- Validate PIDs as integers; validate container names against the runtime's namespace; resolve names through the API.
- Set `LC_ALL=C`, a deterministic `PATH`, and explicit timeouts.
- Put each engine in its own process group/session so signals and cleanup are reliable.
- Use pty only when a tool changes behavior on a TTY; otherwise use pipes and force line buffering.
- Never run untrusted binaries from a container rootfs. Extract only artifacts you intend to inspect.

### Phase C: Asynchronous Streaming and Supervision

1. Spawn the engine with separate `stdout`/`stderr` pipes.
2. Read both streams concurrently. Do not read one to completion before the other; a full stderr pipe can deadlock the child.
3. Set `LC_ALL=C` and handle partial lines, carriage returns, and final lines without newlines.
4. Handle tools that buffer when stdout is not a TTY (`stdbuf -oL`, tool-specific `-o`/`--line-buffered`, or a pty when unavoidable).
5. Parse incrementally into typed records. Keep a raw line for diagnostics and a parser version for reproducibility.
6. Push records into a bounded queue. Apply a drop/coalesce policy and count drops.
7. Label every record with source, timestamp, clock domain, target ID, and quality flags.
8. Supervise the child: exit code, signal, timeout, stderr tail, restart policy, and cleanup.
9. On stop, send the tool's graceful signal (for example, `SIGINT` to `strace` so it prints a summary), wait with a timeout, then terminate the process group if needed.
10. Reap children; never leave zombies. Ensure a failed engine cannot crash the orchestrator.
11. Check disk space and memory limits before writing perf.data, core dumps, or large traces.

### Phase D: Normalization, Correlation, and Provenance

Normalize every record into a shared model:

- **Identity:** `target_id`, `container_id`, host `pid`, `tid`, process start time, cgroup, command, namespace IDs.
- **Time:** `timestamp_ns` plus `clock_domain`; a conversion function to a display clock; no mixing of clocks without labeling.
- **Signal:** `kind` (metric/event/log/trace/profile), `name`, `value`, `unit`, `labels`.
- **Quality:** `supported`, `sampled`, `multiplexed`, `estimated`, `dropped`, `partial`, `error`.
- **Provenance:** engine, engine version, kernel, CPU, cgroup version, image digest, run ID.

Correlation keys:

- `(pid, start_time)` for process lifetime.
- `(pid, tid, start_time)` for thread lifetime.
- `(container_id, cgroup)` for container scope.
- `(timestamp, cpu, pid, tid)` for scheduler samples.
- `symbol + offset` or `build_id + address` for stack/assembly joins.

Cross-lens joins that should be supported:

- A syscall event -> thread stack at that time -> source line -> disassembly.
- A perf sample -> symbol -> source line -> assembly -> cache-line/annotation.
- A scheduler event -> thread state -> lock/off-CPU stack -> contended resource.
- An FD/socket event -> file path/connection -> container mount/network namespace -> host path.
- A cgroup pressure spike -> process/thread activity -> syscall/IO/network events in the same interval.

If clocks cannot be aligned precisely, show intervals and uncertainty rather than implying false precision.

---

## Part 3: The Core Five-Step Workflow

This is the minimum viable investigation. Each step includes the concept, the engines, and strict validation criteria.

### Step 1: High-Level Resource Verification

**Concept:** Establish what the target is and where it spends resources over time. Distinguish CPU-bound, I/O-bound, memory-pressure, and scheduling-limited behavior.

**Engines:** `/proc`, cgroup v2 files, `pidstat`, `lsof`, `ss`, optional `docker stats`/`systemd-cgtop`.

**Implementation:**

- Resolve the full target set (processes and threads).
- Poll resource counters at a configurable interval; store deltas and rates.
- Read FDs and sockets as state snapshots, not event streams.
- Read cgroup limits and pressure (`cpu.stat`, `memory.current/stat/events`, `io.stat`, `*.pressure`).
- Track FD growth, thread count, context switches, page faults, RSS/PSS, swap, and I/O delay.

**Validation criteria:**

- **FD accounting:** open 500 files in the target; the parsed count must increase by 500 plus known initial descriptors. Explain memory-mapped files and runtime descriptors rather than assuming they are leaks.
- **CPU-bound test:** a spin loop should show high CPU time and near-zero voluntary context switches; a sleep loop should show low CPU and high voluntary switches.
- **I/O test:** a cold random read should show major faults/block I/O; a warm-cache rerun should show a clear drop.
- **Cgroup test:** set a low CPU quota; the target should show `nr_throttled`/`throttled_usec` growth and increased run-queue/PSI pressure. The tool must report throttling rather than calling it "slow code".
- **Target lifecycle:** stop the target mid-run; the tool must mark the target as gone and must not reuse the PID without a start-time check.

### Step 2: System-Call Bifurcation

**Concept:** Separate user-space execution from kernel time and identify syscall-level bottlenecks: file opens, reads/writes, futex waits, event-loop sleeps, signal handling, and failed syscalls.

**Engines:** `strace` (exact, expensive), `perf trace` (lower overhead), eBPF (`syscount`, custom `bpftrace`), `ltrace` for library calls.

**Implementation:**

- Support attach and launch modes.
- Parse both full traces and summary mode.
- Use `-w` for wall-clock accounting and `-T` for per-call latency where available.
- Follow forks/threads.
- Detect failures (`EPERM`, `ENOENT`, `EAGAIN`) and group by syscall, command, and file/socket.
- Expose signal delivery and signal masks (`SigBlk`, `SigIgn`, `SigCgt` in `/proc/PID/status`), plus process lifecycle events (`fork`, `exec`, `exit`) so short-lived children are not missed.
- For containers, attach to the host PID; use namespace injection only for path/network-aware tooling.

**Validation criteria:**

- **Sleep test:** a process calling `sleep(1)` repeatedly must show most syscall wall time in `nanosleep`/`clock_nanosleep`, and the tool must distinguish syscall time from user CPU time.
- **Signal test:** send a signal to the target and confirm the signal event, delivery/ignored state, and handler behavior are visible. A signal blocked by `SigBlk` must not be reported as delivered.
- **Lifecycle test:** a target that forks a short-lived child must be observable in launch mode or through lifecycle tracepoints; attach-only polling must explicitly warn that it can miss it.
- **Lock test:** a contended mutex must produce a large `futex` share; correlate with `perf lock` or thread stacks. Do not claim lock contention from futex time alone.
- **Open-storm test:** a loop reopening a file per chunk must show high `openat` counts; a loop reusing a descriptor must not.
- **Parser test:** feed recorded outputs from at least two `strace` versions and locales; assert the same normalized numbers.
- **Permission test:** with `ptrace_scope=1` and no `CAP_SYS_PTRACE`, the engine must fail with an actionable error, not an empty table.

### Step 3: Hardware Profiling (The "Why")

**Concept:** Explain CPU behavior with PMCs: cycles, instructions, IPC, cache/TLB misses, branch misses, stalls, memory bandwidth, false sharing, and scheduler effects.

**Engines:** `perf stat`, `perf record`/`report`, `perf annotate`, `perf mem`, `perf c2c`, `perf sched`, `perf lock`, Intel PT/LBR where available, Callgrind for deterministic instruction counts.

**Implementation:**

- Probe supported events with `perf list`; never assume `-d` or named cache events exist.
- Prefer counting for comparisons and sampling for hotspots.
- Capture call graphs with frame pointers, DWARF, or LBR; record the method in provenance.
- For containers, record with host perf and build IDs, then use `perf report --symfs /proc/$HOST_PID/root` (not `perf record --symfs`) and cgroup scope where supported.
- Normalize counts by time/instructions and report scaling/multiplexing.
- Produce flame graphs and differential profiles from perf.data.
- Report unsupported events as unsupported, never as zero.

**Validation criteria:**

- **Cache test:** contiguous-array traversal vs randomized linked-list traversal must show a statistically significant difference in cache misses across repeated runs. Use medians and report variance; do not rely on a single pair.
- **Branch test:** a data-dependent unpredictable branch must show higher branch misses than a predictable loop.
- **Vectorization test:** `-O0` vs `-O3 -mavx2` must show vector instructions and a measurable instruction/cycle difference in the target function.
- **False-sharing test:** two threads writing adjacent independent counters must show cache-line contention in `perf c2c`; padding the counters must reduce it.
- **Scheduler test:** CPU pinning must reduce migrations and run-queue noise; the tool must show affinity effects.
- **Graceful fallback:** on a VM with no PMU, hardware events must be marked unsupported while software events and `/proc` data still work.

### Step 4: Concurrency and Determinism

**Concept:** Explain why threads are running, blocked, waiting, contending, or racing, and reproduce concurrency failures deterministically.

**Engines:**

- Live state: `/proc/PID/task/*`, `ps -L`, `top -H`, `eu-stack`, `gdb` thread backtraces.
- Scheduling: `perf sched`, off-CPU/wakeup/run-queue eBPF tools, PSI/cgroup throttling.
- Locking: `perf lock`, futex tracing, `strace -e futex`, lock statistics.
- Memory contention: `perf c2c`, `perf mem`.
- Race detection: ThreadSanitizer, Helgrind, DRD (instrumented runs).
- Deterministic replay: `rr` record/replay, `rr pack`; language-specific deterministic testers where available.
- Runtime concurrency: Go execution trace/schedtrace, JVM JFR/jstack, Python `py-spy`, Node inspector.

**Implementation:**

- Provide one-click thread dumps and a thread-state timeline.
- Join scheduler events to thread stacks and lock waits.
- Show per-thread on-CPU/off-CPU time, run-queue latency, migration, and involuntary context switches.
- Detect cgroup CPU throttling as a concurrency bottleneck.
- For `rr`, launch under record; `rr record --chaos` randomizes scheduling to expose races while replay remains deterministic. For live debugging, use `gdb` attach.
- If the workload is idle, apply controlled external load (for example, `stress-ng`) so scheduler and lock behavior are observable.
- For containers, attach via host PID; run `rr` in a dedicated debug container with minimal capabilities.

**Validation criteria:**

- **Deadlock test:** attach to a deadlocked multithreaded target; `thread apply all bt full` must show every blocked thread and its lock chain. The process must not need a restart.
- **Race test:** a race that fails intermittently natively must fail deterministically under `rr` record/replay. Document CPU/kernel compatibility.
- **Off-CPU test:** a thread blocked on a mutex must show off-CPU time and a futex wait; a thread waiting for CPU must show run-queue latency instead.
- **False-sharing test:** as in Step 3, confirm with `perf c2c` and a layout fix.
- **Throttling test:** a CPU-limited cgroup must show throttling and elevated run-queue latency, not just "high CPU".
- **Runtime test:** for Go/Java/Python/Node, the runtime-aware thread/task view must agree with OS thread state for a known blocking case.

### Step 5: Assembly, Source, and Static Analysis

**Concept:** Connect observed machine behavior to the actual instructions and source code: vectorization, spills, bounds checks, syscall thunks, PLT calls, and hot instructions.

**Engines:** `objdump`, `readelf`, `nm`, `addr2line`, `perf annotate`, `debuginfod`, `rizin`/`radare2`, optional Ghidra.

**Implementation:**

- Resolve the binary from the merged rootfs or `/proc/PID/root`; do not hardcode storage-driver paths.
- Keep build IDs, debug info, and source paths, or use `debuginfod`.
- Disassemble by symbol when available; fall back to address ranges for stripped binaries.
- Map perf samples to source lines and instructions; show hot instructions and branch behavior.
- For containers, symbolize with `perf report --symfs /proc/$HOST_PID/root` or extract the exact image binary; verify the build ID matches the running process.

**Validation criteria:**

- **Vectorization test:** `-O0` shows scalar instructions; `-O3 -mavx2` shows vector instructions in the target function.
- **Source mapping test:** `perf annotate` hotspots point to the expected source lines and match a manual `objdump` inspection.
- **Stripped-binary test:** the tool explains missing symbols and still offers address-level disassembly; it never fabricates function names.
- **Container test:** the binary extracted from the container matches the running process's build ID; a mismatch is reported, not ignored.

---

## Part 4: Extended Lenses

The five-step workflow covers the core loop. A serious observability platform also needs the following lenses, because "much more" is where root causes usually hide.

### 6. Memory Lens

**Questions:** Is memory growing? Is it a leak, fragmentation, cache growth, or a cgroup limit? Is the process swapping? Is NUMA placement bad? Is a shared page or huge page involved?

**Engines:** `/proc/PID/smaps_rollup`, `/proc/PID/status`, `pmap`, `smem`, cgroup `memory.*`, PSI, heaptrack, Valgrind Massif, BCC `memleak`, `perf mem`, `/proc/PID/numa_maps`.

**Validation:**

- A deliberate 50 MB allocation must increase RSS/PSS by approximately 50 MB (accounting for allocator and page-cache effects).
- A freed allocation should return or reuse memory; if RSS stays high, distinguish allocator retention from a leak with heaptrack/Massif.
- A cgroup memory limit must produce `memory.events`/OOM evidence, not just a disappeared process.
- NUMA tests must show placement changes when binding with `numactl`.

### 7. I/O and Filesystem Lens

**Questions:** Is the process doing buffered or direct I/O? Is it the block device, the filesystem, the overlay, or the page cache? Are there deleted-open files? Is io_uring involved?

**Engines:** `/proc/PID/io`, `pidstat -d`, `iotop`, `lsof`, BCC `biolatency`/`biosnoop`/`filetop`/`cachestat`, block/vfs/io_uring tracepoints, `strace -e trace=%file,%desc`.

**Validation:**

- `rchar/wchar` vs `read_bytes/write_bytes` must distinguish syscall traffic from block I/O.
- A cold read must show block I/O; a warm read should be served from cache.
- `lsof +L1` must find a deleted-but-open file created by the test.
- OverlayFS paths must be mapped to container-visible paths and vice versa.

### 8. Network Lens

**Questions:** Is the process blocked on a socket? Is the network namespace correct? Is there retransmission, connection churn, or a full accept queue?

**Engines:** `ss -tiepm`, `nstat`, `tcpdump`, `ip -s link`, `ethtool -S`, BCC `tcpconnect`/`tcpaccept`/`tcplife`/`tcpretrans`, network tracepoints, container netns.

**Validation:**

- A local client/server test must show the connection in `ss` with the right namespace and process.
- An injected packet loss/retransmit scenario must appear in `tcpretrans`/`ss -ti`.
- Container port mapping must resolve to the host-side socket and the container-side namespace correctly.

### 9. Scheduler and Off-CPU Lens

**Questions:** Is the process waiting for CPU, for a lock, for I/O, or for a wakeup? Is cgroup CPU throttling involved? Is the scheduler migrating threads?

**Engines:** `perf sched`, BCC `offcputime`/`wakeuptime`/`runqlat`/`runqlen`/`cpudist`, `/proc/PID/schedstat`, PSI, cgroup `cpu.stat`, `mpstat`.

**Validation:**

- A CPU-hog in a constrained cgroup must show throttling and run-queue latency.
- A mutex-blocked thread must show off-CPU time and a futex wait, not run-queue latency.
- A sleeper must show wakeup latency; a busy thread must show on-CPU time.

### 10. Hardware, NUMA, Power, and DMA Lens

**Questions:** Is the CPU throttling thermally or by power limits? Is memory bandwidth saturated? Is NUMA locality wrong? Are interrupts/softirqs stealing time? Is a device or DMA path the bottleneck?

**Engines:** `lscpu`, `lstopo`, `turbostat`, `cpupower`, `powertop`, RAPL, uncore/IMC perf events, `numastat`, `/proc/interrupts`, `/proc/softirqs`, `lspci -vv`, `/sys/class/iommu`, `/sys/class/dma`, vendor counters, `dma-buf` debugfs when available. For GPUs/accelerators, use vendor tools such as `nvidia-smi`/`nvtop`, Nsight Systems/Compute (`nsys`, `ncu`), `rocm-smi`, and `rocprof`.

**Validation:**

- `turbostat` must show frequency/thermal changes under load.
- A NUMA-bound workload must show different local/remote access behavior under `numactl`.
- Interrupt/softirq load must correlate with network or block-device activity.
- DMA is largely device-driven; the tool must document that it observes drivers, IOMMU, interrupts, memory bandwidth, and vendor counters, not a generic "DMA counter" that does not exist on most systems.
- A GPU/accelerator workload must be attributed through the vendor's counters/timeline; CPU-only profiling must not claim GPU time.

### 11. Security and Capability Lens

**Questions:** What can this process do? Is it confined by seccomp/AppArmor/SELinux? Which capabilities does it hold? Is it making denied syscalls? Is it accessing unexpected files or network endpoints?

**Engines:** `/proc/PID/status` (`CapEff`, `Seccomp`), `capsh --decode`, `getcap`, `aa-status`, `sestatus`, `auditd`, eBPF capable/seccomp probes, `strace` error paths, `lsof`, `ss`, `opensnoop`.

**Validation:**

- A process with a dropping capability set must show the correct effective/permitted sets.
- A seccomp-denied syscall must produce an audit/error event, not a silent success.
- A container with `--cap-drop=ALL` must show no unexpected effective capabilities.
- Never run this lens against untrusted targets without isolation; it can expose sensitive paths and credentials.

### 12. Timeline, Baseline, and Regression Lens

**Questions:** What happened first? Did the fix remove the bottleneck or move it? Is the change statistically real?

**Engines:** The platform's own event store, `perf diff`, differential flame graphs, `hyperfine`, saved syscall/thread/memory snapshots, cgroup and PSI time series.

**Validation:**

- An A/B benchmark must show a change larger than the run-to-run noise, with medians and spread.
- A timeline must place a syscall, a perf sample, a scheduler event, and a thread-state change in a defensible order.
- A regression report must identify the metric, the comparison baseline, and the environment; it must not compare across different CPU/kernel/cgroup settings without warning.

### 13. Kernel and System Logs Lens

**Questions:** Did the kernel OOM-kill the process, reject a syscall, log a segfault, report a driver/IOMMU error, or record an audit/security event?

**Engines:** `dmesg -T`, `journalctl -k`, `journalctl -u <service>`, `coredumpctl`, `auditd`/`ausearch`, AppArmor/SELinux logs, and container runtime logs. Correlate timestamps with the platform's event timeline.

**Validation:**

- An OOM test must produce a cgroup `memory.events` record and a kernel/runtime log entry, not just a vanished process.
- A deliberate segfault must produce a signal event and a core dump/coredumpctl entry when core dumps are enabled.
- A denied seccomp/capability operation must appear in the audit/security log path when audit is enabled.
- Log timestamps must be normalized to the same clock domain or displayed with an explicit offset.

---

## Part 5: Engine Catalog, Matrices, and Parsing

### 5.1 Overhead, Privilege, and Container Support

| Engine | Signal | Typical overhead | Privilege | Container strategy |
|---|---|---|---|---|
| `/proc`, cgroup | Metrics/state | Very low | Usually unprivileged | Host PID/cgroup reads |
| `ss`, `lsof` | State/events | Low | Some root-only | Host PID; netns for sockets |
| `pidstat` | Metrics | Low | Mostly unprivileged | Enumerate cgroup processes |
| `perf stat` | Metrics | Low | CAP_PERFMON/root | Host perf; `-a -G <cgroup>` for cgroup scope |
| `perf record` | Profiles | Moderate | CAP_PERFMON/root | Record with build IDs; report with `--symfs` |
| `perf c2c`/`mem` | Profiles | Moderate-high | Root/PMU support | Host perf; CPU/VM support required |
| `perf trace` | Events | Low-moderate | CAP_PERFMON/root | Host PID |
| `strace` | Events | Very high | CAP_SYS_PTRACE | Host PID; namespace only for paths |
| `bpftrace`/BCC | Events/metrics | Low-moderate | CAP_BPF/root | Host-side, cgroup/PID filters, uprobes/USDT |
| `gdb` | State | Stop-the-world | CAP_SYS_PTRACE | Host PID; sysroot/symbols |
| `rr` | Deterministic trace | High | CAP_SYS_PTRACE + perf policy | Dedicated debug container |
| Valgrind | Instrumented profile | Very high | Unprivileged | Run inside container or on copied binary |
| `objdump`/`readelf` | Static | None | Unprivileged | Extract binary or use merged rootfs |
| `tcpdump` | Packets | Moderate-high | CAP_NET_RAW | Netns for container traffic |

### 5.2 Machine-Readable Modes and Parser Strategy

Prefer structured output when a tool provides it:

- `perf stat -j` for JSON counters.
- `perf script -F ...` for machine-readable samples.
- `bpftrace -f json` where supported; otherwise define a stable output format in the script.
- `crictl`/`docker` `--format` with explicit fields.
- `ps -o` with a fixed column list.
- `ss -H`/`-O` where supported.
- Avoid parsing human-formatted tables when a structured mode exists.

When only text exists:

- Set `LC_ALL=C`.
- Pin the tool version and keep golden fixtures.
- Parse by header names, not fixed column positions, when possible.
- Handle units (`KiB`, `MiB`, `ms`, `us`, `%`, `K`, `M`), `<not supported>`, `<not counted>`, `<not available>`, and empty fields.
- Store the raw line with each parsed record for debuggability.
- Treat parser drift as a first-class failure: surface it, do not silently emit wrong numbers.
- Fuzz parsers with truncated, reordered, and localized inputs.

### 5.3 Provenance and Reproducibility

Every run should record:

- Platform version, engine versions, kernel version, CPU model/topology, governor, PMU availability, BTF/tracefs status.
- Container runtime, container ID, image digest, cgroup path, cgroup v1/v2, limits.
- Target command, build ID, debug info availability, start time.
- Affinity, nice value, cgroup limits, benchmark parameters, run duration.
- Dropped-event/lost-sample counts and parser warnings.

This metadata is what makes a comparison meaningful months later.

---

## Part 6: State, Data Model, and UI

### 6.1 Data Model

Use two complementary stores:

1. **Time-series metrics:** ring buffers or downsampled histories for resource counters, rates, and derived values.
2. **Discrete events:** append-only event records for syscalls, scheduler events, FD changes, logs, and samples. Retain bounded history and allow explicit export.

A normalized record should look like:

```text
Event {
  id, run_id, target_id, source, signal_kind,
  timestamp_ns, clock_domain,
  pid, tid, start_time, cgroup, container_id,
  kind, name, value, unit, labels,
  quality_flags, raw_ref, provenance
}
```

Do not store only rendered strings. The renderer must be able to produce a table, JSON, a timeline, or a flame graph from the same model.

### 6.2 Snapshot Semantics

The renderer must never see a partially updated data structure. Options:

- Immutable snapshot published atomically (`Arc`/reference-counted copy).
- Double buffering with a generation counter.
- Copy-on-write for large maps and event batches.
- A bounded ring buffer for high-rate events, with a separate latest-value map for gauges.

Avoid deep-copying every sample every 16 ms; that can dominate the tool's own CPU. Prefer immutable batches and cheap references. A "lock-free" design is not required for correctness; a well-scoped mutex or channel with bounded queues is often simpler and safer. Measure before optimizing.

### 6.3 Self-Observability

The platform must observe itself:

- CPU and memory usage of the orchestrator and each engine.
- Parser lag, queue depth, dropped records, and reconnects.
- Child process exit codes, signals, and stderr tails.
- Artifact sizes (perf.data, core dumps, traces) and disk headroom.
- Overhead estimate for the target based on the selected engines.

If the tool becomes the bottleneck, the investigation is invalid.

### 6.4 Renderers

**GUI (immediate mode):**

- Process tree and target selector.
- Overview dashboard: CPU, memory, I/O, network, context switches, cgroup pressure.
- Thread explorer and thread/state timeline with on-CPU/off-CPU bands.
- Syscall table and syscall timeline.
- Flame graph (on-CPU, off-CPU, memory, I/O).
- Memory map and FD/socket tables.
- Source/assembly/annotate view linked to samples.
- Timeline/correlation view and baseline comparison.
- Engine status and data-quality panel.

**CLI/TUI:**

- `--format table|json|ndjson|text`.
- `--duration`, `--interval`, `--output`.
- Non-interactive mode for CI with deterministic exit codes.
- Live dashboard only when attached to a TTY; plain output otherwise.
- Stable field names and units for machine consumers.
- No ANSI escapes when not a TTY or when `NO_COLOR` is set.

**Artifacts:**

- perf.data plus `perf report`/flame graph.
- JSON/NDJSON event dump.
- Perfetto/Chrome trace export for timeline viewing.
- Markdown/HTML report with evidence, environment, and warnings.
- Optional OTLP export for integration with existing observability backends.

### 6.5 UX Rules

- Every metric shows its source and timestamp.
- Every unsupported or dropped signal is visible.
- Every destructive or stop-the-world action requires confirmation when interactive.
- The user can start with the cheap lenses and opt into expensive ones.
- The UI never blocks on a slow engine; show a pending/error state instead.
- Filtering by process, thread, syscall, file, socket, and time range is global, not per-panel.

---

## Part 7: Testing and Validation Strategy

This is where most observability tools fail. The platform must prove correctness at three levels: parsing, integration, and real-world behavior.

### Level 1: Unit and Parser Tests

- Golden files from real tool outputs across versions/distributions.
- Mock subprocesses that emit known text, partial lines, CRLF, missing newlines, and errors.
- Assert exact typed values, units, and quality flags.
- Test failure modes: tool missing, permission denied, target exit, timeout, empty output, malformed output.
- Fuzz parsers and event normalization.
- Test PID/start-time validation and cgroup path parsing on cgroup v1 and v2 fixtures.
- Test locale independence (`LC_ALL=C`) and Unicode paths.

### Level 2: Integration Tests

- Spawn a local test process with known behavior: sleep loop, CPU spin, file open storm, mutex contention, memory allocation, socket client/server.
- Verify each engine yields the expected lens data.
- Verify graceful stop/cleanup, no zombies, no leaked FDs, no orphaned process groups.
- Verify engine failure does not corrupt the orchestrator or other engines.
- Verify container target resolution with a disposable container, including multi-process containers, rootless where possible, and cgroup v2.
- Verify `nsenter` PID semantics: inside the namespace the init process is PID 1.

### Level 3: End-to-End Benchmark (The Hasher)

- Build a standardized multi-threaded benchmark that reads a large file, hashes chunks, and writes the result.
- Run it natively and in a container.
- Verify:
  - FD counts and thread counts match the expected set.
  - Syscall summaries identify the expected file/futex/sleep behavior.
  - Cache/branch tests produce repeatable differences.
  - Thread dumps and `rr` reproduce a known deadlock/race.
  - Disassembly shows expected vectorization.
- **Container parity methodology:** do not demand an arbitrary 5% match on raw hardware counters. Compare the same binary, same affinity, same cgroup limits, same page-cache state, and multiple runs. Use medians and distributions. Explain differences caused by filesystem layering, runtime processes, and cgroup scheduling. For syscall counts and FD/thread state, exact or near-exact comparisons are meaningful; for PMCs, normalize and report variance.
- Verify that `--symfs`/merged-root symbolization resolves container frames; if not, the report must show raw addresses and a warning.

### Level 4: Overhead and Meta-Tests

- Measure target wall time and CPU time with no observer, with each engine individually, and with the recommended combination.
- Per-engine budgets are more honest than a single 10-15% claim:
  - `/proc`/cgroup/`ss`: low single-digit percent or less.
  - `perf stat`: low.
  - `perf record`: moderate and sampling-rate-dependent.
  - eBPF: probe-dependent; measure.
  - `strace`: inherently 10x-100x; never include it in a "low overhead" claim.
  - `gdb`: stop-the-world.
  - `rr`: high; use only for reproduction.
- Measure the platform's own overhead and dropped records.
- Warn when the user combines incompatible heavy engines.

### Level 5: Failure, Permission, and Portability Tests

- `perf_event_paranoid` values 1-4; `ptrace_scope` 0-3; missing capabilities; seccomp; kernel lockdown.
- No PMU (VM/cloud), missing BTF, missing tracefs/debugfs, old kernel, ARM vs x86 event differences.
- Tool version drift and missing tools.
- Container runtime variants: Docker, Podman, containerd, rootless, cgroup v1 vs v2, Kubernetes with/without shared PID namespace.
- Target exits during attach, PID reuse simulation, container restart.
- Locale, timezone, non-UTF-8 paths, very long command lines.
- Slow/blocked streams, full pipes, child writing to stderr only, child ignoring SIGINT.
- Disk full while writing perf.data/core dump.

### Level 6: Security and Privacy Tests

- Command injection attempts through container names/PIDs/paths.
- Malicious tool output with ANSI escapes or format strings; the renderer must not interpret untrusted text as control sequences.
- Trace/core artifacts containing secrets are not world-readable by default.
- Debug container capabilities are minimal and documented.
- The platform refuses to profile unauthorized/privileged targets by default or requires explicit confirmation.
- The TUI/GUI handles terminal escape sequences safely.

### Level 7: Long-Run and Regression Tests

- Multi-hour runs with bounded memory and disk usage.
- Target restarts, container restarts, and engine crashes during a run.
- Baseline comparison across code changes with statistical reporting.
- CI job that records a benchmark baseline and fails on a real regression above a configurable threshold, not on normal noise.

### Acceptance Checklist

- [ ] Container -> host PID -> cgroup -> all processes/threads is resolved, validated, and re-resolved on change.
- [ ] Every engine has a capability probe, timeout, graceful stop, and actionable error.
- [ ] Every metric carries source, timestamp, unit, and quality flags.
- [ ] Unsupported/dropped data is visible, never silently zero.
- [ ] Syscall, scheduler, perf, memory, I/O, and network events share a correlation model.
- [ ] A syscall/stack/sample can be joined to source/assembly when debug info exists.
- [ ] Container symbolization works via `--symfs` or merged root, with a clear fallback.
- [ ] `rr` and `gdb` paths are clearly separated: launch vs attach, deterministic vs live.
- [ ] Overhead is measured per engine and reported; heavy engines are opt-in.
- [ ] Parser golden tests cover multiple tool versions and locales.
- [ ] Permission and failure paths are tested, not just the happy path.
- [ ] Security/privacy review covers command construction, artifacts, and debug containers.
- [ ] CLI JSON/NDJSON schema is stable and documented.
- [ ] GUI/TUI never blocks on acquisition and never displays stale data without a timestamp.
- [ ] A known regression can be detected against a baseline with medians and variance.

---

## Part 8: Implementation Roadmap

### MVP: `/proc` and cgroup Truth

- Target resolution for PIDs and Docker/Podman; cgroup v2 process enumeration.
- Process/thread/FD/socket tables from `/proc`.
- Resource metrics (CPU, memory, I/O, context switches, page faults) with history.
- JSON/NDJSON output and a simple TUI.
- PID reuse/start-time validation, capability probing, graceful errors.

### v1: Tracing and CPU

- `strace` and `perf stat/record` engines with attach/launch modes.
- Syscall summaries/timelines, flame graphs, symbolization.
- `perf trace`/BCC optional backend.
- Container symbolization via report-time `--symfs` and namespace-aware path resolution.
- Overhead measurement and engine gating.

### v2: Concurrency, Memory, I/O, Network

- Scheduler/off-CPU/lock lenses, thread-state timelines.
- `perf c2c`, `perf mem`, cgroup pressure/PSI.
- Memory/IO/network engines and BCC probes.
- Core dump/thread dump integration and `rr` launch/replay workflow.
- Timeline/correlation view and artifact export.

### v3: Managed Runtimes, Kubernetes, and Automation

- Java/Python/Node/Go runtime adapters.
- Kubernetes/containerd adapters and node-level debugging.
- Baseline/regression mode for CI.
- OTLP/Perfetto export and optional agent-facing evidence bundles.
- Continuous profiling integration where appropriate.

---

## Part 9: Summary

Build a **state-driven orchestrator**, not a pile of shell wrappers:

- The kernel's host PID is the truth; a container is namespaces + cgroups + a rootfs, and its target is a set of processes/threads.
- Attach when possible, launch when necessary, and never pretend that observation is free.
- Decouple acquisition, parsing, normalization, correlation, and rendering with bounded queues and immutable snapshots.
- Treat data quality, provenance, and unsupported signals as part of the product.
- Expose lenses incrementally, with measured overhead and graceful fallbacks.
- Validate with known workloads, golden parsers, container parity, failure injection, and statistical comparisons.

If those principles hold, the choice of language, GUI toolkit, and storage backend is an implementation detail. Without them, a beautiful dashboard can still be confidently wrong.

---

## References

- Linux perf and perf security: <https://perf.wiki.kernel.org/>, <https://docs.kernel.org/admin-guide/perf-security.html>
- BPF performance tools: <https://www.brendangregg.com/bpf-performance-tools-book.html>
- bpftrace: <https://github.com/bpftrace/bpftrace>
- BCC: <https://github.com/iovisor/bcc>
- strace: <https://strace.io/>
- rr: <https://rr-project.org/>
- GDB: <https://sourceware.org/gdb/>
- systemd-coredump: <https://www.freedesktop.org/software/systemd/man/latest/systemd-coredump.html>
- Linux namespaces: <https://man7.org/linux/man-pages/man7/namespaces.7.html>
- cgroup v2: <https://docs.kernel.org/admin-guide/cgroup-v2.html>
- Linux tracing: <https://docs.kernel.org/trace/index.html>
- Docker runtime: <https://docs.docker.com/>
- Playground introduction video: <https://www.youtube.com/watch?v=qFEuYUfauNY>
