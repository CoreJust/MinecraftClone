#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace generation::detail
{
class V9ClimateSparseLu final
{
public:
    using Row = std::map<int32_t, double>;

    explicit V9ClimateSparseLu(std::span<const Row> rows, std::stop_token cancellation);
    ~V9ClimateSparseLu();
    V9ClimateSparseLu(V9ClimateSparseLu &&) noexcept;
    V9ClimateSparseLu &operator=(V9ClimateSparseLu &&) noexcept;
    V9ClimateSparseLu(V9ClimateSparseLu const &) = delete;
    V9ClimateSparseLu &operator=(V9ClimateSparseLu const &) = delete;

    std::vector<double> solve(std::span<const double> rhs, std::stop_token cancellation);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace generation::detail
