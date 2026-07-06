#pragma once

#include "model/checker.hpp"

#include <iosfwd>
#include <string>

namespace cli {

std::string action_text(const model::Action& action);
std::string verdict_of(const model::CheckResult& result);
bool has_bug(const model::CheckResult& result);
void print_report(std::ostream& output, const model::Program& program, const model::CheckResult& result);

} // namespace cli
