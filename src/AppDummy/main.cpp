
#include <format>
#include <unordered_map>

static std::string g_static = "Hello!";

auto& GetGreeting() {
    static std::string f_static = "Hello!";
    return f_static;
}

int main(int argc, const char* argv[]) {
    // Create some stack variables for inspecting
    std::unordered_map<int, std::unordered_map<int, std::string>> maps;

    for (auto i = 0; i < 16; i++) {
        for (auto j = 0; j < 100; j++) {
            maps[i][j] = std::format("{}, {}", i, j);
        }
    }

    // Stop debugger here
    __builtin_debugtrap();

    return 0;
}
