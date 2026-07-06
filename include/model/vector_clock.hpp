#pragma once

#include <cstdint>
#include <map>

namespace model {

class VectorClock {
public:
    void tick(std::uint32_t tid);
    void join(const VectorClock& other);
    [[nodiscard]] bool happens_before_or_equal(const VectorClock& other) const;
    [[nodiscard]] std::uint64_t get(std::uint32_t tid) const;

private:
    std::map<std::uint32_t, std::uint64_t> clock_;
};

} // namespace model
