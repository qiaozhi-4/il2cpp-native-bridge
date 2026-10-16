#pragma once

#include <iostream>
#include <string>
#include <stdexcept>
#include <string_view>

namespace test_support {

inline void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

} /// namespace test_support
