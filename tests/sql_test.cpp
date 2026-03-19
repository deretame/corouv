#include <corouv/runtime.h>
#include <corouv/sql.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using corouv::sql::Backend;
using corouv::sql::Connection;
using corouv::sql::ConnectionInfo;
using corouv::sql::Driver;
using corouv::sql::Error;
using corouv::sql::Params;
using corouv::sql::Result;

void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void parse_connection_url_case() {
    {
        const auto info = corouv::sql::parse_connection_url("sqlite:///:memory:");
        require(info.backend == Backend::SQLite, "sql_test: sqlite backend mismatch");
        require(info.scheme == "sqlite", "sql_test: sqlite scheme mismatch");
        require(info.path == ":memory:", "sql_test: sqlite path mismatch");
    }

    {
        const auto info = corouv::sql::parse_connection_url(
            "postgresql://alice:secret@127.0.0.1:5544/demo?"
            "sslmode=disable&application_name=corouv");
        require(info.backend == Backend::PostgreSQL,
                "sql_test: pgsql backend mismatch");
        require(info.scheme == "postgresql", "sql_test: pgsql scheme mismatch");
        require(info.host == "127.0.0.1", "sql_test: pgsql host mismatch");
        require(info.port == 5544, "sql_test: pgsql port mismatch");
        require(info.database == "demo", "sql_test: pgsql db mismatch");
        require(info.username == "alice", "sql_test: pgsql user mismatch");
        require(info.password == "secret", "sql_test: pgsql password mismatch");
        require(info.options.at("sslmode") == "disable",
                "sql_test: pgsql sslmode mismatch");
        require(info.options.at("application_name") == "corouv",
                "sql_test: pgsql app name mismatch");
    }

    {
        const auto info = corouv::sql::parse_connection_url(
            "mysql://root:pw@localhost/demo");
        require(info.backend == Backend::MySQL, "sql_test: mysql backend mismatch");
        require(info.port == 3306, "sql_test: mysql default port mismatch");
    }

    bool threw = false;
    try {
        (void)corouv::sql::parse_connection_url("oracle://localhost/db");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "sql_test: expected unsupported scheme throw");

    threw = false;
    try {
        (void)corouv::sql::parse_connection_url("mysql://localhost:99999/demo");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "sql_test: expected invalid port throw");
}

void registry_case() {
    const auto drivers = corouv::sql::available_drivers();
    require(std::find(drivers.begin(), drivers.end(), "sqlite") != drivers.end(),
            "sql_test: builtin sqlite driver missing");
    require(std::find(drivers.begin(), drivers.end(), "pgsql") != drivers.end(),
            "sql_test: builtin pgsql driver missing");
    require(std::find(drivers.begin(), drivers.end(), "mysql") != drivers.end(),
            "sql_test: builtin mysql driver missing");
    require(corouv::sql::has_driver("postgres"),
            "sql_test: postgres alias missing");
}

class DummyDriver final : public Driver {
public:
    Backend backend() const noexcept override { return Backend::Unknown; }
    std::string_view name() const noexcept override { return "dummy"; }

    void connect(const ConnectionInfo& info) override { connected_ = !info.scheme.empty(); }

    Result query(std::string_view sql, const Params& params) override {
        if (!connected_) {
            throw std::logic_error("sql_test dummy not connected");
        }
        Result result;
        result.columns = {"sql", "param_count"};
        result.rows.push_back(
            {std::string(sql), static_cast<std::int64_t>(params.size())});
        result.affected_rows = static_cast<std::int64_t>(params.size());
        return result;
    }

    void close() noexcept override { connected_ = false; }

private:
    bool connected_{false};
};

corouv::Task<void> custom_driver_case(corouv::UvExecutor& ex) {
    corouv::sql::register_driver("dummy", [] {
        return std::make_unique<DummyDriver>();
    });

    bool cleanup_registered = true;
    try {
        Connection conn(ex);
        ConnectionInfo info;
        info.scheme = "dummy";
        co_await conn.connect(std::move(info));

        const auto result = co_await conn.query(
            "select * from t where id = ? and name = ?",
            {std::int64_t{7}, std::string("alice")});
        require(result.rows.size() == 1, "sql_test: dummy row count mismatch");
        require(result.columns.size() == 2, "sql_test: dummy columns mismatch");

        const auto* sql_text = std::get_if<std::string>(&result.rows[0][0]);
        const auto* count = std::get_if<std::int64_t>(&result.rows[0][1]);
        require(sql_text != nullptr && *sql_text == "select * from t where id = ? and name = ?",
                "sql_test: dummy sql mismatch");
        require(count != nullptr && *count == 2,
                "sql_test: dummy param_count mismatch");

        co_await conn.close();
    } catch (...) {
        if (cleanup_registered) {
            corouv::sql::unregister_driver("dummy");
            cleanup_registered = false;
        }
        throw;
    }

    if (cleanup_registered) {
        corouv::sql::unregister_driver("dummy");
    }
}

corouv::Task<void> sqlite_in_memory_roundtrip_case(corouv::UvExecutor& ex) {
    Connection conn(ex);
    try {
        co_await conn.connect("sqlite:///:memory:");
    } catch (const Error& err) {
        if (err.backend() == Backend::SQLite &&
            std::string(err.what()).find("library not found") != std::string::npos) {
            co_return;
        }
        throw;
    }

    co_await conn.execute(
        "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, age "
        "INTEGER)");
    const auto affected = co_await conn.execute(
        "INSERT INTO users(name, age) VALUES(?, ?)",
        {std::string("alice"), std::int64_t{30}});
    require(affected == 1, "sql_test: sqlite affected rows mismatch");

    const auto selected = co_await conn.query(
        "SELECT id, name, age FROM users WHERE age >= ? ORDER BY id",
        {std::int64_t{18}});
    require(selected.rows.size() == 1, "sql_test: sqlite row count mismatch");
    require(selected.columns.size() == 3, "sql_test: sqlite col count mismatch");

    const auto* name = std::get_if<std::string>(&selected.rows[0][1]);
    const auto* age = std::get_if<std::int64_t>(&selected.rows[0][2]);
    require(name != nullptr && *name == "alice",
            "sql_test: sqlite selected name mismatch");
    require(age != nullptr && *age == 30, "sql_test: sqlite selected age mismatch");

    co_await conn.close();
}

corouv::Task<void> postgres_optional_async_case(corouv::UvExecutor& ex,
                                                std::string url) {
    Connection conn(ex);
    co_await conn.connect(std::move(url));
    const auto result = co_await conn.query("SELECT 1");
    require(!result.rows.empty(), "sql_test: pgsql optional query no rows");
    co_await conn.close();
}

corouv::Task<void> mysql_optional_async_case(corouv::UvExecutor& ex,
                                             std::string url) {
    Connection conn(ex);
    co_await conn.connect(std::move(url));
    const auto result = co_await conn.query("SELECT 1");
    require(!result.rows.empty(), "sql_test: mysql optional query no rows");
    co_await conn.close();
}

void json_render_case() {
    Result result;
    result.columns = {"id", "name", "active"};
    result.rows.push_back(
        {std::int64_t{1}, std::string("alice"), true});
    result.affected_rows = 1;

    const auto json = corouv::sql::result_to_json(result);
    require(json.find("\"name\":\"alice\"") != std::string::npos,
            "sql_test: result_to_json mismatch");
}

}  // namespace

int main() {
    parse_connection_url_case();
    registry_case();
    json_render_case();

    corouv::Runtime rt;
    rt.run(custom_driver_case(rt.executor()));
    rt.run(sqlite_in_memory_roundtrip_case(rt.executor()));
    if (const char* pg_url = std::getenv("COROUV_SQL_TEST_PG_URL");
        pg_url != nullptr && pg_url[0] != '\0') {
        rt.run(postgres_optional_async_case(rt.executor(), std::string(pg_url)));
    }
    if (const char* mysql_url = std::getenv("COROUV_SQL_TEST_MYSQL_URL");
        mysql_url != nullptr && mysql_url[0] != '\0') {
        rt.run(mysql_optional_async_case(rt.executor(), std::string(mysql_url)));
    }
    return 0;
}
