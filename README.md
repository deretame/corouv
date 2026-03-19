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
- `io::ByteStream` / `io::ByteListener` / `io::DatagramSocket` model wrappers
- `io::File` object wrapper over libuv fs operations
- `io::Directory` and async filesystem path operations
- `copy_file`, `readlink`, `realpath`, `symlink`, and `watch`
- TCP listeners/streams, UDP sockets, UDP options/multicast basics, and async DNS resolution
- blocking work offload via `uv_queue_work`
- cancellation and timeout helpers
- coroutine concurrency primitives (`AsyncMutex`, `AsyncSemaphore`, `AsyncQueue`)
- HTTP/1.1 request/response parsing plus HTTP and HTTPS client/server support
- codec-backed transports via `transport::CodecStream`
- lightweight SQL abstraction (`sqlite` / `pgsql` / `mysql`) with runtime-loaded drivers
- small libuv fs awaiters

Main public entry points live in:

- [`corouv.h`](/home/windy/pack/libuv-test/include/corouv/corouv.h)
- [`executor.h`](/home/windy/pack/libuv-test/include/corouv/executor.h)
- [`file.h`](/home/windy/pack/libuv-test/include/corouv/file.h)
- [`filesystem.h`](/home/windy/pack/libuv-test/include/corouv/filesystem.h)
- [`runtime.h`](/home/windy/pack/libuv-test/include/corouv/runtime.h)
- [`sql.h`](/home/windy/pack/libuv-test/include/corouv/sql.h)
- [`poll.h`](/home/windy/pack/libuv-test/include/corouv/poll.h)
- [`blocking.h`](/home/windy/pack/libuv-test/include/corouv/blocking.h)
- [`net.h`](/home/windy/pack/libuv-test/include/corouv/net.h)
- [`http.h`](/home/windy/pack/libuv-test/include/corouv/http.h)
- [`https.h`](/home/windy/pack/libuv-test/include/corouv/https.h)
- [`io.h`](/home/windy/pack/libuv-test/include/corouv/io.h)
- [`transport.h`](/home/windy/pack/libuv-test/include/corouv/transport.h)
- [`web.h`](/home/windy/pack/libuv-test/include/corouv/web.h)
- [`datagram_channel.h`](/home/windy/pack/libuv-test/include/corouv/datagram_channel.h)
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

## Naming

State-query APIs use `is_*` names:

- `is_open()`
- `is_connected()`

Actual I/O actions keep verb names and are asynchronous, for example:

```cpp
auto file = co_await corouv::io::open(path, 0);
if (!file.is_open()) {
    throw std::runtime_error("open failed");
}

auto stream = co_await corouv::io::connect(ex, "127.0.0.1", 8080);
if (!stream.is_open()) {
    throw std::runtime_error("connect failed");
}
```

The same rule applies across the library:

- `open(...)`, `connect(...)`, `listen(...)`, `bind(...)` perform libuv-backed I/O
- `is_open()` and `is_connected()` only inspect current object state

## High-Level Web API

For code that should stay above raw `http` / `https` wiring, use
`corouv::web`:

```cpp
auto server = corouv::web::ServerBuilder(ex)
    .listen_on("127.0.0.1", 8080)
    .get("/demo", [](corouv::web::Request req) -> corouv::Task<corouv::web::Response> {
        corouv::web::Response resp;
        resp.body = "hello " + req.target;
        co_return resp;
    })
    .not_found([](corouv::web::Request req) -> corouv::Task<corouv::web::Response> {
        corouv::web::Response resp;
        resp.status = 404;
        resp.reason = corouv::web::reason_phrase(404);
        resp.body = "no route for " + req.target;
        resp.keep_alive = false;
        co_return resp;
    })
    .build();

auto client = corouv::web::ClientBuilder(ex).build();
auto response = co_await client.fetch("http://127.0.0.1:8080/demo");
```

This layer chooses HTTP vs HTTPS from the URL scheme on the client side and
switches the server to HTTPS automatically when a TLS config is provided to the
builder. It also supports route-style registration directly on the builder via
`get/post/put/patch/delete_/any/prefix/mount/use/not_found`, and route patterns
support path params like `:id`.

## HTTP API Notes

### I/O timeouts (`IoTimeouts`)

`corouv::http::{ClientOptions,ServerOptions,Connection}` all accept:

- `timeouts.read_headers`: header parse timeout
- `timeouts.read_body`: body read timeout
- `timeouts.write`: write timeout

Timeouts raise `corouv::http::Error` with HTTP-like status:

- `408` for request-side timeout cases
- `504` for response/write timeout cases

### Streaming read/write APIs

For large bodies, prefer stream APIs instead of buffering into one `std::string`:

```cpp
std::string request_body;
auto req = co_await conn.read_request_stream(
    [&](std::string_view chunk) -> corouv::Task<void> {
        request_body.append(chunk);
        co_return;
    });

corouv::http::BodyChunkSource source =
    [i = 0]() mutable -> corouv::Task<std::optional<std::string>> {
        if (i == 0) {
            ++i;
            co_return std::string("part-1");
        }
        if (i == 1) {
            ++i;
            co_return std::string("part-2");
        }
        co_return std::nullopt;
    };
co_await conn.write_response_stream(resp, std::move(source), req->method);
```

`*_stream` variants leave `.body` empty and deliver data through callbacks.

### Informational responses and `Expect: 100-continue`

- `read_response(...)` and `read_response_stream(...)` skip informational `1xx`
  responses by default (except `101 Switching Protocols`, which is returned as
  the terminal response).
- When a request includes `Expect: 100-continue`, server-side request parsing
  automatically emits `100 Continue` before reading the request body.
- Client-side `write_request(...)` / `write_request_stream(...)` honor
  `Expect: 100-continue`: body bytes are only sent after a `100` response is
  received; otherwise the non-`100` response is prefetched and returned on the
  next `read_response(...)`.
- Server-side request parsing rejects unsupported `Expect` tokens with `417
  Expectation Failed`.

### Chunked trailers

- Incoming chunked trailers are parsed into:
  `corouv::http::Request::trailers` and `corouv::http::Response::trailers`.
- Outgoing chunked trailers can be set on `request.trailers` /
  `response.trailers` and are serialized at the end of the chunked stream.

### Message framing hardening

- Incoming HTTP messages that contain both `Transfer-Encoding: chunked` and
  `Content-Length` are rejected to prevent ambiguous framing.
- Unsupported `Transfer-Encoding` values are rejected; current support is
  chunked transfer-coding.
- Request-side conflict maps to `400`; response-side conflict maps to `502`.

### Form and multipart helpers

`http.h` includes helpers for `application/x-www-form-urlencoded`:

- `parse_form_urlencoded(...)`
- `parse_form_urlencoded_request(...)`
- `serialize_form_urlencoded(...)`
- `find_form_value(...)` / `find_form_values(...)`

`multipart.h` includes:

- `multipart::parse(...)`
- `multipart::parse_request(...)`
- `multipart::serialize(...)`

`web::RequestContext` provides convenience wrappers:

- `ctx.parse_form(...)`
- `ctx.parse_multipart(...)`

### Router behavior notes

- `web::Router` route patterns support path params (`:id`) and wildcard tail
  (`*`), plus `mount(...)` prefix stripping.
- `Router::use(...)` middleware wraps mounted routers as well.
- If path matches but HTTP method does not, router returns `405 Method Not Allowed`
  and includes an `Allow` header listing matched methods.

### TLS transport notes

- `transport::TlsClientConfig` / `transport::TlsServerConfig` expose optional
  `handshake_timeout`.
- `transport::CodecStream::close_notify()` sends TLS `close_notify`, flushes
  pending ciphertext, then shuts down the write side.
- `CodecStream::read_some(...)` treats TLS logical close (`close_notify`) as EOF
  and returns `0` without requiring a second TCP-level close edge.

## SQL API Notes

`corouv::sql` provides a small async facade over sync DB client APIs:

- backends: `sqlite`, `pgsql` (`postgres/postgresql` aliases), `mysql` (`mariadb` alias)
- runtime loading via `dlopen`/`LoadLibrary`: no compile-time dependency on DB headers/libraries
- async model:
  - `pgsql`: socket-based nonblocking (`PQconnectPoll`/`PQsendQueryParams` path)
  - `mysql`: socket-based nonblocking (`*_start/*_cont` path)
  - `sqlite`: worker-thread execution via `blocking::run(...)`
- cross-platform dynamic loader paths for Linux/macOS + Windows DLLs

Core types live in [`sql.h`](/home/windy/pack/libuv-test/include/corouv/sql.h):

- `sql::ConnectionInfo` + `parse_connection_url(...)`
- `sql::Connection::connect/query/execute/close`
- `sql::Result`, `sql::Value`, `sql::Params`
- driver extension points: `sql::Driver` / `sql::AsyncDriver`, `register_driver(...)`

Parameter binding support:

- `sqlite`: supported (`?` placeholders)
- `pgsql`: supported via `PQexecParams` text params
- `mysql`: currently text query mode only (params not implemented yet)

### ORM runtime helpers (`sql/orm.h`)

`corouv::sql::orm` now includes a practical query/runtime layer:

- CRUD helpers: `find_all/find_by_pk/insert/update_by_pk/delete_by_pk`
- safer bulk ops: `update_where(...)` / `delete_where(...)`
- composable conditions: `WhereClause` + `where_eq/where_in/where_between/where_and/where_or/...`
- query options: `SelectOptions` (`select_expr`, `where`, `group_by`, `having`, `order_by`, `limit`, `offset`)
- join APIs:
  - generic `join_query(...)`
  - relation helpers `join_one_to_one(...)`, `join_one_to_many(...)`, `join_many_to_many(...)`

The relation helpers are intentionally lightweight: common 1:1, 1:N, N:M are covered,
while very complex graph loading should still use explicit SQL.

### SQL integration smoke test env

`tests/sql_test.cpp` includes optional remote async checks.

- `COROUV_SQL_TEST_PG_URL` (example: `postgresql://postgres:mysecretpassword@127.0.0.1:5432/postgres`)
- `COROUV_SQL_TEST_MYSQL_URL` (example: `mysql://corouv:corouvpass@127.0.0.1:3306/corouv`)

If these env vars are unset, remote DB checks are skipped.

### Disposable DB containers (podman)

PostgreSQL:

```bash
./scripts/run_postgres_podman.sh
```

MySQL:

```bash
./scripts/run_mysql_podman.sh
```

If your host does not provide `libmysqlclient`/`libmariadb`, build a portable
client `.so` via podman:

```bash
./scripts/build_mysql_client_lib.sh
export COROUV_SQL_MYSQL_LIB=/abs/path/to/third_party/mysql-client/lib/libmariadb.so.3
export MARIADB_PLUGIN_DIR=/abs/path/to/third_party/mysql-client/lib/plugin
```

Default MySQL URL:

```text
mysql://corouv:corouvpass@127.0.0.1:3306/corouv
```

### SQL annotations for codegen

Use SQL annotations from [`sql/reflect.h`](/home/windy/pack/libuv-test/include/corouv/sql/reflect.h):

```cpp
struct User {
    SQL_PK SQL_COL("id") std::int64_t id{};
    SQL_COL("name") std::string name;
} SQL_TABLE("users");
```

`SQL_PK` / `SQL_COL` / `SQL_TABLE` prefer future reflection attributes when
available; for libclang codegen, pass `-DCOROUV_SQL_FORCE_CLANG_ANNOTATE=1`.

### libclang generator (`scripts/sql_codegen.py`)

Generate `ModelMeta<T>` specializations:

```bash
./scripts/sql_codegen.py \
  --input include/app/models.h \
  --output include/app/models_sql_meta.gen.h \
  --clang-arg=-Iinclude \
  --clang-arg=-Ithird_party/async_simple
```

Requirements:

- Python package `clang`
- `libclang` shared library available on the host (or pass `--libclang`)
- If your Python bindings and system `libclang` versions differ, explicitly pass
  `--libclang=/abs/path/to/libclang.so` (or `.dll` on Windows).

Plugin hooks are supported with `--plugin`:

- `transform(models, args)` for AST/model post-processing
- `render(models, args, default_render)` for custom output formats

### Lightweight ORM runtime + generator

Runtime helpers live in [`sql/orm.h`](/home/windy/pack/libuv-test/include/corouv/sql/orm.h):

- `orm::find_all(...)`
- `orm::find_by_pk(...)`
- `orm::insert(...)`
- `orm::update_by_pk(...)`
- `orm::delete_by_pk(...)`
- `orm::update_where(...)`
- `orm::delete_where(...)`
- `orm::select(...)` + `orm::SelectOptions`
- `orm::join_query(...)`
- relation helpers: `orm::join_one_to_one(...)`, `orm::join_one_to_many(...)`, `orm::join_many_to_many(...)`
- where builder: `orm::WhereClause`, `where_eq/where_in/where_between/where_and/where_or/...`

Generate model-specific wrappers with `scripts/sql_orm_codegen.py`:

```bash
./scripts/sql_orm_codegen.py \
  --input include/app/models.h \
  --output include/app/models_orm.gen.h \
  --clang-arg=-Iinclude \
  --clang-arg=-Ithird_party/async_simple \
  --libclang=/usr/lib/llvm-19/lib/libclang.so
```

Generated wrappers provide CRUD + relation-aware helpers, including
`select(...)`, `join_with_options(...)`, `one_to_one(...)`, `one_to_many(...)`,
and `many_to_many(...)`. For complex SQL (deep join graphs, dialect-specific
features, heavy analytics), prefer writing SQL directly with `conn.query(...)`.

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
- `corouv_filesystem_ops`
- `corouv_filesystem_watch`
- `corouv_primitives`
- `corouv_udp_echo`
- `corouv_udp_channel`
- `corouv_udp_options`
- `corouv_http_server`
- `corouv_http_client`
- `corouv_http_form_multipart`
- `corouv_web_builder`
- `corouv_sql_basic`

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
./build/examples/corouv_filesystem_ops /tmp/corouv-filesystem-example
./build/examples/corouv_filesystem_watch /tmp/corouv-filesystem-watch-example
./build/examples/corouv_filesystem_watch /tmp/corouv-filesystem-watch-example poll
./build/examples/corouv_primitives
./build/examples/corouv_udp_echo
./build/examples/corouv_udp_channel
./build/examples/corouv_udp_options
./build/examples/corouv_http_server 127.0.0.1 8080 1
./build/examples/corouv_http_client http://127.0.0.1:8080/hello?name=corouv
./build/examples/corouv_http_form_multipart
./build/examples/corouv_web_builder
./build/examples/corouv_sql_basic
```

What each extra example shows:

- [`event.cpp`](/home/windy/pack/libuv-test/examples/event.cpp): `ManualResetEvent::wait/set/reset`
- [`cancel.cpp`](/home/windy/pack/libuv-test/examples/cancel.cpp): `CancellationSource` + `with_cancellation`
- [`task_group.cpp`](/home/windy/pack/libuv-test/examples/task_group.cpp): `make_task_group()` + `spawn()/wait()`
- [`read_file.cpp`](/home/windy/pack/libuv-test/examples/read_file.cpp): high-level `corouv::io::open(...).read_all()`
- [`fs_write_read.cpp`](/home/windy/pack/libuv-test/examples/fs_write_read.cpp): high-level `corouv::io::File` write/rewind/read/close flow
- [`filesystem_ops.cpp`](/home/windy/pack/libuv-test/examples/filesystem_ops.cpp): `create_directories`, `copy_file`, `read_directory`, `rename`, `readlink`, `realpath`, `symlink`, and cleanup with `corouv::io`
- [`filesystem_watch.cpp`](/home/windy/pack/libuv-test/examples/filesystem_watch.cpp): `corouv::io::watch()` over a directory using either `uv_fs_event_t` or `uv_fs_poll_t`, plus filtered `next(...)` and close-to-end-of-stream behavior
- [`concurrency_primitives.cpp`](/home/windy/pack/libuv-test/examples/concurrency_primitives.cpp): `AsyncMutex` + `AsyncSemaphore` + `AsyncQueue`
- [`udp_echo.cpp`](/home/windy/pack/libuv-test/examples/udp_echo.cpp): connected/unconnected UDP send/recv with `corouv::io::DatagramSocket`
- [`udp_channel.cpp`](/home/windy/pack/libuv-test/examples/udp_channel.cpp): datagram queueing with `corouv::io::DatagramChannel`
- [`udp_options.cpp`](/home/windy/pack/libuv-test/examples/udp_options.cpp): broadcast/ttl/multicast configuration on `corouv::net::UdpSocket`
- [`http_server.cpp`](/home/windy/pack/libuv-test/examples/http_server.cpp): HTTP/1.1 server with `corouv::http::Server`
- [`http_client.cpp`](/home/windy/pack/libuv-test/examples/http_client.cpp): plain HTTP/1.1 client via `corouv::http::fetch`
- [`http_form_and_multipart.cpp`](/home/windy/pack/libuv-test/examples/http_form_and_multipart.cpp): end-to-end POST examples for `application/x-www-form-urlencoded` and `multipart/form-data`
- [`web_builder.cpp`](/home/windy/pack/libuv-test/examples/web_builder.cpp): high-level `corouv::web::ServerBuilder` + `corouv::web::ClientBuilder`
- [`sql_basic.cpp`](/home/windy/pack/libuv-test/examples/sql_basic.cpp): `corouv::sql::Connection` with SQLite in-memory CRUD + JSON result output

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

For stream-oriented protocols, the intended high-level model is:

- raw transports stay in `corouv::net` and `corouv::transport`
- generic consumers depend on `corouv::io::ByteStream`, `corouv::io::ByteListener`, `corouv::io::DatagramSocket`, `corouv::io::File`, and `corouv::io::Directory`
- HTTP/1 now uses that byte-stream model instead of depending directly on `TcpStream`
- callers that want a single HTTP/HTTPS entry point can stay on `corouv::web`

## Notes

- `UvExecutor::shutdown()` should be called on the loop thread before closing
  the loop.
- `blocking::run()` uses libuv's global worker pool. Pool size can be controlled
  with `UV_THREADPOOL_SIZE`.
- HTTP support is HTTP/1.1 over the shared byte-stream model. Plain TCP and TLS
  are both supported. WebSocket upgrade handling is not included.

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
