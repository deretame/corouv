# corouv

`corouv` is a small stackless coroutine base library built on:

- `libuv` for event loop, timers, fd polling, and worker-thread dispatch
- `async_simple` for the coroutine task model (`Lazy<T>`)

The repository is intentionally split into four layers:

- `core`
  - public headers: [`include/corouv`](/home/windy/pack/libuv-test/include/corouv)
  - implementation: [`src`](/home/windy/pack/libuv-test/src)
  - build entry: [`CMakeLists.txt`](/home/windy/pack/libuv-test/CMakeLists.txt)
- `examples`
  - standalone examples: [`examples`](/home/windy/pack/libuv-test/examples)
  - example build entry: [`examples/CMakeLists.txt`](/home/windy/pack/libuv-test/examples/CMakeLists.txt)
- `tests`
  - standalone tests: [`tests`](/home/windy/pack/libuv-test/tests)
  - test build entry: [`tests/CMakeLists.txt`](/home/windy/pack/libuv-test/tests/CMakeLists.txt)
- `scripts`
  - dependency and helper scripts: [`scripts`](/home/windy/pack/libuv-test/scripts)

## Repository layout

```text
include/corouv/     core public API
src/                core implementation
examples/           example programs and example-only adapters
tests/              standalone test programs
scripts/            fetch/build/run helper scripts
third_party/        vendored core dependencies
```

## Core library

The root CMake only builds and installs the library.

Core features:

- `UvExecutor` backed by `uv_loop_t`
- `Runtime`
- `Task<T>`
- timers and `yield_now()`
- fd readiness polling
- TCP listeners/streams and async DNS resolution
- blocking work offload via `uv_queue_work`
- cancellation and timeout helpers
- coroutine concurrency primitives (`AsyncMutex`, `AsyncSemaphore`, `AsyncQueue`)
- HTTP/1.1 request/response parsing plus plain HTTP client/server support
- small libuv fs awaiters

Main public entry points live in:

- [`corouv.h`](/home/windy/pack/libuv-test/include/corouv/corouv.h)
- [`executor.h`](/home/windy/pack/libuv-test/include/corouv/executor.h)
- [`runtime.h`](/home/windy/pack/libuv-test/include/corouv/runtime.h)
- [`poll.h`](/home/windy/pack/libuv-test/include/corouv/poll.h)
- [`blocking.h`](/home/windy/pack/libuv-test/include/corouv/blocking.h)
- [`net.h`](/home/windy/pack/libuv-test/include/corouv/net.h)
- [`http.h`](/home/windy/pack/libuv-test/include/corouv/http.h)
- [`task_group.h`](/home/windy/pack/libuv-test/include/corouv/task_group.h)
- [`async_mutex.h`](/home/windy/pack/libuv-test/include/corouv/async_mutex.h)
- [`async_semaphore.h`](/home/windy/pack/libuv-test/include/corouv/async_semaphore.h)
- [`async_queue.h`](/home/windy/pack/libuv-test/include/corouv/async_queue.h)

## Core deps

Core source deps are:

- `libuv`
- `async_simple`
- vendored `picohttpparser` for HTTP/1 parsing

Fetch them with:

```bash
./scripts/fetch_deps.sh
```

Pinned revisions are recorded in:

- [`third_party/deps.lock`](/home/windy/pack/libuv-test/third_party/deps.lock)

## Build core

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Install:

```bash
cmake --install build --prefix /tmp/corouv-install
```

## Build examples

Examples have their own CMake entry and are built separately from the core
library build.

```bash
cmake -S examples -B build/examples -DCMAKE_BUILD_TYPE=Release
cmake --build build/examples -j
```

Core examples:

- `corouv_timer`
- `corouv_read_file`
- `corouv_blocking`
- `corouv_timeout`
- `corouv_event`
- `corouv_cancel`
- `corouv_task_group`
- `corouv_fs_write_read`
- `corouv_primitives`
- `corouv_http_server`
- `corouv_http_client`

Run them from the examples build tree, for example:

```bash
./build/examples/corouv_timer
./build/examples/corouv_read_file ../CMakeLists.txt
./build/examples/corouv_blocking
./build/examples/corouv_timeout
./build/examples/corouv_event
./build/examples/corouv_cancel
./build/examples/corouv_task_group
./build/examples/corouv_fs_write_read /tmp/corouv-fs-example.txt
./build/examples/corouv_primitives
./build/examples/corouv_http_server 127.0.0.1 8080 1
./build/examples/corouv_http_client http://127.0.0.1:8080/hello?name=corouv
```

What each extra example shows:

- [`event.cpp`](/home/windy/pack/libuv-test/examples/event.cpp): `ManualResetEvent::wait/set/reset`
- [`cancel.cpp`](/home/windy/pack/libuv-test/examples/cancel.cpp): `CancellationSource` + `with_cancellation`
- [`task_group.cpp`](/home/windy/pack/libuv-test/examples/task_group.cpp): `make_task_group()` + `spawn()/wait()`
- [`fs_write_read.cpp`](/home/windy/pack/libuv-test/examples/fs_write_read.cpp): low-level `fs::Open/Write/Close` and `fs::read_file`
- [`concurrency_primitives.cpp`](/home/windy/pack/libuv-test/examples/concurrency_primitives.cpp): `AsyncMutex` + `AsyncSemaphore` + `AsyncQueue`
- [`http_server.cpp`](/home/windy/pack/libuv-test/examples/http_server.cpp): HTTP/1.1 server with `corouv::http::Server`
- [`http_client.cpp`](/home/windy/pack/libuv-test/examples/http_client.cpp): plain HTTP/1.1 client via `corouv::http::fetch`

## Optional libpq example

`libpq` is example-only. It is not part of the core public API or install
surface.

Fetch the example-only PostgreSQL source with:

```bash
./scripts/fetch_libpq_example_deps.sh
```

Pinned example-only revisions are recorded in:

- [`example-deps.lock`](/home/windy/pack/libuv-test/examples/example-deps.lock)

Build bundled `libpq` for the example with:

```bash
./scripts/build_libpq.sh
```

Or from the examples build tree:

```bash
cmake --build build/examples --target corouv_build_libpq_example
```

If you want a disposable local PostgreSQL for testing:

```bash
./scripts/run_postgres_podman.sh
```

Then build and run the example:

```bash
cmake --build build/examples --target corouv_libpq
./build/examples/corouv_libpq
```

Default conninfo:

```text
host=127.0.0.1 port=5432 user=postgres password=mysecretpassword dbname=postgres
```

You can override it with `COROUV_PG_CONNINFO` or pass a conninfo string as the
first CLI argument.

Example-specific files:

- [`libpq.cpp`](/home/windy/pack/libuv-test/examples/libpq.cpp)
- [`libpq_support.h`](/home/windy/pack/libuv-test/examples/libpq_support.h)

## Minimal usage

```cpp
#include <corouv/loop.h>
#include <corouv/sync_wait.h>

#include <async_simple/coro/Sleep.h>

using namespace std::chrono_literals;

async_simple::coro::Lazy<void> main_coro() {
  co_await async_simple::coro::sleep(200ms);
}

int main() {
  corouv::Loop loop;
  corouv::UvExecutor ex(loop.raw());

  corouv::run(ex, main_coro());

  ex.shutdown();
  loop.run(UV_RUN_DEFAULT);
}
```

## Integration model

For third-party async libraries that already expose a nonblocking fd/socket
state machine, the intended integration path is:

1. put the library into nonblocking mode
2. get the fd/socket it currently waits on
3. `co_await corouv::poll::readable(fd)` or `co_await corouv::poll::writable(fd)`
4. re-enter the library state machine

That keeps the core library generic and lets protocol/library adapters stay out
of the base API.

## Notes

- `UvExecutor::shutdown()` should be called on the loop thread before closing
  the loop.
- `blocking::run()` uses libuv's global worker pool. Pool size can be controlled
  with `UV_THREADPOOL_SIZE`.
- HTTP support is currently plain HTTP/1.1 over TCP. TLS / HTTPS and WebSocket
  upgrade handling are not included.

## Tests

Tests build separately from the core library:

```bash
cmake -S tests -B build/tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests -j
ctest --test-dir build/tests --output-on-failure
```

Current tests cover timers, timeout, cancellation, event signaling, blocking
offload (success + exception path), task groups, async mutex/semaphore/queue,
fs roundtrip, runtime control flow, TCP roundtrip, HTTP client/server,
and fd poll awaiters.
