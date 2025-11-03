#include "../core/base_net_thread.h"
#include <iostream>
#include <memory>
#include <string>

class TrackedObject {
public:
    std::string name;
    static int created;
    static int destroyed;

    TrackedObject(const std::string& n) : name(n) {
        ++created;
        std::cout << "TrackedObject('" << name << "') created [total: " << created << "]\n";
    }

    ~TrackedObject() {
        ++destroyed;
        std::cout << "TrackedObject('" << name << "') destroyed [total: " << destroyed << "]\n";
    }
};

int TrackedObject::created = 0;
int TrackedObject::destroyed = 0;

namespace {
class DemoNetThread : public base_net_thread {
public:
    using base_net_thread::base_net_thread;
};
}

int main() {
    std::cout << "=== Test 1: Owned objects should be deleted ===\n";
    {
        DemoNetThread thread;
        thread.set_user_data_owned("obj1", new TrackedObject("owned-1"));
        thread.set_user_data_owned("obj2", new TrackedObject("owned-2"));

        // Manual clear of one
        thread.clear_user_data("obj1");
        std::cout << "After clearing obj1\n";

        // Thread destructor should clean up obj2
        std::cout << "About to destroy thread (should destroy obj2)\n";
    }
    std::cout << "After thread destroyed\n\n";

    std::cout << "=== Test 2: Shared objects with multiple references ===\n";
    {
        auto shared = std::make_shared<TrackedObject>("shared-1");
        std::cout << "shared use_count = " << shared.use_count() << "\n";

        {
            DemoNetThread thread;
            thread.set_user_data_shared("shared", shared);
            std::cout << "After set_user_data_shared, use_count = " << shared.use_count() << "\n";
        }

        std::cout << "After thread destroyed, use_count = " << shared.use_count() << "\n";
        std::cout << "About to destroy shared ptr\n";
    }
    std::cout << "After shared destroyed\n\n";

    std::cout << "=== Test 3: Replacing owned data ===\n";
    {
        DemoNetThread thread;
        thread.set_user_data_owned("key", new TrackedObject("first"));
        std::cout << "After setting first\n";

        thread.set_user_data_owned("key", new TrackedObject("second"));
        std::cout << "After replacing with second (first should be destroyed)\n";
    }
    std::cout << "After thread destroyed (second should be destroyed)\n\n";

    std::cout << "=== Summary ===\n";
    std::cout << "Total created: " << TrackedObject::created << "\n";
    std::cout << "Total destroyed: " << TrackedObject::destroyed << "\n";

    if (TrackedObject::created == TrackedObject::destroyed) {
        std::cout << "✓ No memory leaks detected!\n";
        return 0;
    } else {
        std::cout << "✗ Memory leak: " << (TrackedObject::created - TrackedObject::destroyed)
                  << " objects not destroyed\n";
        return 1;
    }
}
