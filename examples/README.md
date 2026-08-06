# ThreadWeave Examples

Five small, self-contained programs showing common ways to use the library.

| File                        | Demonstrates                                                                                                     |
|-----------------------------|------------------------------------------------------------------------------------------------------------------|
| `01_basic_tasks.cpp`        | Submitting independent tasks to a `ThreadPool` and collecting results.                                           |
| `02_divide_and_conquer.cpp` | Recursive fan-out/fan-in parallelism, relying on `Future::get()` not blocking worker threads.                    |
| `03_exception_handling.cpp` | Exceptions thrown in a task propagate out of `Future::get()`.                                                    |
| `04_lock_free_queue.cpp`    | Using `MichaelScottQueue` directly under real multi-producer/multi-consumer contention, without the thread pool. |
| `05_lock_free_stack.cpp`    | Using `TreiberStack` directly under the same multi-producer/multi-consumer pattern.                              |

## Build

From the repository root:

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release
```

## Run

```bash
./build/examples/01_basic_tasks
./build/examples/02_divide_and_conquer
./build/examples/03_exception_handling
./build/examples/04_lock_free_queue
./build/examples/05_lock_free_stack
```

## Compiling a single file directly

```bash
g++ -std=c++23 -I/path/to/ThreadWeave/include -pthread 01_basic_tasks.cpp -o basic_tasks
./basic_tasks
```

Swap in any of the other filenames the same way.