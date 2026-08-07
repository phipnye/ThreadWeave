# ThreadWeave Examples

Five small, self-contained programs showing common ways to use the library.

| File                        | Demonstrates                                                                                  |
|-----------------------------|-----------------------------------------------------------------------------------------------|
| `01_basic_tasks.cpp`        | Submitting independent tasks to a `ThreadPool` and collecting results.                        |
| `02_divide_and_conquer.cpp` | Recursive fan-out/fan-in parallelism, relying on `Future::get()` not blocking worker threads. |
| `03_exception_handling.cpp` | Exceptions thrown in a task propagate out of `Future::get()`.                                 |
| `04_lock_free_queue.cpp`    | Using `MichaelScottQueue` under multi-producer/multi-consumer semantics.                      |
| `05_lock_free_stack.cpp`    | Using `TreiberStack` under multi-producer/multi-consumer semantics.                           |

## Build

From the repository root:

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release
```

## Run

```bash
./cmake-build-release/examples/01_basic_tasks
./cmake-build-release/examples/02_divide_and_conquer
./cmake-build-release/examples/03_exception_handling
./cmake-build-release/examples/04_lock_free_queue
./cmake-build-release/examples/05_lock_free_stack
```

## Compiling a single file directly

```bash
g++ -std=c++23 -I/path/to/ThreadWeave/include -pthread 01_basic_tasks.cpp -o basic_tasks
./basic_tasks
```

Swap in any of the other filenames the same way.