#pragma once

#include <libpq-fe.h>

#include <async_simple/Executor.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "corouv/poll.h"
#include "corouv/task.h"
#include "corouv/timer.h"

namespace corouv_examples::libpq {

class Error : public std::runtime_error {
public:
    explicit Error(std::string msg) : std::runtime_error(std::move(msg)) {}
};

class Result {
public:
    Result() = default;
    explicit Result(PGresult* res) : _res(res) {}
    ~Result() {
        if (_res) {
            PQclear(_res);
        }
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    Result(Result&& other) noexcept : _res(std::exchange(other._res, nullptr)) {}
    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            if (_res) {
                PQclear(_res);
            }
            _res = std::exchange(other._res, nullptr);
        }
        return *this;
    }

    PGresult* raw() const noexcept { return _res; }
    ExecStatusType status() const noexcept {
        return _res ? PQresultStatus(_res) : PGRES_FATAL_ERROR;
    }
    const char* error_message() const noexcept {
        return _res ? PQresultErrorMessage(_res) : "null PGresult";
    }

private:
    PGresult* _res{nullptr};
};

class Connection {
public:
    Connection() = default;
    explicit Connection(PGconn* conn) : _conn(conn) {}
    ~Connection() {
        if (_conn) {
            PQfinish(_conn);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
        : _conn(std::exchange(other._conn, nullptr)) {}
    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            if (_conn) {
                PQfinish(_conn);
            }
            _conn = std::exchange(other._conn, nullptr);
        }
        return *this;
    }

    PGconn* raw() const noexcept { return _conn; }

    uv_os_sock_t socket() const {
        if (!_conn) {
            throw Error("libpq connection is null");
        }
        const int fd = PQsocket(_conn);
        if (fd < 0) {
            throw Error(PQerrorMessage(_conn));
        }
        return static_cast<uv_os_sock_t>(fd);
    }

    void set_nonblocking() {
        if (PQsetnonblocking(_conn, 1) != 0) {
            throw Error(PQerrorMessage(_conn));
        }
    }

private:
    PGconn* _conn{nullptr};
};

inline void throw_conn_error(PGconn* conn, const char* what) {
    std::string msg = what;
    msg += ": ";
    msg += conn ? PQerrorMessage(conn) : "null PGconn";
    throw Error(std::move(msg));
}

inline corouv::Task<Connection> connect(corouv::UvExecutor& ex,
                                        std::string conninfo) {
    PGconn* conn = PQconnectStart(conninfo.c_str());
    if (!conn) {
        throw Error("PQconnectStart failed");
    }

    Connection wrapped(conn);
    wrapped.set_nonblocking();

    while (true) {
        const PostgresPollingStatusType s = PQconnectPoll(wrapped.raw());
        switch (s) {
            case PGRES_POLLING_READING:
                co_await corouv::poll::readable(ex, wrapped.socket());
                break;
            case PGRES_POLLING_WRITING:
                co_await corouv::poll::writable(ex, wrapped.socket());
                break;
            case PGRES_POLLING_OK:
                co_return std::move(wrapped);
            case PGRES_POLLING_FAILED:
                throw_conn_error(wrapped.raw(), "PQconnectPoll");
            case PGRES_POLLING_ACTIVE:
                co_await corouv::yield_now();
                break;
        }
    }
}

inline corouv::Task<Connection> connect(std::string conninfo) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<corouv::UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "libpq example integration requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await connect(*uvex, std::move(conninfo));
}

inline corouv::Task<void> flush(Connection& conn, corouv::UvExecutor& ex) {
    while (true) {
        const int rc = PQflush(conn.raw());
        if (rc == 0) {
            co_return;
        }
        if (rc == -1) {
            throw_conn_error(conn.raw(), "PQflush");
        }
        co_await corouv::poll::writable(ex, conn.socket());
    }
}

inline corouv::Task<void> consume_until_ready(Connection& conn,
                                              corouv::UvExecutor& ex) {
    while (PQisBusy(conn.raw())) {
        co_await corouv::poll::readable(ex, conn.socket());
        if (PQconsumeInput(conn.raw()) == 0) {
            throw_conn_error(conn.raw(), "PQconsumeInput");
        }
    }
}

inline corouv::Task<std::vector<Result>> exec(Connection& conn,
                                              corouv::UvExecutor& ex,
                                              std::string_view sql) {
    if (PQsendQuery(conn.raw(), std::string(sql).c_str()) == 0) {
        throw_conn_error(conn.raw(), "PQsendQuery");
    }

    co_await flush(conn, ex);
    co_await consume_until_ready(conn, ex);

    std::vector<Result> out;
    while (true) {
        PGresult* res = PQgetResult(conn.raw());
        if (res == nullptr) {
            break;
        }
        out.emplace_back(res);
    }
    co_return out;
}

inline corouv::Task<std::vector<Result>> exec(Connection& conn,
                                              std::string_view sql) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<corouv::UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "libpq example integration requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await exec(conn, *uvex, sql);
}

}  // namespace corouv_examples::libpq
