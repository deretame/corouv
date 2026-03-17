#pragma once

#include <uv.h>

#include <async_simple/Executor.h>

#include <cstdint>
#include <memory>
#include <string>

namespace corouv {

// A libuv-backed async_simple::Executor.
//
// Notes:
// - Designed for a single uv_loop_t thread. `schedule()` is thread-safe.
// - Call `shutdown()` on the loop thread before destroying the loop, otherwise
//   uv_loop_close() will fail due to the internal uv_async_t handle.
class UvExecutor final : public async_simple::Executor {
public:
    explicit UvExecutor(uv_loop_t* loop, std::string name = "uv");
    ~UvExecutor() override;

    UvExecutor(const UvExecutor&) = delete;
    UvExecutor& operator=(const UvExecutor&) = delete;

    uv_loop_t* loop() const noexcept;

    bool schedule(Func func) override;
    bool currentThreadInExecutor() const override;
    async_simple::ExecutorStat stat() const override;

    // Close internal libuv handles. Must be called on the loop thread.
    void shutdown();

protected:
    void schedule(Func func, Duration dur, uint64_t schedule_info,
                  async_simple::Slot* slot) override;

private:
    struct State;
    std::shared_ptr<State> _state;
};

}  // namespace corouv
