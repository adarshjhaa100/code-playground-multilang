### PART 1: The First Principles of the Architecture

To build a tool that observes processes and containers without altering them, you must understand the bedrock of the Linux operating system.

**1. The Kernel is the Single Source of Truth**
User-space abstractions like "Docker containers" do not exist to the CPU scheduler or the memory manager. A container is simply a standard Linux process (or group of processes) isolated by **Namespaces** (which hide global resources) and constrained by **Cgroups** (which limit resource usage). The kernel only recognizes the **Host OS PID**. Therefore, your tool must never attempt to profile a "container"; it must profile the Host PID that backs the container.

**2. The Observer Effect (Heisenberg’s Rule of Software)**
Measuring a system changes the system. If you use `docker exec` to run a profiler inside a container, the kernel must allocate a new process, set up new page tables, and schedule a new thread. This pollutes the L1/L2 cache and alters the thread scheduling of your target. 
*Rule:* You must attach to the existing execution context. When profiling a container, your tool must use OS-level namespace injection (e.g., `nsenter -t <HOST_PID> -a`) to force the profiler to inherit the target's exact namespaces, making the kernel treat the profiler as if it were already running inside the container.

**3. Strict Decoupling of Acquisition and Presentation**
Profiling tools (`strace`, `perf`) are blocking, long-running processes that emit high-volume text streams. If the thread responsible for drawing the UI waits for these processes, the UI will freeze.
*Rule:* Data acquisition (spawning subprocesses, reading pipes) and data presentation (rendering the UI or printing to CLI) must exist in completely separate, asynchronous execution contexts.

**4. The Unified Core (CLI + GUI)**
A unified CLI and GUI tool is not two separate programs. It is a single **Orchestrator Core** with two different **Renderers**. 
*   The **Core** resolves targets, spawns subprocesses, parses streams, and updates a thread-safe state.
*   The **GUI Renderer** reads that state and draws pixels using an Immediate Mode paradigm (`UI = f(State)`).
*   The **CLI Renderer** reads that state and prints formatted text/JSON to `stdout`.

---

### PART 2: The Universal Execution Model

Regardless of the language, your tool must implement this exact execution flow.

**Phase A: Target Resolution**
1. Accept input: A target identifier (PID or Container Name) and a boolean flag (`--container=true`).
2. If `--container=false`: Validate the input is a numeric PID. Verify `/proc/<PID>` exists. Set `Host_PID = Input`.
3. If `--container=true`: Query the container runtime (e.g., `docker inspect -f '{{.State.Pid}}' <name>`). Extract the integer. Set `Host_PID = Extracted_Integer`. If the result is 0 or empty, abort with an error.

**Phase B: Command Construction & Injection**
1. Define the base profiling command (e.g., `strace -c -f -p <Host_PID>`).
2. If `--container=true`, do **not** use container APIs to run the command. Instead, prepend the namespace injection wrapper: `nsenter -t <Host_PID> -a <base_command>`. 
3. *Exception for Step 4 (rr):* `rr` cannot attach to a running process; it must launch it. If `--container=true` for Step 4, the tool must construct a command that launches the container's binary directly via `nsenter` or by extracting the binary path from the container's overlay filesystem.

**Phase C: Asynchronous Streaming**
1. Spawn the constructed command as a subprocess.
2. Open `stdout` and `stderr` as non-blocking streams.
3. Spawn dedicated reader threads/coroutines for each stream.
4. Parse the raw text line-by-line into structured data objects.
5. Push these objects into a thread-safe, lock-free ring buffer or a mutex-protected state snapshot.

---

### PART 3: The 5-Step Observability Workflow & Validation

This is the core feature set of your tool. For each step, here is the conceptual implementation and the strict validation criteria to prove it works.

#### Step 1: High-Level Resource Verification
*   **Tools:** `pidstat`, `lsof`
*   **Concept:** Observe context switches, page faults, and file descriptor (FD) leaks at the kernel level.
*   **Implementation:** Run `pidstat -p <Host_PID> 1` and `lsof -p <Host_PID>`. Parse the context switch rate (`cswch/s`) and the count of open FDs.
*   **Validation Criteria:** 
    *   *Test:* Open 500 dummy files in the target process. 
    *   *Pass Condition:* The tool’s parsed FD count must exactly match `500 + standard_fds`. 
    *   *Test:* Force the target process into a tight spin-lock. 
    *   *Pass Condition:* `pidstat` context switch rate must remain near zero (proving it's CPU bound, not I/O bound).

#### Step 2: System Call Bifurcation
*   **Tools:** `strace -c -f`
*   **Concept:** Measure time spent in kernel space vs. user space. Identify lock contention (e.g., high `futex` time).
*   **Implementation:** Run `strace -c -f -p <Host_PID>`. Let it run for $N$ seconds, then send `SIGINT` to `strace` to force it to print the summary table. Parse the `% time` and `calls` columns.
*   **Validation Criteria:**
    *   *Test:* Make the target process call `sleep(1)` repeatedly.
    *   *Pass Condition:* The tool must report >90% of time spent in `nanosleep` or `clock_nanosleep`.
    *   *Test:* Introduce a highly contended mutex in the target.
    *   *Pass Condition:* The tool must show a massive spike in `futex` syscall time.

#### Step 3: Hardware Profiling (The "Why")
*   **Tools:** `perf stat -d`
*   **Concept:** Read Hardware Performance Counters (PMCs) to measure cache efficiency and branch prediction.
*   **Implementation:** Run `perf stat -d -p <Host_PID>`. Parse `L1-dcache-load-misses` and `branch-misses`. 
*   **Validation Criteria:**
    *   *Test:* Run the target with a cache-friendly workload (iterating a contiguous array) vs. a cache-unfriendly workload (traversing a randomized linked list).
    *   *Pass Condition:* The tool must show a statistically significant delta in `L1-dcache-load-misses` between the two runs, proving it is accurately reading the physical CPU silicon counters.

#### Step 4: Concurrency Determinism
*   **Tools:** `rr` (Record and Replay)
*   **Concept:** Record every syscall and hardware interrupt to deterministically replay race conditions.
*   **Implementation:** Unlike Steps 1-3, this step *launches* the target. The tool must execute `rr record <target_binary>`. Once finished, it must execute `rr replay` and expose a way to step through execution.
*   **Validation Criteria:**
    *   *Test:* Create a target with a deliberate, non-deterministic race condition (e.g., an unsynchronized counter incremented by 4 threads). Run it 100 times natively; it will fail randomly.
    *   *Pass Condition:* Run it 100 times via the tool's `rr record`. It must fail 100% of the time (or 0%, depending on the initial recording), proving `rr` has eliminated non-determinism.

#### Step 5: Assembly Inspection
*   **Tools:** `objdump` (or `rizin`/`radare2` for advanced control flow)
*   **Concept:** Verify compiler optimizations, specifically SIMD/Vectorization.
*   **Implementation:** Locate the physical binary. If `--container=true`, resolve the container's overlay filesystem path on the host (e.g., `/var/lib/docker/overlay2/<id>/merged/path/to/binary`) to avoid copying files. Run `objdump -d -M intel <binary_path>`. Parse the output for the target function.
*   **Validation Criteria:**
    *   *Test:* Compile a target function with `-O0` (no optimization) and then with `-O3 -mavx2` (AVX2 vectorization).
    *   *Pass Condition:* The tool's assembly view must show standard `mov` instructions for the `-O0` build, and vector instructions (e.g., `vmovdqa`, `vaddps`) for the `-O3` build.

---

### PART 4: State Management & UI Paradigm

To ensure the tool is maintainable and never freezes, you must adhere to the **Immediate Mode GUI (IMGUI)** paradigm.

**1. The State Snapshot**
The Orchestrator Core maintains a single, central `State` object. The acquisition threads write to this object. The rendering threads read from it. To prevent race conditions without killing performance, the Core should periodically (e.g., every 16ms) take a "snapshot" (a deep copy or an immutable reference) of the state and hand it to the renderer.

**2. The Rendering Loop (GUI)**
The GUI runs a loop at 30 to 60 FPS. On every single frame, it executes `Draw(State_Snapshot)`. 
*   You do not "update a text box" when a new `perf` metric arrives. 
*   You simply draw a text box containing the latest value from the `State_Snapshot`. 
*   If the state hasn't changed, you draw the exact same thing. This eliminates all widget-state synchronization bugs.

**3. The Rendering Loop (CLI)**
The CLI renderer operates on a timer or an event trigger (e.g., "State updated"). It reads the `State_Snapshot` and prints formatted tables or JSON to `stdout`. It clears the terminal (or uses ANSI escape codes to overwrite lines) to simulate a live dashboard in the terminal.

---

### PART 5: Comprehensive Testing & Validation Strategy

To prove your tool is robust, you must test it at three distinct levels.

**Level 1: Unit & Integration Testing (The Orchestrator)**
*   **Mock Subprocesses:** Do not test against real `perf` or `strace` initially. Create mock shell scripts that output hardcoded, known text.
*   **Validation:** Feed the mock output to your parser. Assert that the parser correctly extracts the exact integers and strings into your `State` object. This proves your text-parsing logic is flawless.

**Level 2: End-to-End Smoke Testing (The "Hasher" Benchmark)**
*   Create a standardized, multi-threaded benchmark binary (e.g., a Rust or C program that reads a 1GB file, hashes it in chunks using a thread pool, and writes it out).
*   **Validation:** Run your tool against this binary natively, and then run it against the binary packaged in a Docker container. The metrics (cache misses, syscall counts) must be within a 5% margin of error between the native and containerized runs. This proves your `nsenter` namespace injection is working perfectly and not introducing overhead.

**Level 3: Overhead & Performance Testing (The Meta-Test)**
*   An observability tool that slows down the target by 50% is useless.
*   **Validation:** Run the benchmark binary natively and measure its execution time. Then, run it while your tool is attached and measuring Steps 1, 2, and 3. 
*   **Pass Condition:** The execution time of the target must not increase by more than 10-15%. (Note: Step 2 `strace` will inherently cause massive slowdowns due to `ptrace`; this is a kernel limitation, not a bug in your tool, but Steps 1 and 3 using `/proc` and PMCs should have near-zero overhead).

### Summary of the Blueprint
You are building a **State-Driven Orchestrator**. It resolves containers to Host PIDs to satisfy the kernel's reality. It uses namespace injection to satisfy the Observer Effect. It decouples subprocess I/O from rendering to satisfy concurrency. And it uses Immediate Mode rendering to satisfy UI maintainability. If you build the tool strictly to these principles, the specific language or library you choose will merely be an implementation detail.
