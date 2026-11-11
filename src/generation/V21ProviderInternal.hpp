#pragma once

#include <generation/Generation.hpp>

#include <stop_token>

namespace generation::detail
{

[[nodiscard]]
std::shared_ptr<GlobalAtlas const> prepareV21GlobalAtlas(
    GenerationConfiguration const& configuration,
    std::stop_token cancellation,
    ProgressCallback progress
);

} // namespace generation::detail
