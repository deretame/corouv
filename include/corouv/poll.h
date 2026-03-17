#pragma once

#include <uv.h>

#include <async_simple/Signal.h>
#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <utility>

#include "corouv/executor.h"
#include "corouv/task.h"
#include "corouv/uv_error.h"

namespace corouv::poll {

namespace detail {

class PollAwaiter {
public:
    PollAwaiter(UvExecutor& ex, uv_os_sock_t fd, int events,
                async_simple::Slot* slot)
        : _ex(&ex), _loop(ex.loop()), _fd(fd), _events(events), _slot(slot) {}

    bool await_ready() const noexcept {
        return async_simple::signalHelper{async_simple::Terminate}.hasCanceled(
            _slot);
    }

    bool await_suspend(std::coroutine_handle<> h) {
        _op = std::make_shared<Op>();
        _op->ex = _ex;
        _op->loop = _loop;
        _op->fd = _fd;
        _op->events = _events;
        _op->slot = _slot;
        _op->h = h;

        if (_slot) {
            const bool ok =
                async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
                    _slot, [op = _op](async_simple::SignalType,
                                      async_simple::Signal*) noexcept {
                        op->cancel_requested.store(true,
                                                   std::memory_order_release);
                        if (auto* ex = op->ex) {
                            (void)ex->schedule(
                                [op]() noexcept { op->cancel_on_loop(); });
                        }
                    });
            if (!ok) {
                return false;
            }
        }

        if (_ex->currentThreadInExecutor()) {
            _op->start_on_loop();
        } else {
            const bool scheduled =
                _ex->schedule([op = _op]() { op->start_on_loop(); });
            if (!scheduled) {
                _op->start_rc = UV_ECANCELED;
                return false;
            }
        }

        return true;
    }

    void await_resume() {
        async_simple::signalHelper{async_simple::Terminate}.checkHasCanceled(
            _slot, "corouv poll canceled");

        auto op = std::move(_op);
        if (!op) {
            throw std::logic_error("corouv::poll missing state");
        }

        if (op->start_rc != 0) {
            throw_uv_error(op->start_rc, "uv_poll_init_socket/uv_poll_start");
        }

        if (op->status < 0) {
            throw_uv_error(op->status, "uv_poll");
        }
    }

private:
    struct Op : std::enable_shared_from_this<Op> {
        UvExecutor* ex = nullptr;
        uv_loop_t* loop = nullptr;
        uv_os_sock_t fd{};
        int events = 0;
        async_simple::Slot* slot = nullptr;
        std::coroutine_handle<> h{};
        uv_poll_t poll{};

        std::atomic<bool> started{false};
        std::atomic<bool> done{false};
        std::atomic<bool> cancel_requested{false};

        int start_rc = 0;
        int status = 0;

        static void on_poll(uv_poll_t* handle, int status, int events) {
            (void)events;
            auto* holder = static_cast<std::shared_ptr<Op>*>(handle->data);
            auto self = *holder;
            self->status = status;
            self->complete_on_loop();
        }

        static void on_close(uv_handle_t* handle) noexcept {
            auto* holder = static_cast<std::shared_ptr<Op>*>(handle->data);
            handle->data = nullptr;
            delete holder;
        }

        void start_on_loop() {
            if (done.load(std::memory_order_acquire)) {
                return;
            }

            poll.data = new std::shared_ptr<Op>(shared_from_this());

            int rc = uv_poll_init_socket(loop, &poll, fd);
            if (rc != 0) {
                start_rc = rc;
                complete_without_handle();
                return;
            }

            rc = uv_poll_start(&poll, events, &Op::on_poll);
            if (rc != 0) {
                start_rc = rc;
                uv_close(reinterpret_cast<uv_handle_t*>(&poll), &Op::on_close);
                complete_without_resume();
                resume();
                return;
            }

            started.store(true, std::memory_order_release);

            if (cancel_requested.load(std::memory_order_acquire)) {
                cancel_on_loop();
            }
        }

        void cancel_on_loop() noexcept { complete_on_loop(); }

        void complete_without_handle() {
            if (done.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            resume();
        }

        void complete_without_resume() {
            done.store(true, std::memory_order_release);
        }

        void complete_on_loop() noexcept {
            if (done.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            if (started.load(std::memory_order_acquire)) {
                uv_poll_stop(&poll);
                uv_close(reinterpret_cast<uv_handle_t*>(&poll), &Op::on_close);
            }

            resume();
        }

        void resume() noexcept {
            if (!h) {
                return;
            }
            try {
                h.resume();
            } catch (...) {
                std::terminate();
            }
        }
    };

    UvExecutor* _ex = nullptr;
    uv_loop_t* _loop = nullptr;
    uv_os_sock_t _fd{};
    int _events = 0;
    async_simple::Slot* _slot = nullptr;
    std::shared_ptr<Op> _op;
};

}  // namespace detail

inline Task<void> wait(UvExecutor& ex, uv_os_sock_t fd, int events) {
    auto* slot = co_await async_simple::coro::CurrentSlot{};
    co_await detail::PollAwaiter(ex, fd, events, slot);
}

inline Task<void> readable(UvExecutor& ex, uv_os_sock_t fd) {
    co_await wait(ex, fd, UV_READABLE);
}

inline Task<void> writable(UvExecutor& ex, uv_os_sock_t fd) {
    co_await wait(ex, fd, UV_WRITABLE);
}

inline Task<void> wait(uv_os_sock_t fd, int events) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
                "corouv::poll::wait requires CurrentExecutor to be UvExecutor");
    }
    co_await wait(*uvex, fd, events);
}

inline Task<void> readable(uv_os_sock_t fd) { co_await wait(fd, UV_READABLE); }
inline Task<void> writable(uv_os_sock_t fd) { co_await wait(fd, UV_WRITABLE); }

}  // namespace corouv::poll
