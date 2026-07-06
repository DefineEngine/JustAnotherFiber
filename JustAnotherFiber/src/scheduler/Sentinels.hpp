#pragma once

// scheduler/Sentinels.hpp -- disposition tags a fiber hands back to its worker
// via transfer_t::data when it switches off its stack.

#include <cstdint>

namespace jaf::detail {

inline constexpr std::uintptr_t kParked   = 1; // fiber parked itself on a counter
inline constexpr std::uintptr_t kFinished = 2; // job returned; fiber awaits recycling

} // namespace jaf::detail
