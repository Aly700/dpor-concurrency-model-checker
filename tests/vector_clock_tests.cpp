#include "model/vector_clock.hpp"

#include <cassert>

int main() {
    model::VectorClock a;
    model::VectorClock b;

    a.tick(0);
    b.tick(1);
    assert(!a.happens_before_or_equal(b));
    assert(!b.happens_before_or_equal(a));

    a.join(b);
    assert(b.happens_before_or_equal(a));
    assert(a.get(0) == 1);
    assert(a.get(1) == 1);

    b.join(a);
    b.tick(1);
    assert(!b.happens_before_or_equal(a));
    assert(a.happens_before_or_equal(b));

    return 0;
}
