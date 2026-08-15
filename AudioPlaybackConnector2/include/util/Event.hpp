#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <wil/resource.h>

template <typename... Args> class Event {
public:
    using Handler = std::function<void(Args...)>;
    using HandlerId = std::size_t;

    HandlerId operator+=(Handler handler) {
        auto guard = m_lock.lock_exclusive();
        HandlerId id = m_nextId++;
        m_handlers.push_back({id, std::make_shared<Handler>(std::move(handler))});
        return id;
    }

    void operator-=(HandlerId id) {
        auto guard = m_lock.lock_exclusive();
        std::erase_if(m_handlers, [id](const auto& pair) { return pair.first == id; });
    }

    template <typename... CallArgs> void operator()(CallArgs&&... args) const noexcept {
        static_assert(sizeof...(CallArgs) == sizeof...(Args), "Event argument count mismatch");
        try {
            auto snapshot = std::tuple<std::decay_t<Args>...>(std::forward<CallArgs>(args)...);
            std::vector<std::shared_ptr<Handler>> copy;
            {
                auto guard = m_lock.lock_shared();
                copy.reserve(m_handlers.size());
                for (auto& handler : m_handlers) {
                    copy.push_back(handler.second);
                }
            }
            for (auto& handler : copy) {
                try {
                    std::apply([&](auto const&... values) { (*handler)(values...); }, snapshot);
                } catch (...) {
                }
            }
        } catch (...) {
        }
    }

    void clear() {
        auto guard = m_lock.lock_exclusive();
        m_handlers.clear();
    }

private:
    mutable wil::srwlock m_lock;
    std::vector<std::pair<HandlerId, std::shared_ptr<Handler>>> m_handlers;
    HandlerId m_nextId = 1;
};
