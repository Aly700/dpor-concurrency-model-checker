#pragma once

#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace oracle_options {

inline std::size_t program_cap(int argc, char* argv[], std::size_t default_cap) {
    if (argc == 1) {
        return default_cap;
    }
    if (argc != 3 || std::string_view(argv[1]) != "--program-cap") {
        throw std::invalid_argument("usage: ORACLE [--program-cap POSITIVE_INTEGER]");
    }

    const std::string_view text(argv[2]);
    std::size_t cap = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), cap);
    if (error != std::errc{} || end != text.data() + text.size() || cap == 0) {
        throw std::invalid_argument("program cap must be a positive integer");
    }
    return cap;
}

} // namespace oracle_options
