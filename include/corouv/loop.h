#pragma once

#include <uv.h>

#include <stdexcept>
#include <string>

namespace corouv {

class Loop final {
public:
    Loop() {
        const int rc = uv_loop_init(&_loop);
        if (rc != 0) {
            throw std::runtime_error(std::string("uv_loop_init: ") +
                                     uv_strerror(rc));
        }
        _inited = true;
    }

    ~Loop() { (void)close(); }

    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    uv_loop_t* raw() noexcept { return &_loop; }
    const uv_loop_t* raw() const noexcept { return &_loop; }

    int run(uv_run_mode mode = UV_RUN_DEFAULT) { return uv_run(&_loop, mode); }

    void stop() noexcept { uv_stop(&_loop); }

    int close() noexcept {
        if (!_inited || _closed) {
            return 0;
        }

        int rc = uv_loop_close(&_loop);
        if (rc == UV_EBUSY) {
            // Best-effort cleanup. This is mainly to keep examples ergonomic.
            // For production, prefer closing handles explicitly.
            uv_walk(
                &_loop,
                [](uv_handle_t* h, void*) {
                    if (!uv_is_closing(h)) {
                        uv_close(h, [](uv_handle_t*) {});
                    }
                },
                nullptr);
            uv_run(&_loop, UV_RUN_DEFAULT);
            rc = uv_loop_close(&_loop);
        }

        if (rc == 0) {
            _closed = true;
        }
        return rc;
    }

private:
    uv_loop_t _loop{};
    bool _inited{false};
    bool _closed{false};
};

}  // namespace corouv
