#include "model/action.hpp"

namespace model {

bool may_conflict(const Action& lhs, const Action& rhs) {
    if (lhs.address.empty() || rhs.address.empty() || lhs.address != rhs.address) {
        return false;
    }
    return lhs.kind == ActionKind::Write || rhs.kind == ActionKind::Write;
}

} // namespace model
