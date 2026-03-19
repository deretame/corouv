#include <corouv/runtime.h>
#include <corouv/sql.h>

#include <iostream>
#include <string>

corouv::Task<void> sql_basic_case(corouv::UvExecutor& ex) {
    corouv::sql::Connection conn(ex);

    co_await conn.connect("sqlite:///:memory:");
    co_await conn.execute(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");
    co_await conn.execute("INSERT INTO users(id, name) VALUES(?, ?)",
                          {std::int64_t{1}, std::string("alice")});
    co_await conn.execute("INSERT INTO users(id, name) VALUES(?, ?)",
                          {std::int64_t{2}, std::string("bob")});

    const auto result =
        co_await conn.query("SELECT id, name FROM users ORDER BY id");
    std::cout << corouv::sql::result_to_json(result) << std::endl;

    co_await conn.close();
}

int main() {
    corouv::Runtime rt;
    try {
        rt.run(sql_basic_case(rt.executor()));
    } catch (const corouv::sql::Error& err) {
        if (err.backend() == corouv::sql::Backend::SQLite &&
            std::string(err.what()).find("library not found") != std::string::npos) {
            std::cerr
                << "sqlite runtime library not found, skip sql_basic example"
                << std::endl;
            return 0;
        }
        std::cerr << "sql_basic failed: " << err.what() << std::endl;
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "sql_basic failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
