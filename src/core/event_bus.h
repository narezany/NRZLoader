// Dispatches named events to subscribed mods.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcbe/mod_api.h"

namespace mcbe {

class EventBus {
public:
    void subscribe(const std::string& name, McbeEventHandler handler, void* user_data);

    // Removes every subscription owned by a mod, used when unloading it.
    void unsubscribe_owner(void* owner_token);

    // Returns true when a handler cancelled the event.
    bool dispatch(const char* name, void* payload);

    size_t subscriber_count(const std::string& name) const;

private:
    struct Subscription {
        McbeEventHandler handler;
        void* user_data;
        void* owner_token;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Subscription>> subscriptions_;
};

}  // namespace mcbe
