#pragma once

#include <async_simple/Signal.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/LazyLocalBase.h>

#include <memory>
#include <type_traits>
#include <utility>

#include "corouv/task.h"

namespace corouv {

class CancellationToken;

class CancellationSource {
public:
    CancellationSource() : _signal(async_simple::Signal::create()) {}

    [[nodiscard]] CancellationToken token() const;

    bool cancel() const noexcept {
        return static_cast<bool>(
            _signal->emits(async_simple::SignalType::Terminate));
    }

    bool canceled() const noexcept {
        return _signal->state() & async_simple::SignalType::Terminate;
    }

private:
    std::shared_ptr<async_simple::Signal> _signal;
};

class CancellationToken {
public:
    CancellationToken() = default;

    bool canceled() const noexcept {
        return _signal &&
               (_signal->state() & async_simple::SignalType::Terminate);
    }

private:
    explicit CancellationToken(std::shared_ptr<async_simple::Signal> signal)
        : _signal(std::move(signal)) {}

    std::shared_ptr<async_simple::Signal> _signal;

    friend class CancellationSource;

    template <class T>
    friend Task<T> with_cancellation(Task<T>, CancellationToken);
};

inline CancellationToken CancellationSource::token() const {
    return CancellationToken(_signal);
}

namespace detail {

struct CancellationLocal : async_simple::coro::LazyLocalBase {
    inline static char tag;

    explicit CancellationLocal(std::shared_ptr<async_simple::Signal> token_signal,
                               async_simple::Signal* outer_signal)
        : LazyLocalBase(&tag), combined(async_simple::Signal::create()) {
        _slot = std::make_unique<async_simple::Slot>(
            combined.get(), async_simple::SignalType::All);

        if (outer_signal != nullptr) {
            outer_forward = std::make_unique<async_simple::Slot>(
                outer_signal, async_simple::SignalType::All);
            outer_forward->chainedSignal(combined.get());
            if (outer_signal->state() != async_simple::SignalType::None) {
                combined->emits(outer_signal->state());
            }
        }

        if (token_signal != nullptr) {
            token_forward = std::make_unique<async_simple::Slot>(
                token_signal.get(), async_simple::SignalType::All);
            token_forward->chainedSignal(combined.get());
            if (token_signal->state() != async_simple::SignalType::None) {
                combined->emits(token_signal->state());
            }
        }
    }

    static bool classof(const async_simple::coro::LazyLocalBase* base) {
        return base->getTypeTag() == &tag;
    }

    std::shared_ptr<async_simple::Signal> combined;
    std::unique_ptr<async_simple::Slot> outer_forward;
    std::unique_ptr<async_simple::Slot> token_forward;
};

}  // namespace detail

template <class T>
Task<T> with_cancellation(Task<T> task, CancellationToken token) {
    auto* outer_slot = co_await async_simple::coro::CurrentSlot{};
    auto local = std::make_shared<detail::CancellationLocal>(
        token._signal, outer_slot ? outer_slot->signal() : nullptr);

    if constexpr (std::is_void_v<T>) {
        co_await std::move(task).setLazyLocal(local);
        co_return;
    } else {
        co_return co_await std::move(task).setLazyLocal(local);
    }
}

}  // namespace corouv
