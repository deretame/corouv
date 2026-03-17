#include "libpq_support.h"

#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

namespace upq = corouv_examples::libpq;

corouv::Task<void> demo_libpq(corouv::UvExecutor& ex, std::string conninfo) {
    auto conn = co_await corouv::with_timeout(upq::connect(ex, std::move(conninfo)),
                                              5s);
    auto results = co_await corouv::with_timeout(
        upq::exec(conn, ex, "select 1::int as value"), 5s);

    if (results.empty()) {
        throw std::runtime_error("libpq query returned no result objects");
    }

    PGresult* res = results.front().raw();
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        throw std::runtime_error(PQresultErrorMessage(res));
    }

    if (PQntuples(res) < 1 || PQnfields(res) < 1) {
        throw std::runtime_error("unexpected empty query result");
    }

    std::cout << "[libpq] value=" << PQgetvalue(res, 0, 0) << "\n";
}

int main(int argc, char** argv) {
    const char* env = std::getenv("COROUV_PG_CONNINFO");
    std::string conninfo =
        env ? env
            : "host=127.0.0.1 port=5432 user=postgres password=mysecretpassword "
              "dbname=postgres";

    if (argc > 1) {
        conninfo = argv[1];
    }

    corouv::Runtime rt;
    rt.run(demo_libpq(rt.executor(), conninfo));
    return 0;
}
