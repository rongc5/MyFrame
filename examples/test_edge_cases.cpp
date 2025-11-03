#include "../core/base_net_thread.h"
#include <iostream>
#include <memory>

namespace {
class DemoNetThread : public base_net_thread {
public:
    using base_net_thread::base_net_thread;
};
}

int main() {
    DemoNetThread thread;

    std::cout << "=== Test: get_user_data_ptr on unowned data ===\n";
    int external = 100;
    thread.set_user_data_unowned("unowned", &external);

    // Try to get as shared_ptr - should return nullptr because owns=false
    auto ptr = thread.get_user_data_ptr<int>("unowned");
    if (ptr) {
        std::cout << "✗ Unexpected: got shared_ptr for unowned data\n";
        return 1;
    } else {
        std::cout << "✓ Correct: get_user_data_ptr returns nullptr for unowned data\n";
    }

    // But get_user_data (raw pointer) should work
    if (auto* raw = thread.get_user_data<int>("unowned")) {
        std::cout << "✓ Correct: get_user_data returns " << *raw << " for unowned data\n";
    } else {
        std::cout << "✗ Unexpected: get_user_data failed for unowned data\n";
        return 1;
    }

    std::cout << "\n=== Test: nullptr handling ===\n";
    thread.set_user_data_owned<int>("test", nullptr);
    if (thread.get_user_data<int>("test") == nullptr) {
        std::cout << "✓ Correct: Setting nullptr clears the entry\n";
    }

    std::cout << "\n=== Test: Type safety ===\n";
    thread.set_user_data_owned("number", new int(42));

    // This would be unsafe but should compile
    int* num = thread.get_user_data<int>("number");
    std::cout << "✓ Value: " << *num << "\n";

    std::cout << "\nAll edge case tests passed!\n";
    return 0;
}
