#include "corouv/sql.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "corouv/blocking.h"
#include "corouv/poll.h"
#include "corouv/timer.h"

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if __has_include(<mariadb/mysql.h>)
#include <mariadb/mysql.h>
#define COROUV_SQL_HAS_MARIADB_HEADER 1
#endif

namespace corouv::sql {

namespace {

constexpr const char* kMissingDriverMessage =
    "corouv::sql no driver registered for scheme";

std::string to_lower_copy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();

    while (begin < end &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string_view strip_prefix(std::string_view text,
                              std::string_view prefix) noexcept {
    if (text.substr(0, prefix.size()) == prefix) {
        return text.substr(prefix.size());
    }
    return text;
}

std::string decode_percent(std::string_view value) {
    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    };

    std::string out;
    out.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch != '%') {
            out.push_back(ch);
            continue;
        }

        if (i + 2 >= value.size()) {
            throw std::invalid_argument(
                "corouv::sql invalid percent-encoding in URL");
        }
        const int hi = hex_value(value[i + 1]);
        const int lo = hex_value(value[i + 2]);
        if (hi < 0 || lo < 0) {
            throw std::invalid_argument(
                "corouv::sql invalid percent-encoding in URL");
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }

    return out;
}

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    unsigned int parsed = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (ec != std::errc{} || ptr != text.data() + text.size() || parsed > 65535) {
        throw std::invalid_argument("corouv::sql invalid port in URL");
    }

    return static_cast<std::uint16_t>(parsed);
}

std::unordered_map<std::string, std::string> parse_query_options(
    std::string_view query) {
    std::unordered_map<std::string, std::string> out;

    std::size_t start = 0;
    while (start <= query.size()) {
        const auto amp = query.find('&', start);
        const auto token = query.substr(
            start, amp == std::string_view::npos ? query.size() - start
                                                 : amp - start);
        if (!token.empty()) {
            const auto eq = token.find('=');
            const auto key = decode_percent(
                token.substr(0, eq == std::string_view::npos ? token.size() : eq));
            const auto value = eq == std::string_view::npos
                                   ? std::string{}
                                   : decode_percent(token.substr(eq + 1));
            if (!key.empty()) {
                out[key] = value;
            }
        }

        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }

    return out;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);

    for (unsigned char ch : value) {
        switch (ch) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (ch < 0x20) {
                    char buf[7] = {0};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }

    return out;
}

std::string backend_name_copy(Backend backend) {
    return std::string(backend_name(backend));
}

#if defined(__unix__) || defined(__APPLE__)
using SharedLibraryHandle = void*;

SharedLibraryHandle open_shared_library(const std::vector<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle != nullptr) {
            return handle;
        }
    }
    return nullptr;
}

void close_shared_library(SharedLibraryHandle handle) {
    if (handle != nullptr) {
        dlclose(handle);
    }
}

template <class T>
T load_symbol(SharedLibraryHandle handle, const char* name,
              std::string_view driver_name) {
    void* sym = dlsym(handle, name);
    if (sym == nullptr) {
        throw std::runtime_error("corouv::sql " + std::string(driver_name) +
                                 " missing symbol: " + name);
    }
    return reinterpret_cast<T>(sym);
}
#elif defined(_WIN32)
using SharedLibraryHandle = HMODULE;

SharedLibraryHandle open_shared_library(const std::vector<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        HMODULE handle = LoadLibraryA(candidate.c_str());
        if (handle != nullptr) {
            return handle;
        }
    }
    return nullptr;
}

void close_shared_library(SharedLibraryHandle handle) {
    if (handle != nullptr) {
        FreeLibrary(handle);
    }
}

template <class T>
T load_symbol(SharedLibraryHandle handle, const char* name,
              std::string_view driver_name) {
    auto sym = GetProcAddress(handle, name);
    if (sym == nullptr) {
        throw std::runtime_error("corouv::sql " + std::string(driver_name) +
                                 " missing symbol: " + name);
    }
    return reinterpret_cast<T>(sym);
}
#else
using SharedLibraryHandle = void*;

SharedLibraryHandle open_shared_library(const std::vector<std::string>&) {
    return nullptr;
}

void close_shared_library(SharedLibraryHandle) {}

template <class T>
T load_symbol(SharedLibraryHandle, const char*, std::string_view) {
    throw std::runtime_error(
        "corouv::sql dynamic loading is not supported on this platform");
}
#endif

Task<void> wait_socket_events(UvExecutor& ex, uv_os_sock_t sock, bool readable,
                              bool writable) {
    int events = 0;
    if (readable) {
        events |= UV_READABLE;
    }
    if (writable) {
        events |= UV_WRITABLE;
    }
    if (events == 0) {
        co_await corouv::yield_now();
        co_return;
    }
    co_await corouv::poll::wait(ex, sock, events);
}

bool invalid_socket(uv_os_sock_t sock) {
#if defined(_WIN32)
    return sock == static_cast<uv_os_sock_t>(INVALID_SOCKET);
#else
    return sock < 0;
#endif
}

// SQLite driver (runtime-loaded)
struct sqlite3;
struct sqlite3_stmt;
using sqlite3_int64 = long long;
using sqlite3_destructor_type = void (*)(void*);

constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteInteger = 1;
constexpr int kSqliteFloat = 2;
constexpr int kSqliteText = 3;
constexpr int kSqliteBlob = 4;
constexpr int kSqliteNull = 5;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteOpenUri = 0x00000040;

inline sqlite3_destructor_type sqlite_transient() {
    return reinterpret_cast<sqlite3_destructor_type>(-1);
}

class SqliteDriver final : public Driver {
public:
    SqliteDriver() { load_api(); }

    ~SqliteDriver() override {
        close();
        close_shared_library(_lib);
        _lib = nullptr;
    }

    Backend backend() const noexcept override { return Backend::SQLite; }
    std::string_view name() const noexcept override { return "sqlite"; }

    void connect(const ConnectionInfo& info) override {
        close();

        std::string path = info.path;
        if (path.empty()) {
            path = info.database;
        }
        if (path.empty()) {
            path = ":memory:";
        }

        sqlite3* db = nullptr;
        const int rc = _api.open_v2(
            path.c_str(), &db,
            kSqliteOpenReadWrite | kSqliteOpenCreate | kSqliteOpenUri, nullptr);
        if (rc != kSqliteOk) {
            std::string message = "sqlite open failed";
            if (db != nullptr) {
                if (const char* err = _api.errmsg(db); err != nullptr) {
                    message = err;
                }
                _api.close_v2(db);
            }
            throw Error(Backend::SQLite, rc, std::move(message));
        }

        _db = db;
    }

    Result query(std::string_view sql, const Params& params) override {
        if (_db == nullptr) {
            throw std::logic_error("corouv::sql sqlite driver is not connected");
        }

        sqlite3_stmt* stmt = nullptr;
        const int prep = _api.prepare_v2(
            _db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
        if (prep != kSqliteOk) {
            throw Error(Backend::SQLite, prep, sqlite_error_message(prep));
        }

        struct FinalizeGuard {
            Api* api;
            sqlite3_stmt* stmt;
            ~FinalizeGuard() {
                if (api != nullptr && stmt != nullptr) {
                    api->finalize(stmt);
                }
            }
        } guard{&_api, stmt};

        bind_params(stmt, params);

        Result result;
        bool initialized_columns = false;

        while (true) {
            const int step = _api.step(stmt);
            if (step == kSqliteRow) {
                if (!initialized_columns) {
                    initialized_columns = true;
                    const int n = _api.column_count(stmt);
                    result.columns.reserve(static_cast<std::size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const char* col = _api.column_name(stmt, i);
                        result.columns.push_back(col != nullptr ? col : "");
                    }
                }

                const int ncols = _api.column_count(stmt);
                std::vector<Value> row;
                row.reserve(static_cast<std::size_t>(ncols));

                for (int i = 0; i < ncols; ++i) {
                    const int type = _api.column_type(stmt, i);
                    switch (type) {
                        case kSqliteNull:
                            row.push_back(nullptr);
                            break;
                        case kSqliteInteger:
                            row.push_back(static_cast<std::int64_t>(
                                _api.column_int64(stmt, i)));
                            break;
                        case kSqliteFloat:
                            row.push_back(_api.column_double(stmt, i));
                            break;
                        case kSqliteBlob: {
                            const void* blob = _api.column_blob(stmt, i);
                            const int size = _api.column_bytes(stmt, i);
                            Blob bytes;
                            if (blob != nullptr && size > 0) {
                                const auto* begin =
                                    static_cast<const std::uint8_t*>(blob);
                                bytes.assign(begin, begin + size);
                            }
                            row.push_back(std::move(bytes));
                            break;
                        }
                        case kSqliteText:
                        default: {
                            const unsigned char* text = _api.column_text(stmt, i);
                            const int size = _api.column_bytes(stmt, i);
                            if (text == nullptr || size <= 0) {
                                row.push_back(std::string{});
                            } else {
                                row.push_back(std::string(
                                    reinterpret_cast<const char*>(text),
                                    static_cast<std::size_t>(size)));
                            }
                            break;
                        }
                    }
                }

                result.rows.push_back(std::move(row));
                continue;
            }

            if (step == kSqliteDone) {
                break;
            }

            throw Error(Backend::SQLite, step, sqlite_error_message(step));
        }

        result.affected_rows = static_cast<std::int64_t>(_api.changes64(_db));
        result.last_insert_id =
            static_cast<std::int64_t>(_api.last_insert_rowid(_db));
        return result;
    }

    void close() noexcept override {
        if (_db != nullptr) {
            _api.close_v2(_db);
            _db = nullptr;
        }
    }

private:
    struct Api {
        int (*open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
        int (*close_v2)(sqlite3*) = nullptr;
        const char* (*errmsg)(sqlite3*) = nullptr;
        int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**,
                          const char**) = nullptr;
        int (*step)(sqlite3_stmt*) = nullptr;
        int (*finalize)(sqlite3_stmt*) = nullptr;
        int (*bind_null)(sqlite3_stmt*, int) = nullptr;
        int (*bind_int64)(sqlite3_stmt*, int, sqlite3_int64) = nullptr;
        int (*bind_double)(sqlite3_stmt*, int, double) = nullptr;
        int (*bind_text)(sqlite3_stmt*, int, const char*, int,
                         sqlite3_destructor_type) = nullptr;
        int (*bind_blob)(sqlite3_stmt*, int, const void*, int,
                         sqlite3_destructor_type) = nullptr;
        int (*column_count)(sqlite3_stmt*) = nullptr;
        const char* (*column_name)(sqlite3_stmt*, int) = nullptr;
        int (*column_type)(sqlite3_stmt*, int) = nullptr;
        sqlite3_int64 (*column_int64)(sqlite3_stmt*, int) = nullptr;
        double (*column_double)(sqlite3_stmt*, int) = nullptr;
        const unsigned char* (*column_text)(sqlite3_stmt*, int) = nullptr;
        const void* (*column_blob)(sqlite3_stmt*, int) = nullptr;
        int (*column_bytes)(sqlite3_stmt*, int) = nullptr;
        sqlite3_int64 (*changes64)(sqlite3*) = nullptr;
        sqlite3_int64 (*last_insert_rowid)(sqlite3*) = nullptr;
    } _api;

    SharedLibraryHandle _lib = nullptr;
    sqlite3* _db = nullptr;

    void load_api() {
        if (_lib != nullptr) {
            return;
        }

        std::vector<std::string> candidates;
        if (const char* env = std::getenv("COROUV_SQL_SQLITE_LIB")) {
            candidates.emplace_back(env);
        }
#if defined(_WIN32)
        candidates.emplace_back("sqlite3.dll");
#else
        candidates.emplace_back("libsqlite3.so");
        candidates.emplace_back("libsqlite3.so.0");
        candidates.emplace_back("/lib/x86_64-linux-gnu/libsqlite3.so.0");
#endif

        _lib = open_shared_library(candidates);
        if (_lib == nullptr) {
            throw Error(Backend::SQLite, -1,
                        "corouv::sql sqlite library not found");
        }

        _api.open_v2 = load_symbol<decltype(_api.open_v2)>(
            _lib, "sqlite3_open_v2", "sqlite");
        _api.close_v2 = load_symbol<decltype(_api.close_v2)>(
            _lib, "sqlite3_close_v2", "sqlite");
        _api.errmsg =
            load_symbol<decltype(_api.errmsg)>(_lib, "sqlite3_errmsg", "sqlite");
        _api.prepare_v2 = load_symbol<decltype(_api.prepare_v2)>(
            _lib, "sqlite3_prepare_v2", "sqlite");
        _api.step = load_symbol<decltype(_api.step)>(_lib, "sqlite3_step", "sqlite");
        _api.finalize = load_symbol<decltype(_api.finalize)>(
            _lib, "sqlite3_finalize", "sqlite");
        _api.bind_null = load_symbol<decltype(_api.bind_null)>(
            _lib, "sqlite3_bind_null", "sqlite");
        _api.bind_int64 = load_symbol<decltype(_api.bind_int64)>(
            _lib, "sqlite3_bind_int64", "sqlite");
        _api.bind_double = load_symbol<decltype(_api.bind_double)>(
            _lib, "sqlite3_bind_double", "sqlite");
        _api.bind_text = load_symbol<decltype(_api.bind_text)>(
            _lib, "sqlite3_bind_text", "sqlite");
        _api.bind_blob = load_symbol<decltype(_api.bind_blob)>(
            _lib, "sqlite3_bind_blob", "sqlite");
        _api.column_count = load_symbol<decltype(_api.column_count)>(
            _lib, "sqlite3_column_count", "sqlite");
        _api.column_name = load_symbol<decltype(_api.column_name)>(
            _lib, "sqlite3_column_name", "sqlite");
        _api.column_type = load_symbol<decltype(_api.column_type)>(
            _lib, "sqlite3_column_type", "sqlite");
        _api.column_int64 = load_symbol<decltype(_api.column_int64)>(
            _lib, "sqlite3_column_int64", "sqlite");
        _api.column_double = load_symbol<decltype(_api.column_double)>(
            _lib, "sqlite3_column_double", "sqlite");
        _api.column_text = load_symbol<decltype(_api.column_text)>(
            _lib, "sqlite3_column_text", "sqlite");
        _api.column_blob = load_symbol<decltype(_api.column_blob)>(
            _lib, "sqlite3_column_blob", "sqlite");
        _api.column_bytes = load_symbol<decltype(_api.column_bytes)>(
            _lib, "sqlite3_column_bytes", "sqlite");
        _api.changes64 = load_symbol<decltype(_api.changes64)>(
            _lib, "sqlite3_changes64", "sqlite");
        _api.last_insert_rowid = load_symbol<decltype(_api.last_insert_rowid)>(
            _lib, "sqlite3_last_insert_rowid", "sqlite");
    }

    std::string sqlite_error_message(int rc) const {
        if (_db == nullptr || _api.errmsg == nullptr) {
            return "sqlite error " + std::to_string(rc);
        }
        const char* msg = _api.errmsg(_db);
        if (msg == nullptr) {
            return "sqlite error " + std::to_string(rc);
        }
        return msg;
    }

    void bind_params(sqlite3_stmt* stmt, const Params& params) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            const int index = static_cast<int>(i + 1);
            const int rc = std::visit(
                [this, stmt, index](const auto& v) -> int {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::nullptr_t>) {
                        return _api.bind_null(stmt, index);
                    } else if constexpr (std::is_same_v<T, bool>) {
                        return _api.bind_int64(stmt, index, v ? 1 : 0);
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        return _api.bind_int64(
                            stmt, index, static_cast<sqlite3_int64>(v));
                    } else if constexpr (std::is_same_v<T, double>) {
                        return _api.bind_double(stmt, index, v);
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        return _api.bind_text(stmt, index, v.c_str(),
                                              static_cast<int>(v.size()),
                                              sqlite_transient());
                    } else if constexpr (std::is_same_v<T, Blob>) {
                        return _api.bind_blob(
                            stmt, index, v.empty() ? nullptr : v.data(),
                            static_cast<int>(v.size()), sqlite_transient());
                    } else {
                        return kSqliteOk;
                    }
                },
                params[i]);

            if (rc != kSqliteOk) {
                throw Error(Backend::SQLite, rc, sqlite_error_message(rc));
            }
        }
    }
};

// PostgreSQL driver (runtime-loaded) - text protocol via libpq API.
struct PGconn;
struct PGresult;

constexpr int kPgConnOk = 0;
constexpr int kPgCommandOk = 1;
constexpr int kPgTuplesOk = 2;
constexpr int kPgPollingFailed = 0;
constexpr int kPgPollingReading = 1;
constexpr int kPgPollingWriting = 2;
constexpr int kPgPollingOk = 3;
constexpr int kPgPollingActive = 4;

class PostgresDriver final : public AsyncDriver {
public:
    PostgresDriver() { load_api(); }

    ~PostgresDriver() override {
        close();
        close_shared_library(_lib);
        _lib = nullptr;
    }

    Backend backend() const noexcept override { return Backend::PostgreSQL; }
    std::string_view name() const noexcept override { return "pgsql"; }

    Task<void> connect_async(UvExecutor& ex, const ConnectionInfo& info) override {
        close();

        std::string conninfo;
        append_conninfo(conninfo, "host", info.host);
        append_conninfo(conninfo, "port",
                        std::to_string(info.port == 0 ? 5432 : info.port));
        append_conninfo(conninfo, "dbname", info.database);
        append_conninfo(conninfo, "user", info.username);
        append_conninfo(conninfo, "password", info.password);
        for (const auto& [k, v] : info.options) {
            append_conninfo(conninfo, k, v);
        }

        _conn = _api.connect_start(conninfo.c_str());
        if (_conn == nullptr) {
            throw Error(Backend::PostgreSQL, -1,
                        "corouv::sql pgsql allocate connection failed");
        }

        while (true) {
            const int status = _api.connect_poll(_conn);
            if (status == kPgPollingOk) {
                break;
            }
            if (status == kPgPollingFailed) {
                const int conn_status = _api.status(_conn);
                close();
                throw Error(Backend::PostgreSQL, conn_status,
                            connection_error_message("pgsql connect failed"));
            }

            const int sock = _api.socket(_conn);
            if (sock < 0) {
                close();
                throw Error(Backend::PostgreSQL, -1,
                            "corouv::sql pgsql invalid socket");
            }

            if (status == kPgPollingReading) {
                co_await corouv::poll::readable(ex,
                                                static_cast<uv_os_sock_t>(sock));
                continue;
            }
            if (status == kPgPollingWriting) {
                co_await corouv::poll::writable(ex,
                                                static_cast<uv_os_sock_t>(sock));
                continue;
            }
            if (status == kPgPollingActive) {
                co_await corouv::yield_now();
                continue;
            }

            close();
            throw Error(Backend::PostgreSQL, status,
                        "corouv::sql pgsql connect poll failed");
        }

        const int final_status = _api.status(_conn);
        if (final_status != kPgConnOk) {
            close();
            throw Error(Backend::PostgreSQL, final_status,
                        connection_error_message("pgsql connect failed"));
        }

        if (_api.set_nonblocking(_conn, 1) != 0) {
            close();
            throw Error(Backend::PostgreSQL, -1,
                        "corouv::sql pgsql set_nonblocking failed");
        }

        co_return;
    }

    Task<Result> query_async(UvExecutor& ex, std::string sql,
                             Params params) override {
        if (_conn == nullptr) {
            throw std::logic_error("corouv::sql pgsql driver is not connected");
        }

        std::vector<std::string> rendered(params.size());
        std::vector<const char*> values(params.size(), nullptr);
        std::vector<int> lengths(params.size(), 0);
        std::vector<int> formats(params.size(), 0);

        for (std::size_t i = 0; i < params.size(); ++i) {
            if (std::holds_alternative<std::nullptr_t>(params[i])) {
                values[i] = nullptr;
                continue;
            }
            rendered[i] = value_to_string(params[i]);
            values[i] = rendered[i].c_str();
        }

        if (_api.send_query_params(
                _conn, sql.c_str(), static_cast<int>(params.size()), nullptr,
                values.empty() ? nullptr : values.data(),
                lengths.empty() ? nullptr : lengths.data(),
                formats.empty() ? nullptr : formats.data(), 0) != 1) {
            throw Error(Backend::PostgreSQL, -1,
                        connection_error_message("pgsql send query failed"));
        }

        while (true) {
            const int flush_rc = _api.flush(_conn);
            if (flush_rc == 0) {
                break;
            }
            if (flush_rc == 1) {
                const int sock = _api.socket(_conn);
                if (sock < 0) {
                    throw Error(Backend::PostgreSQL, -1,
                                "corouv::sql pgsql invalid socket");
                }
                co_await corouv::poll::writable(ex,
                                                static_cast<uv_os_sock_t>(sock));
                continue;
            }
            throw Error(Backend::PostgreSQL, flush_rc,
                        connection_error_message("pgsql flush failed"));
        }

        Result out;
        while (true) {
            if (_api.consume_input(_conn) != 1) {
                throw Error(Backend::PostgreSQL, -1,
                            connection_error_message("pgsql consume input failed"));
            }

            while (PGresult* result = _api.get_result(_conn)) {
                struct ClearGuard {
                    Api* api;
                    PGresult* result;
                    ~ClearGuard() {
                        if (api != nullptr && result != nullptr) {
                            api->clear(result);
                        }
                    }
                } guard{&_api, result};

                parse_result(out, result);
            }

            if (_api.is_busy(_conn) == 0) {
                break;
            }

            const int sock = _api.socket(_conn);
            if (sock < 0) {
                throw Error(Backend::PostgreSQL, -1,
                            "corouv::sql pgsql invalid socket");
            }
            co_await corouv::poll::readable(ex, static_cast<uv_os_sock_t>(sock));
        }

        co_return out;
    }

    void close() noexcept override {
        if (_conn != nullptr) {
            _api.finish(_conn);
            _conn = nullptr;
        }
    }

private:
    struct Api {
        PGconn* (*connect_start)(const char*) = nullptr;
        int (*connect_poll)(PGconn*) = nullptr;
        int (*socket)(const PGconn*) = nullptr;
        int (*set_nonblocking)(PGconn*, int) = nullptr;
        int (*status)(const PGconn*) = nullptr;
        const char* (*error_message)(const PGconn*) = nullptr;
        void (*finish)(PGconn*) = nullptr;
        int (*send_query_params)(PGconn*, const char*, int,
                                 const unsigned int*, const char* const*,
                                 const int*, const int*, int) = nullptr;
        int (*flush)(PGconn*) = nullptr;
        int (*consume_input)(PGconn*) = nullptr;
        int (*is_busy)(PGconn*) = nullptr;
        PGresult* (*get_result)(PGconn*) = nullptr;
        int (*result_status)(const PGresult*) = nullptr;
        int (*nfields)(const PGresult*) = nullptr;
        int (*ntuples)(const PGresult*) = nullptr;
        const char* (*fname)(const PGresult*, int) = nullptr;
        int (*get_is_null)(const PGresult*, int, int) = nullptr;
        const char* (*get_value)(const PGresult*, int, int) = nullptr;
        const char* (*cmd_tuples)(PGresult*) = nullptr;
        void (*clear)(PGresult*) = nullptr;
    } _api;

    SharedLibraryHandle _lib = nullptr;
    PGconn* _conn = nullptr;

    void load_api() {
        if (_lib != nullptr) {
            return;
        }

        std::vector<std::string> candidates;
        if (const char* env = std::getenv("COROUV_SQL_PG_LIB")) {
            candidates.emplace_back(env);
        }
#if defined(_WIN32)
        candidates.emplace_back("libpq.dll");
        candidates.emplace_back("third_party/postgresql-install/lib/libpq.dll");
        candidates.emplace_back("../third_party/postgresql-install/lib/libpq.dll");
        candidates.emplace_back("../../third_party/postgresql-install/lib/libpq.dll");
#else
        candidates.emplace_back("libpq.so");
        candidates.emplace_back("libpq.so.5");
        candidates.emplace_back("third_party/postgresql-install/lib/libpq.so");
        candidates.emplace_back("../third_party/postgresql-install/lib/libpq.so");
        candidates.emplace_back("../../third_party/postgresql-install/lib/libpq.so");
#endif

        _lib = open_shared_library(candidates);
        if (_lib == nullptr) {
            throw Error(Backend::PostgreSQL, -1,
                        "corouv::sql pgsql library not found");
        }

        _api.connect_start = load_symbol<decltype(_api.connect_start)>(
            _lib, "PQconnectStart", "pgsql");
        _api.connect_poll = load_symbol<decltype(_api.connect_poll)>(
            _lib, "PQconnectPoll", "pgsql");
        _api.socket =
            load_symbol<decltype(_api.socket)>(_lib, "PQsocket", "pgsql");
        _api.set_nonblocking = load_symbol<decltype(_api.set_nonblocking)>(
            _lib, "PQsetnonblocking", "pgsql");
        _api.status =
            load_symbol<decltype(_api.status)>(_lib, "PQstatus", "pgsql");
        _api.error_message = load_symbol<decltype(_api.error_message)>(
            _lib, "PQerrorMessage", "pgsql");
        _api.finish =
            load_symbol<decltype(_api.finish)>(_lib, "PQfinish", "pgsql");
        _api.send_query_params = load_symbol<decltype(_api.send_query_params)>(
            _lib, "PQsendQueryParams", "pgsql");
        _api.flush =
            load_symbol<decltype(_api.flush)>(_lib, "PQflush", "pgsql");
        _api.consume_input = load_symbol<decltype(_api.consume_input)>(
            _lib, "PQconsumeInput", "pgsql");
        _api.is_busy =
            load_symbol<decltype(_api.is_busy)>(_lib, "PQisBusy", "pgsql");
        _api.get_result = load_symbol<decltype(_api.get_result)>(
            _lib, "PQgetResult", "pgsql");
        _api.result_status = load_symbol<decltype(_api.result_status)>(
            _lib, "PQresultStatus", "pgsql");
        _api.nfields =
            load_symbol<decltype(_api.nfields)>(_lib, "PQnfields", "pgsql");
        _api.ntuples =
            load_symbol<decltype(_api.ntuples)>(_lib, "PQntuples", "pgsql");
        _api.fname =
            load_symbol<decltype(_api.fname)>(_lib, "PQfname", "pgsql");
        _api.get_is_null = load_symbol<decltype(_api.get_is_null)>(
            _lib, "PQgetisnull", "pgsql");
        _api.get_value = load_symbol<decltype(_api.get_value)>(
            _lib, "PQgetvalue", "pgsql");
        _api.cmd_tuples = load_symbol<decltype(_api.cmd_tuples)>(
            _lib, "PQcmdTuples", "pgsql");
        _api.clear =
            load_symbol<decltype(_api.clear)>(_lib, "PQclear", "pgsql");
    }

    std::string connection_error_message(std::string_view fallback) const {
        if (_conn == nullptr || _api.error_message == nullptr) {
            return std::string(fallback);
        }
        const char* err = _api.error_message(_conn);
        if (err == nullptr) {
            return std::string(fallback);
        }
        const std::string trimmed = trim_copy(err);
        if (trimmed.empty()) {
            return std::string(fallback);
        }
        return trimmed;
    }

    void parse_result(Result& out, PGresult* result) const {
        const int status = _api.result_status(result);
        if (status != kPgCommandOk && status != kPgTuplesOk) {
            throw Error(Backend::PostgreSQL, status,
                        connection_error_message("pgsql query failed"));
        }

        if (status == kPgTuplesOk) {
            const int nfields = _api.nfields(result);
            const int nrows = _api.ntuples(result);

            out.columns.clear();
            out.rows.clear();
            out.columns.reserve(static_cast<std::size_t>(nfields));
            for (int c = 0; c < nfields; ++c) {
                const char* name = _api.fname(result, c);
                out.columns.push_back(name != nullptr ? name : "");
            }

            out.rows.reserve(static_cast<std::size_t>(nrows));
            for (int r = 0; r < nrows; ++r) {
                std::vector<Value> row;
                row.reserve(static_cast<std::size_t>(nfields));
                for (int c = 0; c < nfields; ++c) {
                    if (_api.get_is_null(result, r, c) != 0) {
                        row.push_back(nullptr);
                    } else {
                        const char* text = _api.get_value(result, r, c);
                        row.push_back(std::string(text != nullptr ? text : ""));
                    }
                }
                out.rows.push_back(std::move(row));
            }
        }

        if (const char* tuples = _api.cmd_tuples(result);
            tuples != nullptr && tuples[0] != '\0') {
            std::int64_t affected = 0;
            const auto [ptr, ec] = std::from_chars(
                tuples, tuples + std::strlen(tuples), affected, 10);
            if (ec == std::errc{} && ptr == tuples + std::strlen(tuples)) {
                out.affected_rows = affected;
            }
        }
    }

    static void append_conninfo(std::string& out, std::string_view key,
                                std::string_view value) {
        if (value.empty()) {
            return;
        }
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(key);
        out.push_back('=');
        out.push_back('\'');
        for (char ch : value) {
            if (ch == '\'' || ch == '\\') {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
        out.push_back('\'');
    }
};
// MySQL driver (runtime-loaded) - basic text query support.
#if defined(COROUV_SQL_HAS_MARIADB_HEADER)
using MYSQL = ::MYSQL;
using MYSQL_RES = ::MYSQL_RES;
using MYSQL_ROW = ::MYSQL_ROW;
#else
struct MYSQL;
struct MYSQL_RES;
using MYSQL_ROW = char**;
#endif

class MySqlDriver final : public AsyncDriver {
public:
    MySqlDriver() { load_api(); }

    ~MySqlDriver() override {
        close();
        close_shared_library(_lib);
        _lib = nullptr;
    }

    Backend backend() const noexcept override { return Backend::MySQL; }
    std::string_view name() const noexcept override { return "mysql"; }

    Task<void> connect_async(UvExecutor& ex, const ConnectionInfo& info) override {
        close();

        _conn = _api.init(nullptr);
        if (_conn == nullptr) {
            throw Error(Backend::MySQL, -1, "corouv::sql mysql init failed");
        }

        if (_api.options != nullptr) {
            const int opt_rc =
                _api.options(_conn, kMysqlOptNonblock, nullptr);
            if (opt_rc != 0) {
                const int code = _api.error_no != nullptr
                                     ? static_cast<int>(_api.error_no(_conn))
                                     : opt_rc;
                const char* err = _api.error != nullptr ? _api.error(_conn) : nullptr;
                std::string message =
                    err != nullptr ? std::string(trim_copy(err))
                                   : std::string("mysql async option setup failed");
                close();
                throw Error(Backend::MySQL, code, std::move(message));
            }
        }

        const char* host = info.host.empty() ? "127.0.0.1" : info.host.c_str();
        const std::uint16_t port = info.port == 0 ? 3306 : info.port;

        MYSQL* connected = nullptr;
        int status = _api.real_connect_start(
            &connected, _conn, host,
            info.username.empty() ? nullptr : info.username.c_str(),
            info.password.empty() ? nullptr : info.password.c_str(),
            info.database.empty() ? nullptr : info.database.c_str(), port, nullptr,
            0UL);

        while (status != 0) {
            co_await wait_status(ex, status);
            status = _api.real_connect_cont(&connected, _conn, status);
        }

        if (connected == nullptr) {
            const int code = _api.error_no != nullptr
                                 ? static_cast<int>(_api.error_no(_conn))
                                 : -1;
            const char* err = _api.error != nullptr ? _api.error(_conn) : nullptr;
            std::string message =
                err != nullptr ? std::string(trim_copy(err))
                               : std::string("mysql connect failed");
            close();
            throw Error(Backend::MySQL, code, std::move(message));
        }

        co_return;
    }

    Task<Result> query_async(UvExecutor& ex, std::string sql,
                             Params params) override {
        if (_conn == nullptr) {
            throw std::logic_error("corouv::sql mysql driver is not connected");
        }
        if (!params.empty()) {
            throw Error(Backend::MySQL, -1,
                        "corouv::sql mysql parameter binding is not implemented");
        }

        int query_rc = 0;
        int status =
            _api.real_query_start(&query_rc, _conn, sql.c_str(), sql.size());
        while (status != 0) {
            co_await wait_status(ex, status);
            status = _api.real_query_cont(&query_rc, _conn, status);
        }

        if (query_rc != 0) {
            const int code = _api.error_no != nullptr
                                 ? static_cast<int>(_api.error_no(_conn))
                                 : query_rc;
            const char* err = _api.error != nullptr ? _api.error(_conn) : nullptr;
            throw Error(Backend::MySQL, code,
                        err != nullptr ? std::string(trim_copy(err))
                                       : std::string("mysql query failed"));
        }

        Result out;
        MYSQL_RES* res = nullptr;
        status = _api.store_result_start(&res, _conn);
        while (status != 0) {
            co_await wait_status(ex, status);
            status = _api.store_result_cont(&res, _conn, status);
        }

        if (res == nullptr) {
            if (_api.field_count(_conn) != 0) {
                const int code = _api.error_no != nullptr
                                     ? static_cast<int>(_api.error_no(_conn))
                                     : -1;
                const char* err = _api.error != nullptr ? _api.error(_conn) : nullptr;
                throw Error(Backend::MySQL, code,
                            err != nullptr ? std::string(trim_copy(err))
                                           : std::string("mysql store_result failed"));
            }
            out.affected_rows = static_cast<std::int64_t>(_api.affected_rows(_conn));
            out.last_insert_id = static_cast<std::int64_t>(_api.insert_id(_conn));
            co_return out;
        }

        struct FreeGuard {
            Api* api;
            MYSQL_RES* res;
            ~FreeGuard() {
                if (api != nullptr && res != nullptr) {
                    api->free_result(res);
                }
            }
        } guard{&_api, res};

        const unsigned int nfields = _api.num_fields(res);
        out.columns.reserve(nfields);
        for (unsigned int i = 0; i < nfields; ++i) {
            out.columns.push_back("col" + std::to_string(i));
        }

        while (true) {
            MYSQL_ROW row = _api.fetch_row(res);
            if (row == nullptr) {
                break;
            }
            const unsigned long* lengths = _api.fetch_lengths(res);

            std::vector<Value> values;
            values.reserve(nfields);
            for (unsigned int i = 0; i < nfields; ++i) {
                if (row[i] == nullptr) {
                    values.push_back(nullptr);
                } else {
                    const std::size_t len =
                        lengths != nullptr ? static_cast<std::size_t>(lengths[i])
                                           : std::strlen(row[i]);
                    values.push_back(std::string(row[i], len));
                }
            }
            out.rows.push_back(std::move(values));
        }

        out.affected_rows = static_cast<std::int64_t>(_api.affected_rows(_conn));
        out.last_insert_id = static_cast<std::int64_t>(_api.insert_id(_conn));
        co_return out;
    }

    void close() noexcept override {
        if (_conn != nullptr) {
            _api.close(_conn);
            _conn = nullptr;
        }
    }

private:
    static constexpr int kMysqlWaitRead = 1;
    static constexpr int kMysqlWaitWrite = 2;
    static constexpr int kMysqlWaitExcept = 4;
    static constexpr int kMysqlWaitTimeout = 8;
    // Enable the async context required by mysql_*_start/*_cont.
#if defined(COROUV_SQL_HAS_MARIADB_HEADER)
    static constexpr unsigned int kMysqlOptNonblock =
        static_cast<unsigned int>(MYSQL_OPT_NONBLOCK);
#else
    // Fallback value from MariaDB mysql.h enum mysql_option.
    static constexpr unsigned int kMysqlOptNonblock = 6000;
#endif

    struct Api {
        int (*library_init)(int, char**, char**) = nullptr;
        void (*library_end)() = nullptr;
        MYSQL* (*init)(MYSQL*) = nullptr;
        int (*options)(MYSQL*, unsigned int, const void*) = nullptr;
        void (*close)(MYSQL*) = nullptr;
        const char* (*error)(MYSQL*) = nullptr;
        unsigned int (*error_no)(MYSQL*) = nullptr;
        unsigned int (*field_count)(MYSQL*) = nullptr;
        unsigned int (*num_fields)(MYSQL_RES*) = nullptr;
        MYSQL_ROW (*fetch_row)(MYSQL_RES*) = nullptr;
        unsigned long* (*fetch_lengths)(MYSQL_RES*) = nullptr;
        void (*free_result)(MYSQL_RES*) = nullptr;
        unsigned long long (*affected_rows)(MYSQL*) = nullptr;
        unsigned long long (*insert_id)(MYSQL*) = nullptr;

        int (*real_connect_start)(MYSQL**, MYSQL*, const char*, const char*,
                                  const char*, const char*, unsigned int,
                                  const char*, unsigned long) = nullptr;
        int (*real_connect_cont)(MYSQL**, MYSQL*, int) = nullptr;

        int (*real_query_start)(int*, MYSQL*, const char*, unsigned long) =
            nullptr;
        int (*real_query_cont)(int*, MYSQL*, int) = nullptr;

        int (*store_result_start)(MYSQL_RES**, MYSQL*) = nullptr;
        int (*store_result_cont)(MYSQL_RES**, MYSQL*, int) = nullptr;

        uv_os_sock_t (*get_socket)(MYSQL*) = nullptr;
        unsigned int (*get_timeout_value)(MYSQL*) = nullptr;
    } _api;

    SharedLibraryHandle _lib = nullptr;
    MYSQL* _conn = nullptr;
    static std::once_flag _library_once;

    Task<void> wait_status(UvExecutor& ex, int status) {
        bool need_read = (status & kMysqlWaitRead) != 0;
        bool need_write = (status & kMysqlWaitWrite) != 0;
        if ((status & kMysqlWaitExcept) != 0) {
            need_read = true;
            need_write = true;
        }

        if (need_read || need_write) {
            if (_api.get_socket == nullptr) {
                throw Error(Backend::MySQL, -1,
                            "corouv::sql mysql_get_socket unavailable");
            }
            const uv_os_sock_t sock = _api.get_socket(_conn);
            if (invalid_socket(sock)) {
                throw Error(Backend::MySQL, -1,
                            "corouv::sql mysql invalid socket");
            }
            co_await wait_socket_events(ex, sock, need_read, need_write);
        }

        if ((status & kMysqlWaitTimeout) != 0) {
            unsigned int timeout_ms = 1;
            if (_api.get_timeout_value != nullptr) {
                const unsigned int v = _api.get_timeout_value(_conn);
                if (v > 0) {
                    timeout_ms = v;
                }
            }
            co_await corouv::sleep_for(std::chrono::milliseconds(timeout_ms));
        }

        if (!(need_read || need_write) && (status & kMysqlWaitTimeout) == 0) {
            co_await corouv::yield_now();
        }

        co_return;
    }

    void load_api() {
        if (_lib != nullptr) {
            return;
        }

        std::vector<std::string> candidates;
        if (const char* env = std::getenv("COROUV_SQL_MYSQL_LIB")) {
            candidates.emplace_back(env);
        }
#if defined(_WIN32)
        candidates.emplace_back("libmysql.dll");
        candidates.emplace_back("libmysqlclient.dll");
        candidates.emplace_back("libmariadb.dll");
#else
        candidates.emplace_back("third_party/mysql-client/lib/libmariadb.so.3");
        candidates.emplace_back("third_party/mysql-client/lib/libmariadb.so");
        candidates.emplace_back("libmysqlclient.so");
        candidates.emplace_back("libmysqlclient.so.21");
        candidates.emplace_back("libmariadb.so");
        candidates.emplace_back("libmariadb.so.3");
#endif

        _lib = open_shared_library(candidates);
        if (_lib == nullptr) {
            throw Error(Backend::MySQL, -1,
                        "corouv::sql mysql library not found");
        }

        try {
            _api.library_init = load_symbol<decltype(_api.library_init)>(
                _lib, "mysql_library_init", "mysql");
        } catch (const std::exception&) {
            _api.library_init = nullptr;
        }
        if (_api.library_init == nullptr) {
            try {
                _api.library_init = load_symbol<decltype(_api.library_init)>(
                    _lib, "mysql_server_init", "mysql");
            } catch (const std::exception&) {
                _api.library_init = nullptr;
            }
        }

        try {
            _api.library_end = load_symbol<decltype(_api.library_end)>(
                _lib, "mysql_library_end", "mysql");
        } catch (const std::exception&) {
            _api.library_end = nullptr;
        }
        if (_api.library_end == nullptr) {
            try {
                _api.library_end = load_symbol<decltype(_api.library_end)>(
                    _lib, "mysql_server_end", "mysql");
            } catch (const std::exception&) {
                _api.library_end = nullptr;
            }
        }

        _api.init = load_symbol<decltype(_api.init)>(_lib, "mysql_init", "mysql");
        _api.options =
            load_symbol<decltype(_api.options)>(_lib, "mysql_options", "mysql");
        _api.close =
            load_symbol<decltype(_api.close)>(_lib, "mysql_close", "mysql");
        _api.error =
            load_symbol<decltype(_api.error)>(_lib, "mysql_error", "mysql");
        _api.error_no = load_symbol<decltype(_api.error_no)>(_lib, "mysql_errno",
                                                              "mysql");
        _api.field_count = load_symbol<decltype(_api.field_count)>(
            _lib, "mysql_field_count", "mysql");
        _api.num_fields = load_symbol<decltype(_api.num_fields)>(
            _lib, "mysql_num_fields", "mysql");
        _api.fetch_row = load_symbol<decltype(_api.fetch_row)>(
            _lib, "mysql_fetch_row", "mysql");
        _api.fetch_lengths = load_symbol<decltype(_api.fetch_lengths)>(
            _lib, "mysql_fetch_lengths", "mysql");
        _api.free_result = load_symbol<decltype(_api.free_result)>(
            _lib, "mysql_free_result", "mysql");
        _api.affected_rows = load_symbol<decltype(_api.affected_rows)>(
            _lib, "mysql_affected_rows", "mysql");
        _api.insert_id = load_symbol<decltype(_api.insert_id)>(
            _lib, "mysql_insert_id", "mysql");

        _api.real_connect_start = load_symbol<decltype(_api.real_connect_start)>(
            _lib, "mysql_real_connect_start", "mysql");
        _api.real_connect_cont = load_symbol<decltype(_api.real_connect_cont)>(
            _lib, "mysql_real_connect_cont", "mysql");
        _api.real_query_start = load_symbol<decltype(_api.real_query_start)>(
            _lib, "mysql_real_query_start", "mysql");
        _api.real_query_cont = load_symbol<decltype(_api.real_query_cont)>(
            _lib, "mysql_real_query_cont", "mysql");
        _api.store_result_start = load_symbol<decltype(_api.store_result_start)>(
            _lib, "mysql_store_result_start", "mysql");
        _api.store_result_cont = load_symbol<decltype(_api.store_result_cont)>(
            _lib, "mysql_store_result_cont", "mysql");
        _api.get_socket = load_symbol<decltype(_api.get_socket)>(
            _lib, "mysql_get_socket", "mysql");

        try {
            _api.get_timeout_value = load_symbol<decltype(_api.get_timeout_value)>(
                _lib, "mysql_get_timeout_value", "mysql");
        } catch (const std::exception&) {
            _api.get_timeout_value = nullptr;
        }

        std::call_once(_library_once, [this] {
            if (_api.library_init == nullptr) {
                return;
            }
            if (_api.library_init(0, nullptr, nullptr) != 0) {
                throw Error(Backend::MySQL, -1,
                            "corouv::sql mysql_library_init failed");
            }
        });
    }
};

std::once_flag MySqlDriver::_library_once;
std::unordered_map<std::string, DriverFactory> g_driver_factories;
std::mutex g_driver_factories_mu;
std::once_flag g_driver_builtins_once;

void ensure_builtin_drivers() {
    std::call_once(g_driver_builtins_once, [] {
        std::lock_guard<std::mutex> lock(g_driver_factories_mu);
        g_driver_factories.emplace(
            "sqlite", [] { return std::make_unique<SqliteDriver>(); });
        g_driver_factories.emplace(
            "pgsql", [] { return std::make_unique<PostgresDriver>(); });
        g_driver_factories.emplace(
            "postgres", [] { return std::make_unique<PostgresDriver>(); });
        g_driver_factories.emplace(
            "postgresql", [] { return std::make_unique<PostgresDriver>(); });
        g_driver_factories.emplace(
            "mysql", [] { return std::make_unique<MySqlDriver>(); });
        g_driver_factories.emplace(
            "mariadb", [] { return std::make_unique<MySqlDriver>(); });
    });
}

}  // namespace

std::optional<std::size_t> Result::column_index(
    std::string_view name) const noexcept {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::reference_wrapper<const Value>> Result::value(
    std::size_t row, std::string_view column) const noexcept {
    if (row >= rows.size()) {
        return std::nullopt;
    }
    const auto index = column_index(column);
    if (!index.has_value() || *index >= rows[row].size()) {
        return std::nullopt;
    }
    return std::cref(rows[row][*index]);
}

Backend backend_from_scheme(std::string_view scheme) noexcept {
    const std::string normalized = to_lower_copy(trim_copy(scheme));
    if (normalized == "sqlite" || normalized == "sqlite3") {
        return Backend::SQLite;
    }
    if (normalized == "pgsql" || normalized == "postgres" ||
        normalized == "postgresql" || normalized == "pg") {
        return Backend::PostgreSQL;
    }
    if (normalized == "mysql" || normalized == "mariadb") {
        return Backend::MySQL;
    }
    return Backend::Unknown;
}

std::string_view backend_name(Backend backend) noexcept {
    switch (backend) {
        case Backend::SQLite:
            return "sqlite";
        case Backend::PostgreSQL:
            return "pgsql";
        case Backend::MySQL:
            return "mysql";
        default:
            return "unknown";
    }
}

ConnectionInfo parse_connection_url(std::string_view url) {
    ConnectionInfo info;

    if (url.empty()) {
        throw std::invalid_argument("corouv::sql connection URL is empty");
    }

    if (url.rfind("sqlite:", 0) == 0) {
        info.backend = Backend::SQLite;
        info.scheme = "sqlite";

        std::string_view path = strip_prefix(url, "sqlite:");
        path = strip_prefix(path, "//");
        if (path == "/:memory:") {
            info.path = ":memory:";
        } else {
            info.path = decode_percent(path);
        }
        if (info.path.empty()) {
            info.path = ":memory:";
        }
        return info;
    }

    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument(
            "corouv::sql connection URL must use scheme://");
    }

    info.scheme = to_lower_copy(url.substr(0, scheme_end));
    info.backend = backend_from_scheme(info.scheme);
    if (info.backend == Backend::Unknown) {
        throw std::invalid_argument("corouv::sql unsupported scheme: " +
                                    info.scheme);
    }

    std::string_view rest = url.substr(scheme_end + 3);
    const auto path_pos = rest.find_first_of("/?");
    std::string_view authority =
        rest.substr(0, path_pos == std::string_view::npos ? rest.size() : path_pos);
    std::string_view tail =
        path_pos == std::string_view::npos ? std::string_view{} : rest.substr(path_pos);

    std::string_view host_port = authority;
    const auto at = authority.rfind('@');
    if (at != std::string_view::npos) {
        const auto userinfo = authority.substr(0, at);
        host_port = authority.substr(at + 1);

        const auto colon = userinfo.find(':');
        if (colon == std::string_view::npos) {
            info.username = decode_percent(userinfo);
        } else {
            info.username = decode_percent(userinfo.substr(0, colon));
            info.password = decode_percent(userinfo.substr(colon + 1));
        }
    }

    if (!host_port.empty()) {
        if (host_port.front() == '[') {
            const auto close = host_port.find(']');
            if (close == std::string_view::npos) {
                throw std::invalid_argument(
                    "corouv::sql invalid bracketed host in URL");
            }
            info.host = std::string(host_port.substr(1, close - 1));
            if (close + 1 < host_port.size()) {
                if (host_port[close + 1] != ':') {
                    throw std::invalid_argument("corouv::sql invalid host:port");
                }
                info.port = parse_port(host_port.substr(close + 2)).value_or(0);
            }
        } else {
            const auto colon = host_port.rfind(':');
            if (colon != std::string_view::npos &&
                host_port.find(':') == host_port.rfind(':')) {
                info.host = std::string(host_port.substr(0, colon));
                info.port = parse_port(host_port.substr(colon + 1)).value_or(0);
            } else {
                info.host = std::string(host_port);
            }
        }
    }

    if (tail.starts_with('/')) {
        const auto q = tail.find('?');
        const auto db = tail.substr(
            1, q == std::string_view::npos ? tail.size() - 1 : q - 1);
        info.database = decode_percent(db);
        if (q != std::string_view::npos) {
            info.options = parse_query_options(tail.substr(q + 1));
        }
    } else if (tail.starts_with('?')) {
        info.options = parse_query_options(tail.substr(1));
    }

    if (info.port == 0) {
        if (info.backend == Backend::PostgreSQL) {
            info.port = 5432;
        } else if (info.backend == Backend::MySQL) {
            info.port = 3306;
        }
    }

    return info;
}

void register_driver(std::string scheme, DriverFactory factory) {
    if (!factory) {
        throw std::invalid_argument("corouv::sql register_driver got empty factory");
    }

    ensure_builtin_drivers();
    const std::string key = to_lower_copy(trim_copy(scheme));
    if (key.empty()) {
        throw std::invalid_argument("corouv::sql register_driver empty scheme");
    }

    std::lock_guard<std::mutex> lock(g_driver_factories_mu);
    g_driver_factories[key] = std::move(factory);
}

void unregister_driver(std::string_view scheme) {
    ensure_builtin_drivers();
    const std::string key = to_lower_copy(trim_copy(scheme));
    if (key.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_driver_factories_mu);
    g_driver_factories.erase(key);
}

bool has_driver(std::string_view scheme) {
    ensure_builtin_drivers();
    const std::string key = to_lower_copy(trim_copy(scheme));

    std::lock_guard<std::mutex> lock(g_driver_factories_mu);
    return g_driver_factories.find(key) != g_driver_factories.end();
}

std::vector<std::string> available_drivers() {
    ensure_builtin_drivers();

    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(g_driver_factories_mu);
    names.reserve(g_driver_factories.size());
    for (const auto& [name, _] : g_driver_factories) {
        (void)_;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string value_to_string(const Value& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                return "NULL";
            } else if constexpr (std::is_same_v<T, bool>) {
                return v ? "1" : "0";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return v;
            } else if constexpr (std::is_same_v<T, Blob>) {
                return "<blob:" + std::to_string(v.size()) + ">";
            } else {
                return {};
            }
        },
        value);
}

std::string value_to_json(const Value& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                return "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return "\"" + json_escape(v) + "\"";
            } else if constexpr (std::is_same_v<T, Blob>) {
                return "\"<blob:" + std::to_string(v.size()) + ">\"";
            } else {
                return "null";
            }
        },
        value);
}

std::string result_to_json(const Result& result) {
    std::string out;
    out.append("{\"columns\":[");
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out.append("\"");
        out.append(json_escape(result.columns[i]));
        out.append("\"");
    }
    out.append("],\"rows\":[");

    for (std::size_t r = 0; r < result.rows.size(); ++r) {
        if (r > 0) {
            out.push_back(',');
        }
        out.push_back('{');
        for (std::size_t c = 0; c < result.rows[r].size(); ++c) {
            if (c > 0) {
                out.push_back(',');
            }
            const std::string key =
                c < result.columns.size() ? result.columns[c]
                                          : ("col" + std::to_string(c));
            out.append("\"");
            out.append(json_escape(key));
            out.append("\":");
            out.append(value_to_json(result.rows[r][c]));
        }
        out.push_back('}');
    }

    out.append("],\"affected_rows\":");
    out.append(std::to_string(result.affected_rows));
    out.append(",\"last_insert_id\":");
    out.append(std::to_string(result.last_insert_id));
    out.push_back('}');
    return out;
}

Connection::Connection(UvExecutor& ex) : _ex(&ex) {}

Task<void> Connection::connect(std::string url) {
    co_await connect(parse_connection_url(url));
}

Task<void> Connection::connect(ConnectionInfo info) {
    if (_ex == nullptr) {
        throw std::logic_error("corouv::sql::Connection missing executor");
    }

    if (info.backend == Backend::Unknown) {
        if (info.scheme.empty()) {
            throw std::invalid_argument(
                "corouv::sql connect requires backend or scheme");
        }
        info.backend = backend_from_scheme(info.scheme);
    }
    if (info.scheme.empty()) {
        info.scheme = backend_name_copy(info.backend);
    }

    std::shared_ptr<Driver> driver = create_driver(info.scheme);
    if (!driver) {
        throw std::logic_error("corouv::sql driver factory returned null");
    }

    if (auto* async_driver = dynamic_cast<AsyncDriver*>(driver.get())) {
        co_await async_driver->connect_async(*_ex, info);
    } else {
        struct ConnectSyncWork {
            std::shared_ptr<Driver> driver;
            ConnectionInfo info;

            void operator()() { driver->connect(info); }
        };

        ConnectSyncWork work;
        work.driver = driver;
        work.info = info;
        co_await corouv::blocking::run(*_ex, std::move(work));
    }

    std::shared_ptr<Driver> old_driver;
    {
        std::lock_guard<std::mutex> lock(_mu);
        old_driver = std::move(_driver);
        _driver = std::move(driver);
        _info = std::move(info);
    }

    if (!old_driver) {
        co_return;
    }

    if (auto* async_driver = dynamic_cast<AsyncDriver*>(old_driver.get())) {
        co_await async_driver->close_async(*_ex);
    } else {
        struct CloseSyncWork {
            std::shared_ptr<Driver> driver;
            void operator()() { driver->close(); }
        };

        CloseSyncWork work;
        work.driver = std::move(old_driver);
        co_await corouv::blocking::run(*_ex, std::move(work));
    }
}

Task<Result> Connection::query(std::string sql, Params params) {
    if (_ex == nullptr) {
        throw std::logic_error("corouv::sql::Connection missing executor");
    }

    std::shared_ptr<Driver> driver;
    {
        std::lock_guard<std::mutex> lock(_mu);
        driver = _driver;
    }

    if (!driver) {
        throw std::logic_error("corouv::sql::Connection is not connected");
    }

    if (auto* async_driver = dynamic_cast<AsyncDriver*>(driver.get())) {
        co_return co_await async_driver->query_async(*_ex, std::move(sql),
                                                     std::move(params));
    }

    struct QuerySyncWork {
        std::shared_ptr<Driver> driver;
        std::string sql;
        Params params;

        Result operator()() { return driver->query(sql, params); }
    };

    QuerySyncWork work;
    work.driver = std::move(driver);
    work.sql = std::move(sql);
    work.params = std::move(params);
    co_return co_await corouv::blocking::run(*_ex, std::move(work));
}

Task<std::int64_t> Connection::execute(std::string sql, Params params) {
    auto result = co_await query(std::move(sql), std::move(params));
    co_return result.affected_rows;
}

Task<void> Connection::close() {
    if (_ex == nullptr) {
        co_return;
    }

    std::shared_ptr<Driver> driver;
    {
        std::lock_guard<std::mutex> lock(_mu);
        driver = std::move(_driver);
        _info = {};
    }

    if (!driver) {
        co_return;
    }

    if (auto* async_driver = dynamic_cast<AsyncDriver*>(driver.get())) {
        co_await async_driver->close_async(*_ex);
    } else {
        struct CloseSyncWork {
            std::shared_ptr<Driver> driver;
            void operator()() { driver->close(); }
        };

        CloseSyncWork work;
        work.driver = std::move(driver);
        co_await corouv::blocking::run(*_ex, std::move(work));
    }
}

bool Connection::is_connected() const noexcept {
    std::lock_guard<std::mutex> lock(_mu);
    return _driver != nullptr;
}

Backend Connection::backend() const noexcept {
    std::lock_guard<std::mutex> lock(_mu);
    return _driver ? _info.backend : Backend::Unknown;
}

std::shared_ptr<Driver> Connection::create_driver(std::string_view scheme) {
    ensure_builtin_drivers();

    const std::string key = to_lower_copy(trim_copy(scheme));
    std::lock_guard<std::mutex> lock(g_driver_factories_mu);
    const auto it = g_driver_factories.find(key);
    if (it == g_driver_factories.end()) {
        throw std::invalid_argument(std::string(kMissingDriverMessage) + ": " +
                                    std::string(scheme));
    }

    std::unique_ptr<Driver> created = it->second();
    if (!created) {
        throw std::logic_error("corouv::sql driver factory returned null");
    }

    return std::shared_ptr<Driver>(std::move(created));
}
}  // namespace corouv::sql
