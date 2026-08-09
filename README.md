# ThreadWeave

[[THIS PROJECT IS CURRENTLY WIP AND SUBJECT TO CHANGE]]

ThreadWeave is a modern C++23 concurrency library providing a high-performance, work-stealing **thread pool** along with
a collection of lock-free data structures used to build it. It's designed for low-overhead task submission and execution
across many worker threads.

## Features

- **`ThreadPool`** — a work-stealing thread pool where each worker owns a local Chase-Lev deque and can steal work from
  other workers when idle. Tasks submitted from non-worker threads go through a global MPSC injection queue.
- **`Future<T>`** — a lightweight future returned from `ThreadPool::submit`, supporting result retrieval and exception
  propagation.
- Lock-free building blocks, usable independently of the thread pool:
    - **`ChaseLevDeque<T>`** — single-producer, multi-consumer work-stealing deque.
    - **`TreiberStack<T>`** — classic lock-free stack, using hazard pointers for safe memory reclamation.
    - **`MichaelScottQueue<T>`** — lock-free MPMC queue, also hazard-pointer protected.
    - **`VyukovQueue<T>`** — intrusive MPSC queue used internally for task injection.
    - **`Hazard`** — hazard pointer infrastructure for safe lock-free memory reclamation.
    - **`NodeAllocator`** — per-thread pooled allocator for node-based structures.

## Requirements

- A C++23-capable compiler
- CMake ≥ 3.23
- Internet access on first configure (CMake `FetchContent` pulls in GoogleTest and Google Benchmark for the test and
  benchmark targets)

## Getting Started

### Build

```bash
git clone https://github.com/phipnye/ThreadWeave
cd ThreadWeave
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release
```

This builds the `ThreadWeave` library along with the `examples`, `benchmark`, `tests`, and `profile` subprojects.

### Run the examples

```bash
./cmake-build-release/examples/01_basic_tasks
./cmake-build-release/examples/02_divide_and_conquer
./cmake-build-release/examples/03_exception_handling
./cmake-build-release/examples/04_lock_free_queue
./cmake-build-release/examples/05_lock_free_stack 
```

The examples demonstrate several simple instances of how you can use several features provided in the library.

### Run tests

Tests use GoogleTest:

```bash
cd tests
./run_tests.sh
```

Debug builds enable AddressSanitizer and UndefinedBehaviorSanitizer for test targets.

### Run benchmarks

```bash
cd benchmark
./run_benchmarks.sh
```

Benchmark executables are built with Google Benchmark and result jsons are output to `benchmark/analysis/jsons`. If the
user has `R` installed, this bash script will run the scripts in `benchmark/analysis/scripts` and produce benchmarking
result plots in `benchmark/analysis/plots`.

## Usage

```cpp
#include <threadweave/ThreadPool.h>
#include <iostream>

int square(int n) {
  return n * n;
}

int main() {
  ThreadWeave::ThreadPool pool{2};  // use 2 threads
  auto f{pool.submit(square, 21)};  // submit square(21) to pool
  const int res{future.get()};      // blocks until the task completes
  std::cout << "Result = " << res << '\n';
}
```

- `ThreadPool::submit(f, args...)` returns a `Future<std::invoke_result_t<F, Args...>>`.
- If the caller submitting a task is itself a worker thread, the task is pushed directly onto that worker's own deque;
  otherwise it goes through the shared injection queue.
- Threads waiting on a future (`Future::get`) that happen to be pool workers keep executing other queued work while they
  wait, rather than blocking idly.

## Configuration

The library exposes several compile-time knobs via `target_compile_definitions` in the top-level `CMakeLists.txt`:

| Macro                      | Purpose                                                                                                                               |
|----------------------------|---------------------------------------------------------------------------------------------------------------------------------------|
| `TW_NDEBUG`                | Automatically defined in Release builds; strips internal debug assertions.                                                            |
| `TW_CACHE_LINE_SIZE`       | Cache line size used for alignment (default `64`), avoids relying on `std::hardware_destructive_interference_size`.                   |
| `TW_SEQ_CST`               | If defined, forces every atomic operation in the library to use sequential consistency (useful for debugging memory-ordering issues). |
| `TW_CALLABLE_STORAGE_SIZE` | Size of the internal buffer used to store a task's bound function/arguments inside a `Future` node.                                   |
| `TW_MAX_THREADS`           | Maximum number of threads the hazard-pointer subsystem should expect to manage.                                                       |

## Project Layout

```
include/threadweave/    Public headers (ThreadPool, lock-free data structures)
examples/               Minimal usage example
tests/                  GoogleTest unit tests for each data structure and the thread pool
benchmark/              Google Benchmark suites + R scripts/plots comparing against other libraries
profile/                Profiling target
```

## License

Released under the [MIT License](LICENSE).