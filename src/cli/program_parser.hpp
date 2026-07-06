#pragma once

#include "model/action.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace cli {

class ParseError : public std::runtime_error {
public:
    ParseError(std::size_t line, const std::string& message);

    std::size_t line() const;

private:
    std::size_t line_;
};

model::Program parse_program_file(const std::string& path);
model::Program parse_program_text(const std::string& text);
model::Schedule parse_schedule_file(const std::string& path);
std::string render_program(const model::Program& program);

} // namespace cli
