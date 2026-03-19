#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "corouv/executor.h"
#include "corouv/task.h"
#include "corouv/sql/model.h"
#include "corouv/sql/reflect.h"

namespace corouv::sql {

enum class Backend {
    Unknown = 0,
    SQLite,
    PostgreSQL,
    MySQL,
};

using Blob = std::vector<std::uint8_t>;
using Value =
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Blob>;
using Params = std::vector<Value>;

struct ConnectionInfo {
    Backend backend{Backend::Unknown};
    std::string scheme;
    std::string host;
    std::uint16_t port{0};
    std::string database;
    std::string username;
    std::string password;
    std::string path;
    std::unordered_map<std::string, std::string> options;
};

struct Result {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::int64_t affected_rows{-1};
    std::int64_t last_insert_id{-1};

    [[nodiscard]] std::size_t row_count() const noexcept { return rows.size(); }

    [[nodiscard]] std::optional<std::size_t> column_index(
        std::string_view name) const noexcept;

    [[nodiscard]] std::optional<std::reference_wrapper<const Value>> value(
        std::size_t row, std::string_view column) const noexcept;
};

class Error : public std::runtime_error {
public:
    Error(Backend backend, int code, std::string message)
        : std::runtime_error(std::move(message)),
          _backend(backend),
          _code(code) {}

    [[nodiscard]] Backend backend() const noexcept { return _backend; }
    [[nodiscard]] int code() const noexcept { return _code; }

private:
    Backend _backend{Backend::Unknown};
    int _code{0};
};

class Driver {
public:
    virtual ~Driver() = default;

    virtual Backend backend() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual void connect(const ConnectionInfo& info) = 0;
    virtual Result query(std::string_view sql, const Params& params) = 0;
    virtual void close() noexcept = 0;
};

class AsyncDriver : public Driver {
public:
    ~AsyncDriver() override = default;

    void connect(const ConnectionInfo&) override {
        throw std::logic_error(
            "corouv::sql async driver requires connect_async()");
    }

    Result query(std::string_view, const Params&) override {
        throw std::logic_error(
            "corouv::sql async driver requires query_async()");
    }

    virtual Task<void> connect_async(UvExecutor& ex,
                                     const ConnectionInfo& info) = 0;
    virtual Task<Result> query_async(UvExecutor& ex, std::string sql,
                                     Params params) = 0;
    virtual Task<void> close_async(UvExecutor&) {
        close();
        co_return;
    }
};

using DriverFactory = std::function<std::unique_ptr<Driver>()>;

Backend backend_from_scheme(std::string_view scheme) noexcept;
std::string_view backend_name(Backend backend) noexcept;

ConnectionInfo parse_connection_url(std::string_view url);

void register_driver(std::string scheme, DriverFactory factory);
void unregister_driver(std::string_view scheme);
bool has_driver(std::string_view scheme);
std::vector<std::string> available_drivers();

std::string value_to_string(const Value& value);
std::string value_to_json(const Value& value);
std::string result_to_json(const Result& result);

class Connection {
public:
    explicit Connection(UvExecutor& ex);

    Task<void> connect(std::string url);
    Task<void> connect(ConnectionInfo info);
    Task<Result> query(std::string sql, Params params = {});
    Task<std::int64_t> execute(std::string sql, Params params = {});
    Task<void> close();

    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] Backend backend() const noexcept;
    [[nodiscard]] const ConnectionInfo& info() const noexcept { return _info; }

private:
    [[nodiscard]] std::shared_ptr<Driver> create_driver(std::string_view scheme);

    UvExecutor* _ex = nullptr;
    std::shared_ptr<Driver> _driver;
    ConnectionInfo _info;
    mutable std::mutex _mu;
};

}  // namespace corouv::sql
