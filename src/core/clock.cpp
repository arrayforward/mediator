#include "core/clock.h"

namespace mediator {

int64_t SteadyClock::NowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace mediator
