# Zero to Architect Level — Multicore C++ Synchronization

A complete self-contained guide. Start here if you have never written multithreaded code. End here with a deep understanding of every synchronization primitive, tradeoff, and failure mode used in production multicore systems.

Every concept is explained from first principles, then shown in working code, then connected to a real benchmark in this project.

---

## Table of Contents

1. [Why Multicore? The Problem Statement](#1-why-multicore-the-problem-statement)
2. [What is a Thread?](#2-what-is-a-thread)
3. [The Race Condition — What Goes Wrong Without Synchronization](#3-the-race-condition--what-goes-wrong-without-synchronization)
4. [Mutex — The Basic Hammer](#4-mutex--the-basic-hammer)
5. [Coarse-Grained vs Fine-Grained Locking](#5-coarse-grained-vs-fine-grained-locking)
6. [Reader-Writer Lock — When Reads Dominate](#6-reader-writer-lock--when-reads-dominate)
7. [SpinLock — When the Wait is Short](#7-spinlock--when-the-wait-is-short)
8. [Atomics — The Right Tool for Simple Shared State](#8-atomics--the-right-tool-for-simple-shared-state)
9. [Memory Ordering — The Most Misunderstood Topic](#9-memory-ordering--the-most-misunderstood-topic)
10. [Cache Lines, False Sharing, and Cache-Line Ping-Pong](#10-cache-lines-false-sharing-and-cache-line-ping-pong)
11. [Per-Thread Sharding — Eliminate Shared State Entirely](#11-per-thread-sharding--eliminate-shared-state-entirely)
12. [Barriers, Latches, and Semaphores (C++20)](#12-barriers-latches-and-semaphores-c20)
13. [Condition Variables — Waiting for a Condition](#13-condition-variables--waiting-for-a-condition)
14. [The Hot Path — Finding What to Optimize](#14-the-hot-path--finding-what-to-optimize)
15. [The Optimization Workflow — Step by Step](#15-the-optimization-workflow--step-by-step)
16. [Measuring: perf, ThreadSanitizer, helgrind, perf-c2c](#16-measuring-perf-threadsanitizer-helgrind-perf-c2c)
17. [Common Bugs and How to Recognize Them](#17-common-bugs-and-how-to-recognize-them)
18. [Everything Connected — The celeris Story](#18-everything-connected--the-celeris-story)
19. [Interview Questions and Model Answers](#19-interview-questions-and-model-answers)

---

## 1. Why Multicore? The Problem Statement

Modern CPUs do not get faster by increasing clock speed — they get faster by adding cores. A 2024 server chip has 128 cores, each capable of executing independently. If your program uses only one, you are wasting 127/128 of the machine.

But parallel execution introduces a new class of problem: **shared state**. When two threads both read and write the same memory, the result depends on which one runs first. This ordering is not deterministic — it changes every time you run the program.

The entire field of concurrent programming is about answering one question:

> **How do you let multiple threads make progress simultaneously while keeping shared state correct?**

The answer is always a tradeoff between three things:

```
           ┌────────────┐
           │ Correctness │  — results are always right
           └─────┬───────┘
                 │    you can have any two; the art is
          ┌──────┴──────┐  getting all three
          ▼             ▼
   ┌────────────┐  ┌──────────────┐
   │Performance │  │  Simplicity  │  — code is easy to reason about
   └────────────┘  └──────────────┘
```

This guide shows exactly how to navigate this triangle, from the safest (slow, simple) tools to the fastest (correct only if you understand the rules) tools.

---

## 2. What is a Thread?

A thread is an independent sequence of instructions sharing the same process memory. Every thread has its own:
- **Stack** — local variables, function call frames
- **Registers** — CPU state (instruction pointer, general-purpose registers)
- **Stack pointer** — where the current stack frame lives

Every thread shares with all other threads in the process:
- **Heap** — `malloc`/`new` allocations
- **Global/static variables**
- **File descriptors, signals**

```cpp
#include <thread>
#include <iostream>

void task(int id) {
    // This function runs independently on its own thread
    std::cout << "Thread " << id << " running\n";
}

int main() {
    std::thread t1(task, 1);   // start thread 1
    std::thread t2(task, 2);   // start thread 2
    // Both t1 and t2 run concurrently with main()

    t1.join();  // wait for t1 to finish
    t2.join();  // wait for t2 to finish
    return 0;
}
// Output order is non-deterministic — could be 1,2 or 2,1 or interleaved
```

**C++20 `std::jthread`** — auto-joining thread (join happens in destructor):
```cpp
{
    std::jthread t(task, 1);
    // t automatically joins here when it goes out of scope
}
// No need to call t.join() — can't forget it
```

**Why `std::thread` can deadlock if you forget `join()`:**
If a `std::thread` object is destroyed without being joined or detached, the program calls `std::terminate()`. `std::jthread` fixes this by joining in its destructor.

---

## 3. The Race Condition — What Goes Wrong Without Synchronization

A race condition is when the result of a program depends on the non-deterministic scheduling order of threads.

```cpp
// BROKEN: two threads increment the same counter
int counter = 0;

void increment_1000() {
    for (int i = 0; i < 1000; ++i) {
        ++counter;   // THIS IS NOT ATOMIC
    }
}

int main() {
    std::thread t1(increment_1000);
    std::thread t2(increment_1000);
    t1.join(); t2.join();

    std::cout << counter << "\n";  // Expected: 2000. Actual: could be anything
}
```

**Why `++counter` is not atomic.** At the assembly level, `++counter` is three instructions:
```
MOV EAX, [counter]   ; 1. load current value into register
ADD EAX, 1           ; 2. increment in register
MOV [counter], EAX   ; 3. write back to memory
```

If Thread A and Thread B both execute step 1 before either executes step 3:
```
Thread A: load counter=5
Thread B: load counter=5    ← both see 5
Thread A: increment → 6
Thread B: increment → 6
Thread A: store 6           ← counter is now 6
Thread B: store 6           ← LOST INCREMENT: should be 7
```

This is called a **lost update**. The fix is to ensure the load-modify-store is **atomic** — indivisible, appearing to happen in a single instant from all other threads' perspectives.

---

## 4. Mutex — The Basic Hammer

A mutex (mutual exclusion lock) is the simplest synchronization primitive. Only one thread can hold it at a time. Any other thread trying to lock it will **block** (sleep) until the holder unlocks.

```cpp
#include <mutex>

std::mutex mtx;
int counter = 0;

void increment_safe_1000() {
    for (int i = 0; i < 1000; ++i) {
        std::lock_guard<std::mutex> lk(mtx);  // lock on entry
        ++counter;
        // lk destructor unlocks when it goes out of scope (RAII)
    }
}

int main() {
    std::thread t1(increment_safe_1000);
    std::thread t2(increment_safe_1000);
    t1.join(); t2.join();
    std::cout << counter << "\n";  // Always exactly 2000
}
```

**How a mutex works internally (simplified):**
```
mutex state word: 0 = unlocked, 1 = locked

lock():
    CAS(state, 0, 1)    // try to atomically set 0→1
    if CAS succeeded: we own the mutex, proceed
    if CAS failed:    the mutex is already held
        futex(WAIT)   // ask OS to sleep until state changes
        retry

unlock():
    state = 0
    if any threads are waiting:
        futex(WAKE)   // ask OS to wake one waiter
```

The `futex` syscall (Linux) / `ulock` (macOS) involves the OS kernel. This is why mutex lock/unlock costs ~10–30ns even with zero contention — the kernel path is always there.

**RAII lock wrappers:**

```cpp
// lock_guard: lock on construction, unlock on destruction (no manual unlock)
{
    std::lock_guard<std::mutex> lk(mtx);
    // critical section
}  // automatically unlocked here

// unique_lock: same as lock_guard but can unlock early / defer lock
{
    std::unique_lock<std::mutex> lk(mtx);
    // critical section
    lk.unlock();   // can unlock early
    // non-critical work
    lk.lock();     // re-lock if needed
}

// scoped_lock: locks MULTIPLE mutexes simultaneously, deadlock-safe
{
    std::scoped_lock lk(mtx_a, mtx_b);   // both locked atomically
    // safe: no ABBA deadlock possible
}
```

**The fundamental cost of mutex:** correctness via serialization. While one thread is in the critical section, all others wait. If the critical section is the hot path — called millions of times per second — every thread is paying lock overhead even when there's no actual contention.

---

## 5. Coarse-Grained vs Fine-Grained Locking

This is about **how many locks you use**, not which primitive you use.

### Coarse-grained: one lock for everything

```cpp
// A simulation engine with ONE global mutex
struct CoarseEngine {
    std::mutex global_lock;

    std::map<int, Signal> signals;
    std::vector<Process>  processes;
    std::queue<Event>     event_queue;

    // Every operation takes the same lock
    void write_signal(int id, int value) {
        std::lock_guard lk(global_lock);   // BLOCKS all other ops
        signals[id].value = value;
    }

    Signal read_signal(int id) {
        std::lock_guard lk(global_lock);   // BLOCKS writes AND other reads
        return signals[id];
    }

    void push_event(Event e) {
        std::lock_guard lk(global_lock);   // BLOCKS signal ops too
        event_queue.push(e);
    }
};
```

**Problem:** Thread A writing signal 3 and Thread B reading signal 7 have **zero data dependency** — they touch completely different memory. But they serialize on the same lock. Adding more threads makes it worse, not better — more threads = more lock contention = more time blocked.

### Fine-grained: one lock per resource

```cpp
// The same engine with per-resource locks
struct FineEngine {
    std::vector<std::shared_mutex> signal_locks;   // one per signal
    std::mutex event_queue_lock;                   // only for the queue

    std::vector<Signal> signals;
    std::queue<Event>   event_queue;

    void write_signal(int id, int value) {
        std::unique_lock lk(signal_locks[id]);     // only blocks signal id
        signals[id].value = value;
    }

    Signal read_signal(int id) {
        std::shared_lock lk(signal_locks[id]);     // shared: concurrent reads OK
        return signals[id];
    }

    void push_event(Event e) {
        std::lock_guard lk(event_queue_lock);      // independent of signal ops
        event_queue.push(e);
    }
};
```

**Result:** Thread A writing signal 3 and Thread B reading signal 7 now run in **parallel** with no contention. Thread C pushing an event also runs in parallel with both.

### When fine-grained is worse

Fine-grained locking has overhead: constructing N mutex objects, memory for each one, potential false sharing between adjacent mutexes in a vector. At very low thread counts or very small workloads, this overhead exceeds the contention savings.

**This project measures it:** Experiment 1 shows the crossover point. At 3 threads and 8 signals the fine-grained improvement is 2.15×. At 32 threads and 10,000 signals it would be much larger.

### Deadlock with fine-grained locks

The more locks you have, the more deadlock opportunities. Classic ABBA deadlock:

```cpp
// Thread A:               Thread B:
lock(signal_locks[0]);     lock(signal_locks[1]);
lock(signal_locks[1]);     lock(signal_locks[0]);  // DEADLOCK
//    ↑ waits for B              ↑ waits for A
```

**Fix: always acquire locks in a consistent order (ascending ID):**
```cpp
void swap_signals(int id_a, int id_b) {
    int lo = std::min(id_a, id_b);
    int hi = std::max(id_a, id_b);
    std::scoped_lock lk(signal_locks[lo], signal_locks[hi]);  // always lo then hi
    std::swap(signals[lo], signals[hi]);
}
```

Or use `std::scoped_lock` which handles multiple mutexes with deadlock avoidance built in.

---

## 6. Reader-Writer Lock — When Reads Dominate

In many systems, reads are far more frequent than writes. A `std::shared_mutex` allows:
- **Multiple readers simultaneously** (shared lock)
- **One writer exclusively** (unique lock — no readers allowed)

```cpp
std::shared_mutex rw;
int signal_value = 0;

// Reader thread — many can run at once
void reader() {
    std::shared_lock lk(rw);       // shared: concurrent readers OK
    int v = signal_value;          // read
}

// Writer thread — exclusive access
void writer(int new_val) {
    std::unique_lock lk(rw);       // unique: all readers must finish first
    signal_value = new_val;        // write
}
```

**When to use:**
- Signal values in a simulator: 1 writer (the driving process), N readers (sensitized processes)
- Configuration data read by all threads, written only at startup
- Cache / lookup tables: frequent reads, occasional updates

**When NOT to use:**
- When writes are as frequent as reads (upgrade cost exceeds benefit)
- When the critical section is so short (< ~5 instructions) that atomic is simpler
- When you have very few threads (overhead of shared_mutex > benefit)

**Performance note:** `std::shared_mutex` is heavier than `std::mutex` — it maintains a reader count and writer-pending flag. An uncontested `shared_lock` is still significantly cheaper than an `unique_lock` because it doesn't need to block any other thread.

---

## 7. SpinLock — When the Wait is Short

A spinlock does NOT go to sleep when it fails to acquire — it **busy-waits** (loops, checking again and again) until the lock is free.

```cpp
#include <atomic>

class SpinLock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
public:
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // spin: keep trying
            // _mm_pause() is a CPU hint for spin-wait loops:
            // tells the CPU "I'm spinning, not stalling" → better power/perf
            __builtin_ia32_pause();
        }
    }
    void unlock() {
        flag_.clear(std::memory_order_release);
    }
};
```

**Exponential backoff** — prevent cache-line thrashing by backing off:
```cpp
void lock() {
    int backoff = 1;
    while (flag_.test_and_set(std::memory_order_acquire)) {
        for (int i = 0; i < backoff; ++i)
            __builtin_ia32_pause();
        backoff = std::min(backoff * 2, 1024);   // cap at 1024 pauses
    }
}
```

**When spinlock beats mutex:**
- Critical section is **very short** (< ~100ns): sleeping and waking costs more than spinning
- Low contention: most of the time the lock is free; the spin loop rarely iterates
- Known wait time is bounded and short

**When mutex beats spinlock:**
- Critical section is long: spinning wastes CPU cycles that another thread could use
- High contention: spinning threads steal CPU from the thread holding the lock
- Unknown wait time: could be arbitrarily long

**In this project:** `SpinLock` is used in `TimeWheel` (per-bucket) and `DeltaQueue` (insert/drain). Each bucket hold time is O(1) — push to a vector — so spinning is correct.

---

## 8. Atomics — The Right Tool for Simple Shared State

An atomic operation is **indivisible** — it appears to happen in a single instant from all other threads' perspectives. No thread can observe a partial state.

```cpp
#include <atomic>

std::atomic<int> counter{0};

// CORRECT: fetch_add is a single atomic RMW instruction (LOCK XADD on x86)
void increment() {
    counter.fetch_add(1, std::memory_order_relaxed);
}

// Also available:
counter.store(42, std::memory_order_release);          // atomic write
int v = counter.load(std::memory_order_acquire);       // atomic read
counter.exchange(99, std::memory_order_acq_rel);       // atomic swap
counter.fetch_sub(1, std::memory_order_relaxed);       // subtract
counter.fetch_and(0xFF, std::memory_order_relaxed);    // bitwise AND
counter.fetch_or(0x01, std::memory_order_relaxed);     // bitwise OR
```

**Compare-and-swap (CAS)** — the universal building block:
```cpp
// compare_exchange_strong(expected, desired, success_order, fail_order)
// IF *this == expected: set *this = desired, return true
// ELSE: set expected = *this, return false

std::atomic<int> state{0};

// Atomically transition 0 → 1 (set a flag exactly once)
int expected = 0;
if (state.compare_exchange_strong(expected, 1,
        std::memory_order_acq_rel,
        std::memory_order_relaxed)) {
    // we won the race — we set it to 1
} else {
    // expected now holds the actual current value
    // someone else already set it
}
```

**`compare_exchange_weak` vs `compare_exchange_strong`:**
- `weak`: can fail spuriously (returns false even when `*this == expected`) — must loop
- `strong`: never fails spuriously — correct for one-shot use
- `weak` in a loop is slightly faster on some architectures (LL/SC hardware)

```cpp
// weak CAS in a retry loop (typical for implementing lock-free structures):
int old_val = counter.load(std::memory_order_relaxed);
while (!counter.compare_exchange_weak(old_val, old_val + 1,
        std::memory_order_release, std::memory_order_relaxed))
    ;   // old_val is updated to current value on failure — retry
```

**`std::atomic_flag`** — the most primitive atomic:
```cpp
// The ONLY C++ atomic type guaranteed lock-free on ALL platforms
// test_and_set(): atomically sets flag, returns PREVIOUS value
// LOCK XCHG on x86 — single instruction

std::atomic_flag flag = ATOMIC_FLAG_INIT;   // starts clear (false)

if (!flag.test_and_set(std::memory_order_acquire)) {
    // we cleared → set it; we are the first (or flag was cleared for us)
}

flag.clear(std::memory_order_release);       // reset for next round
```

**C++20: `atomic::wait()` and `notify_all()`** — block until value changes, without a condition variable:
```cpp
std::atomic<int> signal_value{0};

// In waiter thread:
signal_value.wait(0, std::memory_order_acquire);
// Blocks until signal_value != 0. Uses OS futex internally — efficient.

// In writer thread:
signal_value.store(1, std::memory_order_release);
signal_value.notify_all();   // wake all waiters
```

This replaces the classic `mutex + condition_variable + while(!pred)` pattern with a single atomic and two calls.

---

## 9. Memory Ordering — The Most Misunderstood Topic

Modern CPUs and compilers **reorder** instructions for performance. From the perspective of a single thread this is invisible (the CPU preserves the illusion of sequential execution for that thread). But from another thread's perspective, you can observe reorderings.

```cpp
// Thread A:            Thread B:
x = 1;                 if (ready) {
ready = true;              assert(x == 1);  // Can this fail?
                       }
```

On a strongly-ordered architecture (x86) with a sequentially-consistent compiler, this is safe. But on ARM/POWER (weakly-ordered), or with compiler optimizations (`-O2`), the writes to `x` and `ready` can be reordered — Thread B can see `ready == true` but still read `x == 0`.

Memory orderings tell the compiler and CPU what reorderings are allowed.

### The six orderings (from weakest to strongest)

```cpp
// memory_order_relaxed
// No ordering guarantees relative to other operations.
// Only guarantees: the operation itself is atomic (no torn reads/writes).
// Use: counters where you only need the final value, not ordering.
counter.fetch_add(1, std::memory_order_relaxed);   // correct for simple counting

// memory_order_acquire  (load side of release-acquire)
// All reads/writes AFTER this load in this thread are ordered AFTER the load.
// "I will see all writes that happened before the matching release store."
int v = flag.load(std::memory_order_acquire);

// memory_order_release  (store side)
// All reads/writes BEFORE this store in this thread are ordered BEFORE the store.
// "All my previous writes are visible to anyone who does an acquire load."
flag.store(1, std::memory_order_release);

// memory_order_acq_rel  (for read-modify-write operations)
// Combines acquire + release. Used for CAS, fetch_add when ordering matters.
bool ok = state.compare_exchange_strong(exp, val, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed);

// memory_order_seq_cst  (the default, strongest)
// Total global ordering: all seq_cst operations appear in the same order
// to all threads. Safest but most expensive (full memory fence).
counter.fetch_add(1);   // implicitly seq_cst

// memory_order_consume  (deprecated in practice)
// Weaker than acquire; data-dependency ordering only. Avoid.
```

### The release-acquire handshake pattern

This is the most important pattern to understand:

```cpp
// Correct producer-consumer with release-acquire:
std::atomic<bool> data_ready{false};
int data = 0;

// Producer thread:
data = 42;                                        // (1) write data
data_ready.store(true, std::memory_order_release); // (2) signal — release

// Consumer thread:
while (!data_ready.load(std::memory_order_acquire)); // (3) wait — acquire
int v = data;                                         // (4) read data
// GUARANTEED: (4) sees data=42 because release(2) synchronizes-with acquire(3)
// which means all writes before (2) are visible after (3).

// WITHOUT proper ordering:
// data = 42 could be reordered AFTER data_ready = true on some CPUs
// Consumer sees data_ready=true but data=0 → undefined behavior
```

### Rule of thumb for choosing memory order

```
Does the operation need to synchronize other memory with another thread?
  YES → use release (store) / acquire (load) / acq_rel (RMW)
  NO, just need the atomic operation itself to be atomic → use relaxed

Is this a one-time initialization / flag? → release/acquire pair
Is this a counter you only read at the end? → relaxed everywhere
Is this a CAS that's the "gate" to a critical section? → acq_rel / acquire
Unsure? → seq_cst (correct but slower; optimize after profiling)
```

### Memory fences

```cpp
// Explicit fence: create a barrier without an atomic operation
std::atomic_thread_fence(std::memory_order_release);  // all prior writes committed
std::atomic_thread_fence(std::memory_order_acquire);  // all subsequent reads see prior releases
std::atomic_thread_fence(std::memory_order_seq_cst);  // full barrier

// On x86, release/acquire fences are often free (x86 is strongly-ordered).
// On ARM, they emit DMB instructions.
```

---

## 10. Cache Lines, False Sharing, and Cache-Line Ping-Pong

### The memory hierarchy

```
CPU Core 0          CPU Core 1          CPU Core 2
   ↕                   ↕                   ↕
L1 Cache (32KB)    L1 Cache (32KB)    L1 Cache (32KB)    ~1ns
   ↕                   ↕                   ↕
L2 Cache (256KB)   L2 Cache (256KB)   L2 Cache (256KB)   ~4ns
   ↕                   ↕                   ↕
        L3 Cache (shared, 8-32MB)                        ~10ns
                    ↕
             Main Memory (DRAM)                           ~60ns
```

**Cache lines:** Memory is transferred between levels in chunks of **64 bytes** (on x86). When any byte in a 64-byte cache line is accessed, the whole line is loaded into L1. When any byte is modified, the whole line must eventually be written back.

### MESI protocol — how cores coordinate

Each cache line is in one of four states:

```
Modified  (M): this core owns the line, has modified it, not yet in memory
Exclusive (E): this core owns the line, not modified, memory is up to date
Shared    (S): multiple cores have a read-only copy
Invalid   (I): this core's copy is stale
```

**What happens on a write when another core has the line:**
```
Core 0 writes to address X:
  1. Core 0 broadcasts "I want to write X" (RFO: Request For Ownership)
  2. Core 1 (which has X in Shared state) must invalidate its copy → I
  3. Core 0 gets exclusive ownership → M state
  4. Core 0 writes

If Core 1 now wants to read X:
  1. Core 1 sees X is Invalid
  2. Core 1 requests X from Core 0 (cache-to-cache transfer)
  3. Core 0 must flush its Modified line → memory or directly to Core 1
  4. Both go to Shared state
```

This cache-to-cache transfer is "cache-line ping-pong". It costs ~40–100ns per transfer — much more expensive than a cache hit (~1ns) or even a main memory access (~60ns, which is sequential) because it requires inter-core synchronization.

### False sharing

```cpp
// BROKEN: two independent counters on the same cache line
struct Counters {
    long long a;   // bytes 0-7
    long long b;   // bytes 8-15
    // both a and b are on the SAME 64-byte cache line
};

Counters c;
// Thread 0 writes c.a → invalidates the line → Thread 1's copy of c.b becomes Invalid
// Thread 1 writes c.b → has to get the line from Thread 0 first
// They are fighting over a cache line even though they touch different variables
```

```cpp
// FIXED: cache-line padding
struct alignas(64) PaddedCounter {
    long long value;
    char pad[64 - sizeof(long long)];   // fill the rest of the 64-byte line
};

PaddedCounter counters[NUM_THREADS];
// Thread 0 writes counters[0].value → its own cache line → Thread 1 unaffected
// Thread 1 writes counters[1].value → its own cache line → Thread 0 unaffected
```

### Cache-line ping-pong with atomics

Even with `std::atomic`, if multiple cores write to the same atomic variable, the cache line containing it bounces between cores:

```
Core 0: fetch_add(counter) → Core 0 gets Modified ownership
Core 1: fetch_add(counter) → Core 0's line is invalidated; Core 1 gets Modified
Core 2: fetch_add(counter) → Core 1's line is invalidated; Core 2 gets Modified
...
```

This is sequential — only one core can be in Modified state at a time. Your `std::atomic<int>` is effectively serialized at the hardware level regardless of how many threads are calling it. This is why Experiment 4 showed `atomic fetch_add` is only 10× faster than `mutex`, not 3000× — the cache coherence protocol is the bottleneck, not the mutex syscall.

### Measuring false sharing with `perf c2c`

```bash
# Record cache-to-cache (c2c) events:
perf c2c record -ag -- ./build/sim

# Report: shows which cache lines are "hot" (frequently transferred)
perf c2c report

# Output includes:
# - Which lines have high LLC (Last Level Cache) load misses
# - Which source lines access those hot lines
# - How many cross-core hits (= ping-pong events)
```

A high "Remote HITM" count in `perf c2c` output means a cache line is being written by one core while another core needs it — the definitive sign of cache-line ping-pong or false sharing.

---

## 11. Per-Thread Sharding — Eliminate Shared State Entirely

The best way to handle a hot shared variable is to make it not shared. Each thread owns its own copy; they are merged once when needed.

```cpp
// BEFORE: one global counter, all threads contend
std::atomic<long long> total_events{0};

// Hot path (called millions of times per second):
void process_event() {
    // ...event processing...
    total_events.fetch_add(1, std::memory_order_relaxed);  // contended!
}

// ─────────────────────────────────────────────────────

// AFTER: per-thread shard, merged at barrier time
struct alignas(64) EventShard {
    long long count{0};
    char pad[64 - sizeof(long long)];  // isolate on its own cache line
};

std::array<EventShard, MAX_THREADS> event_shards{};

// Hot path — purely local write, zero coherence traffic:
void process_event(int thread_id) {
    // ...event processing...
    ++event_shards[thread_id].count;   // local cache line, no contention
}

// At barrier (once per delta cycle, not on hot path):
long long read_total() {
    long long total = 0;
    for (auto& s : event_shards) total += s.count;
    return total;
}
```

**The tradeoff:**
- Hot path: **zero synchronization** — pure local write at full L1 speed
- Reading current total: must sum all shards — O(num_threads), not O(1)

This is the right pattern when **writes >> reads**. Simulation event counters are updated once per event (millions of times) and read only at the end of a run (once). Sharding is the obvious choice.

**Applied to celeris's Profiler:**
The current `Profiler` uses `std::atomic<uint64_t>` for all counters. At 32 cores with millions of events per second, `total_events.fetch_add()` becomes the hottest cache line. The improvement: replace with `std::array<PaddedCounter, 32> per_thread_events`, sum in `DeltaBarrier::on_delta_complete()` (runs once per delta, not once per event).

---

## 12. Barriers, Latches, and Semaphores (C++20)

### `std::barrier` — synchronize N threads, repeatable

A barrier makes N threads wait until all N have arrived, then releases all of them simultaneously. Optionally runs a completion function on the last arriving thread.

```cpp
#include <barrier>

int num_workers = 4;

// Completion function: runs ONCE per phase, on the LAST arriving thread
// Must be noexcept
std::barrier sync(num_workers, []() noexcept {
    // flip double buffers, advance simulation time, check termination, etc.
    // All other threads are blocked in arrive_and_wait() while this runs
});

void worker(int id) {
    while (true) {
        do_phase_work(id);         // each worker does its part
        sync.arrive_and_wait();    // wait for all workers to finish
        // completion fn has run; all workers released simultaneously
        if (should_stop()) break;
    }
}
```

**Why it replaces hand-rolled barriers:**
```cpp
// HAND-ROLLED (fragile, 25+ lines):
std::mutex mtx;
std::condition_variable cv;
int arrived = 0;
int generation = 0;   // needed to fix a race condition

void arrive_and_wait_manual() {
    std::unique_lock lk(mtx);
    int gen = generation;
    ++arrived;
    if (arrived == num_workers) {
        arrived = 0;
        ++generation;
        do_completion();
        cv.notify_all();
    } else {
        cv.wait(lk, [&]{ return generation != gen; });
        // BUG if you use 'arrived==0' instead: fast threads re-enter
        // next phase and bump arrived_ before slow threads check the predicate
    }
}

// C++20 (1 line, correct by construction):
sync.arrive_and_wait();
```

**`std::barrier` phases:** each call to `arrive_and_wait()` completes one "phase". The barrier automatically resets for the next phase — it is reusable, unlike `std::latch`.

### `std::latch` — one-shot countdown gate

A latch counts down from N to 0 once. When it reaches 0, all waiters are released and it cannot be reset.

```cpp
#include <latch>

std::latch startup_gate(num_workers);

void worker() {
    // Signal "I'm ready"
    startup_gate.count_down();      // decrement without waiting
    // OR: arrive_and_wait() — decrement AND wait for 0

    startup_gate.wait();            // block until count reaches 0
    // Now all workers are initialized; begin simulation
    run_simulation();
}

// In SimulationEngine:
std::latch startup_latch_(num_threads);
// Each WorkerThread calls startup_latch_.arrive_and_wait()
// The last worker to arrive releases all of them simultaneously
// Prevents startup races: no worker begins until all are constructed
```

**`latch` vs `barrier`:**
- `latch`: one-shot, counts down to 0, cannot be reset
- `barrier`: reusable, cycles through phases repeatedly

### `std::counting_semaphore` — limit concurrent access

A semaphore maintains a count. `acquire()` decrements it (blocks if 0). `release()` increments it.

```cpp
#include <semaphore>

// Max 2 threads can access the boundary signal bus simultaneously
// Models cross-region interconnect bandwidth (2 concurrent transfers)
std::counting_semaphore<2> bus_semaphore(2);  // initial count = max = 2

void sync_boundary_signal(int signal_id) {
    bus_semaphore.acquire();    // decrement: block if already 2 threads inside
    // ...transfer signal across region boundary...
    bus_semaphore.release();    // increment: allow another thread in
}

// std::binary_semaphore = counting_semaphore<1> = semaphore with max=1
// Equivalent to a mutex that can be released by a different thread
std::binary_semaphore ready(0);  // starts at 0 (blocked)

// Producer:
ready.release();    // signal ready

// Consumer:
ready.acquire();    // wait for ready
```

---

## 13. Condition Variables — Waiting for a Condition

A condition variable lets one thread sleep until another thread signals that a condition has changed. Always used together with a mutex.

```cpp
std::mutex mtx;
std::condition_variable cv;
bool data_ready = false;
int data = 0;

// Producer:
void produce() {
    {
        std::lock_guard lk(mtx);
        data = 42;
        data_ready = true;
    }
    cv.notify_one();    // wake one waiter (or notify_all() for all)
}

// Consumer:
void consume() {
    std::unique_lock lk(mtx);
    cv.wait(lk, []{ return data_ready; });
    // wait() atomically: 1. releases the lock  2. sleeps
    //                    3. wakes on notify    4. re-acquires lock
    //                    5. checks predicate   6. loops if false (spurious wakeup)
    int v = data;
    // use v
}
```

**Spurious wakeups:** `cv.wait()` can wake up even without a `notify` call. This is not a bug in your code — it's permitted by the C++ standard (and happens on some OS implementations). The lambda predicate `[]{ return data_ready; }` guards against this: if `wait()` wakes spuriously, it checks the predicate, sees it's false, and goes back to sleep.

**Why `cv.wait()` needs the mutex:**
1. The predicate check (`data_ready`) must be atomic with going to sleep — otherwise a notify between the check and the sleep is lost.
2. The mutex protects `data_ready` from concurrent modification.
3. `wait()` releases the mutex while sleeping (so the producer can acquire it to set `data_ready`) and re-acquires it before returning.

**C++20 replacement:** For simple flag-style conditions, `std::atomic::wait()` eliminates the mutex entirely:
```cpp
std::atomic<bool> data_ready{false};
int data = 0;  // only written before store(true)

// Producer:
data = 42;
data_ready.store(true, std::memory_order_release);
data_ready.notify_all();

// Consumer:
data_ready.wait(false, std::memory_order_acquire);  // blocks until != false
int v = data;
```

---

## 14. The Hot Path — Finding What to Optimize

The hot path is the code that runs most often. Optimizing it has the most impact. Optimizing cold code (error handlers, one-time initialization) is wasted effort.

### Step 1: Profile first, optimize second

Never guess where the hot path is. Measure.

```bash
# Compile with debug info (keep -O2 for realistic performance)
c++ -std=c++20 -O2 -g -Iinclude -o build/sim src/main.cpp -lpthread

# Profile with perf (Linux):
perf record -g ./build/sim
perf report        # shows which functions consume most CPU time

# Profile with Instruments (macOS):
# Xcode → Product → Profile → Time Profiler

# Profile with gprof:
c++ -pg -O2 -o build/sim_prof src/main.cpp
./build/sim_prof
gprof build/sim_prof gmon.out | head -40
```

**What to look for in perf output:**
```
Overhead  Command     Shared Object        Symbol
  35.2%   sim         sim                  WorkerThread::process_delta_events
  18.7%   sim         libpthread.so        pthread_mutex_lock          ← HOT LOCK
  12.1%   sim         sim                  FineGrainedStrategy::write_signal
   8.4%   sim         sim                  DeltaQueue::drain
   ...
```

`pthread_mutex_lock` appearing at the top of a profile is a clear sign that lock contention is a bottleneck.

### Step 2: Identify the shared variables on the hot path

For each hot function, ask: what shared state does it read or write?

```
process_delta_events():
    sched_.drain_delta()               → DeltaQueue (drain_lock_)
    strategy_.write_signal(...)        → signals[id] (signal_locks_[id])
    strategy_.activate_process(...)    → processes[id].state (atomic CAS)
    prof_.total_events.fetch_add(...)  → global atomic counter  ← HOT
```

### Step 3: Measure contention specifically

```bash
# Count lock contentions with perf:
perf stat -e lock:contention_begin,lock:contention_end ./build/sim

# Or with mutrace (mutex trace):
LD_PRELOAD=/usr/lib/libmutrace.so ./build/sim
# Reports: most-contended mutexes, average wait time, lock hold time
```

### Step 4: Classify each shared variable

| Variable | Access pattern | Right primitive |
|----------|---------------|----------------|
| Signal value, 1 writer N readers | Read >> Write | `shared_mutex` or `atomic` |
| Event counter, all writers | All writes, read at end | Per-thread shard |
| Process state, single activation | Set-once per round, reset by worker | `atomic_flag` TAS |
| Event queue drain | One drainer at a time, frequent | SpinLock (short hold) |
| Time wheel bucket | Short insert/drain | Per-bucket SpinLock |
| Delta cycle completion | All threads sync once per delta | `std::barrier` |
| Startup synchronization | One-shot | `std::latch` |

---

## 15. The Optimization Workflow — Step by Step

This is exactly what the celeris project demonstrates. Follow this sequence:

### Step 1: Make it correct with a single coarse lock

Start with correctness. One global mutex. If it is correct, you can optimize.

```cpp
// Phase 1: correct, provably simple, definitely slow
class CoarseEngine {
    std::mutex global_;
    void write_signal(int id, int val) {
        std::lock_guard lk(global_);
        signals_[id] = val;
    }
    // ...
};
```

Run correctness tests. Use ThreadSanitizer (see §16). Verify the output is deterministic.

### Step 2: Profile and find the bottleneck

```bash
perf record -g ./build/sim_coarse
perf report
# Typically: global lock shows up at 40-80% of runtime
```

### Step 3: Identify what the lock is protecting and why

Draw a dependency graph:
```
global_lock protects:
  ├── signals_[0..N].value         → per-signal (no cross-signal dependency)
  ├── processes_[0..M].state       → per-process (no cross-process dependency)
  └── event_queue                  → one shared queue (real dependency)
```

Signals have no cross-signal dependency — signal 3 and signal 7 can be read/written concurrently with zero correctness risk. This tells you the global lock is over-synchronized.

### Step 4: Introduce fine-grained locking

```cpp
// Phase 2: per-signal locks — parallel reads on different signals
class FineEngine {
    std::vector<std::shared_mutex> sig_locks_;  // one per signal
    void write_signal(int id, int val) {
        std::unique_lock lk(sig_locks_[id]);    // only this signal
        signals_[id] = val;
    }
    Signal read_signal(int id) {
        std::shared_lock lk(sig_locks_[id]);    // shared: concurrent OK
        return signals_[id];
    }
};
```

Re-run tests and verify correctness. Profile again. Measure speedup.

### Step 5: Replace mutex with atomic on the simplest variables

For variables where the operation is a single read or write (not a conditional read-modify-write), atomic is simpler and faster:

```cpp
// Phase 3: atomic signal values
struct Signal {
    std::atomic<uint8_t> value{0};
};

void write_signal(int id, uint8_t val) {
    signals_[id].value.store(val, std::memory_order_release);
    signals_[id].value.notify_all();   // C++20: wake waiters
}

uint8_t read_signal(int id) {
    return signals_[id].value.load(std::memory_order_acquire);
}
```

### Step 6: Shard high-frequency write-only counters

```cpp
// Phase 4: shard the event counter
alignas(64) struct Shard { long long events; char pad[56]; };
std::array<Shard, MAX_THREADS> event_shards_{};

void count_event(int tid) {
    ++event_shards_[tid].events;   // local, no sharing
}

long long total_events() {   // called rarely
    long long t = 0;
    for (auto& s : event_shards_) t += s.events;
    return t;
}
```

### Step 7: Replace hand-rolled barriers with `std::barrier`

```cpp
// Phase 5: std::barrier instead of mutex+CV+counter
std::barrier<decltype(completion_fn)> delta_sync(
    num_threads, completion_fn);

// Worker:
delta_sync.arrive_and_wait();   // one line, correct, no generation-counter race
```

### Step 8: Measure each phase independently

Each phase change should be benchmarked in isolation — this is exactly what Experiments 1–4 do. Never combine two changes in one measurement. You need to know which change caused which improvement.

---

## 16. Measuring: perf, ThreadSanitizer, helgrind, perf-c2c

### ThreadSanitizer (TSan) — detect race conditions

```bash
# Compile with TSan:
c++ -std=c++20 -fsanitize=thread -g -O1 -o build/sim_tsan src/main.cpp -lpthread

./build/sim_tsan

# TSan output on a race condition:
# WARNING: ThreadSanitizer: data race (pid=12345)
#   Write of size 4 at 0x... by thread T2:
#     #0 FineGrainedStrategy::write_signal src/main.cpp:127
#   Previous read of size 4 at 0x... by thread T1:
#     #0 WorkerThread::process_delta_events src/main.cpp:93
#   Thread T2 created at:
#     #0 SimulationEngine::run_until src/main.cpp:128
```

Run TSan on every synchronization change before measuring performance. A race condition produces wrong answers — there is no point benchmarking incorrect code.

### AddressSanitizer (ASan) — detect memory errors

```bash
c++ -std=c++20 -fsanitize=address -g -O1 -o build/sim_asan src/main.cpp -lpthread
./build/sim_asan
# Detects: buffer overflows, use-after-free, double-free, stack overflow
```

### helgrind — mutex ordering and deadlock detection (Valgrind)

```bash
valgrind --tool=helgrind ./build/sim
# Reports:
# - Lock ordering violations (potential deadlock)
# - Mutex used without initialization
# - Data races (slower than TSan but catches different cases)
```

### perf — CPU performance counters

```bash
# Overall stats: IPC, cache misses, branch mispredictions
perf stat ./build/sim
# Output:
#  1,234,567,890  cycles
#    456,789,012  instructions    #    0.37  insns per cycle   ← low = stalls
#     98,765,432  cache-misses    #   12.3%  of all cache refs ← high = memory bound
#     12,345,678  LLC-loads-misses

# Flamegraph: where is time spent?
perf record -g --call-graph dwarf ./build/sim
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg

# Lock contention events:
perf stat -e lock:contention_begin ./build/sim
```

### perf c2c — cache-to-cache (false sharing) detection

```bash
# Record cache-to-cache transfers:
perf c2c record -ag -- ./build/sim

# Report hot cache lines:
perf c2c report --stdio
# Key columns:
#   LLC Miss  — how often this line caused a LLC miss (had to go to memory/remote cache)
#   Remote HITM — how often another core had this line in Modified state when we needed it
#                 HIGH Remote HITM = cache-line ping-pong or false sharing

# Sort by hottest lines:
perf c2c report --sort=hitm
```

**Interpreting `perf c2c` output:**
```
=================================================
            Shared Data Cache Line Table
=================================================
 ------- Cacheline ----------      Total    ...   Remote HITM
    #, Data address, cl offset     HITMs          (ping-pong)
---------------------------------------------------------
   0  0x7f...4a0  0x0           12,345     ...    11,890  ← THIS LINE IS HOT
      Profiler::total_events              src/main.cpp:45
```

A line with high Remote HITM means this 64-byte block is being bounced between cores. Find the variable at that address and either: make it `alignas(64)` padded per-thread (shard) or reduce write frequency.

### Benchmarking correctly

```bash
# Pin to specific CPUs to reduce OS scheduling noise:
taskset -c 0,1,2 ./build/sim

# Disable CPU frequency scaling for consistent results:
# (Linux, requires root)
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Run multiple times and take the median (not average, which is skewed by outliers):
for i in {1..10}; do ./build/sim 2>&1 | grep Events/sec; done | sort -n | sed -n '5p'

# Use `hyperfine` for automatic statistical benchmarking:
hyperfine --warmup 3 './build/sim'
```

---

## 17. Common Bugs and How to Recognize Them

### Bug 1: The Lost Update (classic data race)

**Symptom:** Counter is always less than expected. Non-deterministic result.

```cpp
// BUGGY:
int counter = 0;
for (int i = 0; i < N; ++i) counter++;   // from multiple threads

// FIX:
std::atomic<int> counter{0};
for (int i = 0; i < N; ++i) counter.fetch_add(1, std::memory_order_relaxed);
```

**Detection:** ThreadSanitizer reports a write-write or write-read race.

### Bug 2: Deadlock (circular lock dependency)

**Symptom:** Program hangs forever. All threads are blocked.

```cpp
// Thread A:           Thread B:
lock(mutex_a);         lock(mutex_b);
lock(mutex_b);         lock(mutex_a);  // DEADLOCK: each waits for the other
```

**Fix:** Always acquire multiple locks in the same order, or use `std::scoped_lock`.

**Detection:** `helgrind` detects lock ordering violations. Stack trace of all threads shows all blocked in `mutex::lock`.

### Bug 3: The ABA Problem (lock-free CAS)

**Symptom:** CAS succeeds but the data has changed and changed back.

```cpp
// Thread A reads value = 5
// Thread B changes value: 5 → 3 → 5
// Thread A does CAS(5 → 7): succeeds because value is 5 again
// But the change to 3 has been lost (if it meant something)
```

**Fix:** Add a version counter alongside the value. CAS on both atomically.

```cpp
struct Versioned { int value; int version; };
std::atomic<Versioned> state;
// CAS checks both value AND version — version never goes backwards
```

### Bug 4: The Spurious Wakeup (condition variable)

**Symptom:** Consumer processes events when none exist. Intermittent.

```cpp
// BUGGY:
cv.wait(lk);   // can wake up with no notify

// FIX: always use a predicate
cv.wait(lk, []{ return !queue.empty(); });
```

**Detection:** Happens rarely, hard to reproduce. Add assertions: `assert(!queue.empty())` after the wait.

### Bug 5: The Hand-Rolled Barrier Race (generation counter)

**Symptom:** Program deadlocks intermittently, more likely with more threads.

```cpp
// BUGGY: arrived_ == 0 predicate races with fast threads re-entering
cv.wait(lk, [this]{ return arrived_ == 0; });

// FIXED: generation counter is phase-stable
cv.wait(lk, [this, gen]{ return generation_ != gen; });

// BEST: use std::barrier (correct by construction)
```

**Detection:** Hang under `std::thread::join()`. All threads blocked in `condition_variable::wait()`.

### Bug 6: Uninitialized Memory Read (startup race)

**Symptom:** First few events processed incorrectly. Non-deterministic crash or wrong output on first run.

```cpp
// BUGGY: threads can start before signals[] is populated
for (int i = 0; i < N; ++i) threads.emplace_back(worker);
for (auto& s : signals) engine.add_signal(s);   // TOO LATE: workers already started

// FIX: std::latch — workers wait until all setup is complete
std::latch ready(N);
for (int i = 0; i < N; ++i) threads.emplace_back([&]{
    ready.arrive_and_wait();   // wait until all workers are ready
    worker();
});
// setup happens here while all workers are blocked at the latch
for (auto& s : signals) engine.add_signal(s);
ready.count_down();  // ← this is wrong actually; all workers call arrive_and_wait
// Correct: the latch counts worker arrivals; they all proceed together
```

**Detection:** ThreadSanitizer reports reads from uninitialized memory. Valgrind Memcheck also catches it.

### Bug 7: Memory Ordering Bug (visibility)

**Symptom:** Thread B reads a value that Thread A "definitely wrote already". Intermittent, only on ARM/POWER, or when compiler reorders.

```cpp
// BUGGY:
data = 42;
flag.store(true, std::memory_order_relaxed);   // ← wrong: no ordering guarantee

// Thread B:
while (!flag.load(std::memory_order_relaxed));  // ← wrong
assert(data == 42);  // can fail on ARM

// FIX: release/acquire pair
data = 42;
flag.store(true, std::memory_order_release);   // all prior writes visible

while (!flag.load(std::memory_order_acquire)); // see all writes before release
assert(data == 42);  // guaranteed
```

**Detection:** Very hard without hardware. Run on ARM (e.g., AWS Graviton, Apple M). ThreadSanitizer does catch some ordering bugs.

---

## 18. Everything Connected — The celeris Story

Here is how every concept above maps to a specific file and line in this project:

| Concept | File | What it shows |
|---------|------|--------------|
| Race condition | `LegacySimEngine.hpp:147` | `volatile bool stop_flag_` — data race between writer (main) and reader (workers) |
| Coarse mutex | `CoarseGrainedStrategy.hpp:47` | One `global_lock_` for every read and write |
| Fine-grained mutex | `FineGrainedStrategy.hpp:55` | `signal_locks_[id]` — only blocks this signal |
| Reader-writer lock | `FineGrainedStrategy.hpp:55,67` | `shared_lock` for reads, `unique_lock` for writes |
| SpinLock + backoff | `SpinLock.hpp` | TAS loop with `_mm_pause()` and exponential backoff |
| Atomic flag TAS | Experiment 3 in `main.cpp` | `atomic_flag::test_and_set` vs mutex+bool |
| Atomic store/load | `AtomicStrategy.hpp` | `atomic_value.store(release)` / `load(acquire)` |
| CAS | `AtomicStrategy.hpp:activate_process` | `compare_exchange_strong(DORMANT → READY)` |
| Release-acquire | `AtomicStrategy.hpp:write_signal` | `store(release)` pairs with `load(acquire)` |
| atomic::wait/notify | `AtomicStrategy.hpp` | C++20 wait without condition_variable |
| Cache-line padding | Experiment 4 `bench_shared_sharded()` | `alignas(64) PaddedCounter` |
| Per-thread shard | Experiment 4 `bench_shared_sharded()` | Each thread owns its counter; merge at end |
| False sharing | Experiment 4 comment | Without `alignas(64)`, shards on same line = ping-pong |
| Ping-pong demo | Experiment 4 `bench_shared_atomic()` | 3245× gap between atomic and shard |
| Condition variable | `LegacySimEngine.hpp:barrier_cv_` | Hand-rolled barrier using CV |
| Spurious wakeup fix | `LegacySimEngine.hpp:247` | Lambda predicate in `cv.wait()` |
| std::barrier | `DeltaBarrier.hpp:52` | One line replaces 30-line hand-rolled barrier |
| Completion function | `DeltaBarrier.hpp:on_delta_complete` | Runs once per delta, on last arriving thread |
| Deadlock (generation bug) | `LegacySimEngine.hpp:216-250` | `arrived_==0` race → fixed with generation counter |
| std::latch | `SimulationEngine.hpp:46` | Startup gate: all workers wait before simulating |
| counting_semaphore | `SimulationEngine.hpp:49` | Bus bandwidth limiter: max 2 concurrent transfers |
| Strategy pattern | `ISyncStrategy.hpp`, `SyncStrategyFactory.hpp` | Runtime swap of sync algorithm |
| Hot path profiling | Experiments 1-4 | Each isolates one variable; measures independently |
| IEEE 1800 scheduling | `EventScheduler.hpp` | Active/NBA/delta regions |
| Time wheel | `TimeWheel.hpp` | O(1) scheduling, per-bucket SpinLock |
| Ping-pong buffer | `DeltaQueue.hpp` | Separate insert/drain locks for different buffers |

---

## 19. Interview Questions and Model Answers

### Fundamentals

**Q: What is a race condition?**

A: A race condition occurs when the result of a program depends on the non-deterministic interleaving of operations from multiple threads on shared state. The canonical example is `++counter` from two threads: the load-modify-store is three instructions, and if both threads load the same value before either stores, one increment is lost. Detection: ThreadSanitizer. Fix: mutex, atomic, or eliminate sharing.

---

**Q: What is the difference between `std::mutex` and `std::atomic`?**

A: A mutex serializes access to an arbitrary-sized critical section through OS-level blocking — any thread that can't acquire the lock is put to sleep by the kernel. An atomic serializes a single read-modify-write operation at the CPU instruction level with no OS involvement. Mutex is general-purpose and handles complex multi-variable invariants. Atomic is faster (single instruction, no syscall) but limited to primitive types and simple operations. Use mutex when you need to protect multiple variables together; use atomic when you need to protect a single integer, flag, or pointer.

---

**Q: What is the difference between coarse-grained and fine-grained locking?**

A: Coarse-grained locking protects a large resource (e.g., all signals in a simulation) with a single lock. It is simple and easy to get correct, but any two operations on any signals serialize even if they have no data dependency. Fine-grained locking assigns one lock per resource (one mutex per signal). Two threads touching different signals now run in parallel. The tradeoff: more locks means more memory, more construction overhead, and more deadlock risk (mitigated by consistent lock ordering). The benefit only materializes when threads frequently access different resources concurrently. In this project: Experiment 1 shows 2.15× improvement from fine-grained at 3 threads and 8 signals.

---

**Q: Explain `memory_order_acquire` and `memory_order_release`.**

A: These form a "release-acquire" pair — the fundamental tool for safely publishing data from one thread to another without a mutex.

`release` on a store: guarantees that all memory writes in this thread that happen before the store are visible to any thread that subsequently does an `acquire` load of the same atomic variable.

`acquire` on a load: guarantees that all memory reads in this thread that happen after the load will see all writes that happened before the matching `release` store.

Example: producer writes data, then `store(true, release)`. Consumer `load(acquire)`, sees true, then reads data — guaranteed to see the correct data. Without release/acquire, the CPU or compiler could reorder the data write and the flag store, breaking the consumer's assumption.

---

**Q: What is a deadlock and how do you prevent it?**

A: A deadlock occurs when thread A holds lock 1 and waits for lock 2, while thread B holds lock 2 and waits for lock 1 — both wait forever. Prevention strategies:
1. **Lock ordering:** always acquire multiple locks in the same global order (e.g., ascending lock ID)
2. **`std::scoped_lock`:** acquires multiple mutexes simultaneously using deadlock avoidance
3. **Try-lock with timeout:** `try_lock_for()` — if you can't get both, release what you have and retry
4. **Lock hierarchy:** enforced by policy — thread at level N can only lock at level > N
5. **Avoid holding locks across blocking operations**

---

**Q: What is `std::atomic::compare_exchange_strong` and when do you use it?**

A: CAS atomically does: if `*this == expected`, set `*this = desired` and return true; else set `expected = *this` and return false. It's the universal building block for lock-free algorithms. Use cases: implementing spinlocks, setting a flag exactly once (even with multiple concurrent attempts), updating a value only if it hasn't changed since you last read it. The "strong" variant never fails spuriously — use it for one-shot activations. The "weak" variant can fail spuriously — use it in retry loops where the extra retry cost is acceptable on LL/SC architectures.

---

### Cache and Performance

**Q: What is a cache line and why does it matter for concurrency?**

A: A cache line is the unit of transfer between CPU caches and memory — typically 64 bytes. When any byte in a line is read, the whole 64 bytes are loaded. When any byte is written, the whole line eventually needs to propagate. For concurrency: if two threads write to different variables that happen to share a cache line, each write invalidates the other thread's copy of the line, forcing a cache-to-cache transfer even though there's no logical data sharing. This is "false sharing" and can make "parallel" code slower than single-threaded. Fix: `alignas(64)` to place each thread's data on its own line.

---

**Q: Why is `atomic::fetch_add` still slow under high contention?**

A: `fetch_add` is a hardware read-modify-write instruction (LOCK XADD on x86) — it's atomic at the instruction level with no OS involvement. But under high contention, the cache line holding the atomic variable must be in "Modified" state in exactly one core at a time (MESI protocol). Every `fetch_add` forces an ownership transfer: the current owner broadcasts an invalidation, the requester waits, gets the line, writes, becomes the new owner. At 32 cores all calling `fetch_add`, this is a chain of 32 sequential cache-line transfers. The atomic instruction is fast; the coherence protocol is the bottleneck. Solution: per-thread shards with a merge at barrier time — zero coherence traffic on the hot path.

---

**Q: What is `perf c2c` and what does "Remote HITM" mean?**

A: `perf c2c` (cache-to-cache) records events where a CPU needed a cache line that another CPU had in Modified state — i.e., the requester had to wait for the owner to transfer the line. "Remote HITM" (Hit In Modified) is the count of these transfers for a specific cache line. A high Remote HITM count on a specific line means that line is being bounced between cores — either true sharing (multiple cores write to the same variable) or false sharing (different variables on the same 64-byte line). Optimization: shard the variable (eliminate sharing) or pad it (eliminate false sharing).

---

### C++20 Specific

**Q: What is `std::barrier` and how does it differ from a hand-rolled barrier?**

A: `std::barrier` is a reusable synchronization point: N threads call `arrive_and_wait()`, all block until all N have arrived, the last arriving thread runs the completion function (if provided), then all N are released simultaneously. Compared to hand-rolled (mutex + condition_variable + counter):
1. **Correct by construction** — the generation-counter race is handled internally
2. **Single object** — replaces three (mutex, CV, counter)
3. **noexcept completion function** — explicit failure (terminate) rather than silent deadlock
4. **Completion function is guaranteed to run on the last thread** — no extra condition needed
5. **All threads released simultaneously** — hand-rolled notify_all wakes threads one at a time from the OS scheduler's perspective

---

**Q: What is `std::latch` and how does it differ from `std::barrier`?**

A: Both are countdown synchronization primitives. `std::latch` counts down from N to 0 once — when it reaches 0, all waiters are released and it cannot be reset. `std::barrier` is reusable — it resets after each phase. Use `latch` for one-time events: startup gate (all threads initialized before any begins work), initialization completion, one-time handshake. Use `barrier` for repeated phase synchronization (each delta cycle, each simulation step).

---

**Q: What does `std::atomic::wait()` do, and what does it replace?**

A: `atomic::wait(old_value)` blocks the calling thread until the atomic's value changes from `old_value`. It uses an OS-efficient mechanism (futex/ulock) — the thread is truly sleeping, not spinning. It replaces the classic `mutex + condition_variable + predicate` pattern for simple flag-style waits. The pairing: store with `notify_all()` on the writer side, `wait(old_val)` on the reader side. No mutex needed, no spurious wakeup handling, no separate predicate variable. C++20 addition.

---

### System Design

**Q: You have a hot shared counter incremented by 32 threads on every event. What do you do?**

A: Three options in increasing performance:

1. **`std::atomic<long long>` + `fetch_add(relaxed)`:** correct, simple, ~10× faster than mutex. But under 32-thread sustained contention, the cache line bounces — all 32 threads serialize at the hardware level. Still fast enough for many use cases.

2. **Per-thread shard + barrier-time merge:** Each thread has its own `alignas(64)` counter. Zero shared state on hot path — pure local writes at L1 speed. Merge all 32 shards once per delta cycle (in the barrier completion function). Throughput scales linearly with threads. Tradeoff: reading the "current" total requires summing all shards.

3. **Report at the end only:** if you only need the total at simulation end (not during), accumulate locally and reduce once. Simplest version of sharding.

Choose 1 if the counter is read frequently (every delta cycle) and contention is moderate. Choose 2 if you profile and see the counter's cache line in `perf c2c` with high Remote HITM.

---

**Q: Describe the producer-consumer pattern with a ping-pong double buffer.**

A: Two buffers alternate roles each cycle. "Active" buffer is being drained by consumers (read-only). "Pending" buffer is being filled by producers (write-only). At cycle boundary, swap the active index — pending becomes active, active (now empty) becomes pending. Key insight: drain and push operate on different buffers at all times, so they need separate locks — not the same lock. The flip (swap) is the only moment of true coordination, and it happens once per cycle in the barrier completion function. This is the `DeltaQueue` design in this project.

---

**Q: How would you find and fix false sharing in a parallel profiler?**

A:
1. Run `perf c2c record -ag -- ./program && perf c2c report` — identify cache lines with high Remote HITM
2. Map the hot line to source code via the address in the report
3. Check if the line contains multiple independent variables (one per thread)
4. Fix: `alignas(64)` padding to place each thread's variable on its own 64-byte line
5. Verify: re-run `perf c2c` — Remote HITM should drop to near zero for that line
6. Benchmark: the per-thread write should now be as fast as a pure local write

Example from this project: `bench_shared_sharded()` in Experiment 4 uses `alignas(64) PaddedCounter`. Without the padding, adjacent counters share a cache line and the "sharded" version sees the same ping-pong as a single atomic. With it, throughput is 3245× higher than the mutex baseline.

---

*The celeris codebase contains a working, benchmarked example of every concept covered in this guide.*
