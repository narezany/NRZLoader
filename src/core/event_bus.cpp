#include "event_bus.h"

#include "log.h"

namespace mcbe {

void EventBus::subscribe(const std::string& name, McbeEventHandler handler, void* user_data) {
    if (handler == nullptr) return;
    std::lock_guard<std::mutex> guard(mutex_);
    subscriptions_[name].push_back(Subscription{handler, user_data, user_data});
}

void EventBus::unsubscribe_owner(void* owner_token) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& [name, list] : subscriptions_) {
        for (auto it = list.begin(); it != list.end();) {
            it = it->owner_token == owner_token ? list.erase(it) : it + 1;
        }
    }
}

bool EventBus::dispatch(const char* name, void* payload) {
    std::vector<Subscription> snapshot;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto entry = subscriptions_.find(name);
        if (entry == subscriptions_.end()) return false;
        // Copy so a handler may subscribe or unsubscribe without invalidating
        // the iteration, and so we never hold the lock across mod code.
        snapshot = entry->second;
    }

    McbeEvent event;
    event.name = name;
    event.data = payload;
    event.cancelled = 0;

    for (const Subscription& subscription : snapshot) {
        subscription.handler(&event, subscription.user_data);
    }
    return event.cancelled != 0;
}

size_t EventBus::subscriber_count(const std::string& name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto entry = subscriptions_.find(name);
    return entry == subscriptions_.end() ? 0 : entry->second.size();
}

}  // namespace mcbe
