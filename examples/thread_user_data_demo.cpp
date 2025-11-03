#include "../core/base_net_thread.h"

#include <iostream>
#include <memory>
#include <string>

namespace {
// Lightweight wrapper so we can instantiate base_net_thread for demonstration.
class DemoNetThread : public base_net_thread {
public:
    using base_net_thread::base_net_thread;
};
} // namespace

int main() {
    DemoNetThread thread;

    // Owned data: thread takes ownership and deletes when cleared.
    thread.set_user_data_owned("owned_counter", new int(42));

    // Shared data: keep shared ownership with callers.
    auto shared_label = std::make_shared<std::string>("shared label");
    thread.set_user_data_shared("shared_label", shared_label);

    // Unowned data: thread only observes; caller must keep it valid.
    int external_flag = 7;
    thread.set_user_data_unowned("observed_flag", &external_flag);

    if (auto value = thread.get_user_data<int>("owned_counter")) {
        std::cout << "Owned counter = " << *value << std::endl;
    }

    if (auto label = thread.get_user_data_ptr<std::string>("shared_label")) {
        std::cout << "Shared label = " << *label << std::endl;
    }

    if (auto observed = thread.get_user_data<int>("observed_flag")) {
        std::cout << "Observed flag = " << *observed << std::endl;
    }

    thread.clear_user_data("owned_counter");   // Drops just the owned counter.
    thread.clear_user_data();                  // Drops remaining entries.

    std::cout << "Thread user-data demo complete.\n";
    return 0;
}
