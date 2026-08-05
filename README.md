# ThreadWeave

[[THIS PROJECT IS CURRENTLY WIP AND SUBJECT TO CHANGE]]

ThreadWeave is a modern C++23 concurrency library providing a high-performance, work-stealing **thread pool** along with a collection of lock-free data structures used to build it. It's designed for low-overhead task submission and execution across many worker threads.

## Features

- **`ThreadPool`** — a work-stealing thread pool where each worker owns a local Chase-Lev deque and can steal work from other workers when idle. Tasks submitted from non-worker threads go through a global MPSC injection queue.
- **`Future<T>`** — a lightweight future returned from `ThreadPool::submit`, supporting result retrieval and exception propagation.
- Lock-free building blocks, usable independently of the thread pool:
    - **`ChaseLevDeque<T>`** — single-producer, multi-consumer work-stealing deque.
    - **`TreiberStack<T>`** — classic lock-free stack, using hazard pointers for safe memory reclamation.
    - **`MichaelScottQueue<T>`** — lock-free MPMC queue, also hazard-pointer protected.
    - **`VyukovQueue<T>`** — intrusive MPSC queue used internally for task injection.
    - **`Hazard`** — hazard pointer infrastructure for safe lock-free memory reclamation.
    - **`NodeAllocator`** — per-thread pooled allocator for node-based structures.

## Requirements

- A C++23-capable compiler (the project uses features such as `std::print` and pack-expansion in lambda captures)
- CMake ≥ 3.23
- Internet access on first configure (CMake `FetchContent` pulls in GoogleTest and Google Benchmark for the test and benchmark targets)

## Getting Started

### Build

```bash
TODO
git clone https://github.com/phipnye/ThreadWeave
cd ThreadWeave
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds the static `ThreadWeave` library along with the `examples`, `benchmark`, `tests`, and `profile` subprojects.

### Run the example

```bash
TODO
./build/examples/main
```

The example spawns a `ThreadPool`, submits a batch of naive-recursive Fibonacci tasks, and prints each result as futures resolve.

### Run tests

Tests use GoogleTest and are discovered automatically via CTest:

```bash
TODO
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

Debug builds enable AddressSanitizer and UndefinedBehaviorSanitizer for most test targets.

### Run benchmarks

Benchmark executables are built with Google Benchmark; see `benchmark/run_benchmarks.sh` for the full workflow, which also drives the R scripts under `benchmark/analysis/scripts` to produce comparison plots against other task-scheduling libraries.

## Usage

```cpp
#include <threadweave/ThreadPool.h>

int square(int n) {
  return n * n;
}

int main() {
  ThreadWeave::ThreadPool pool{};  // defaults to hardware_concurrency() workers
  auto f{pool.submit(square, 21)};
  int res{future.get()};  // blocks until the task completes
}
```

- `ThreadPool::submit(f, args...)` returns a `Future<std::invoke_result_t<F, Args...>>`.
- If the caller submitting a task is itself a worker thread, the task is pushed directly onto that worker's own deque; otherwise it goes through the shared injection queue.
- Threads waiting on a future (`Future::get`) that happen to be pool workers keep executing other queued work while they wait, rather than blocking idle.

## Configuration

The library exposes several compile-time knobs via `target_compile_definitions` in the top-level `CMakeLists.txt`:

| Macro                | Purpose                                                                                                                               |
|----------------------|---------------------------------------------------------------------------------------------------------------------------------------|
| `TW_NDEBUG`          | Automatically defined in Release builds; strips internal debug assertions.                                                            |
| `TW_CACHE_LINE_SIZE` | Cache line size used for alignment (default `64`), avoids relying on `std::hardware_destructive_interference_size`.                   |
| `TW_SEQ_CST`         | If defined, forces every atomic operation in the library to use sequential consistency (useful for debugging memory-ordering issues). |
| `TW_CALLABLE_STORAGE_SIZE`    | Size of the internal buffer used to store a task's bound function/arguments inside a `Future` node.                                   |
| `TW_MAX_THREADS`     | Maximum number of threads the hazard-pointer subsystem should expect to manage.                                                       |

## Project Layout

```
include/threadweave/   Public headers (ThreadPool, Future, lock-free data structures)
src/                    Library implementation files
examples/               Minimal usage example
tests/                  GoogleTest unit tests for each data structure and the thread pool
benchmark/              Google Benchmark suites + R scripts/plots comparing against other libraries
profile/                Profiling target
```

## License

Released under the [MIT License](LICENSE).