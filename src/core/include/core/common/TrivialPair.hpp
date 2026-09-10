#pragma once

namespace core {

// Unlike std::pair. it is aggregate, which unlocks some optimizations
template<typename FirstTy, typename SecondTy>
struct TrivialPair {
    using First = FirstTy;
    using Second = SecondTy;

    First first;
    Second second;
};

} // namespace core
