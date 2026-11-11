#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace generation::detail
{

inline constexpr uint32_t V10_FINE_TERRAIN_SEED = 42U;
inline constexpr double V10_FINE_TERRAIN_PERIOD_M = 65'536.0;

enum class V10FineTerrainLevel : uint8_t
{
    L2,
    L3,
};

using V10FineTerrainBedSampler = std::function<double(double, double)>;

struct V10FineTerrainParent final
{
    uint32_t seed = 0U;
    double period_m = 0.0;
    std::string identity_sha256;
    V10FineTerrainBedSampler bed;
};

struct V10FineTerrainReceipt final
{
    std::string source_id;
    std::string parent_identity_sha256;
    std::string field_identity_sha256;
    std::string l2_identity_sha256;
    std::string l3_identity_sha256;
    std::string plan_sha256;
    size_t prepared_parent_queries = 0U;
    size_t mountain_site_count = 0U;
    size_t valley_network_count = 0U;
    size_t regional_site_count = 0U;
    std::string regional_draws_sha256;
    std::string regional_feature_sha256;
};

class V10FineTerrain final
{
public:
    explicit V10FineTerrain(V10FineTerrainParent parent);
    ~V10FineTerrain();

    V10FineTerrain(V10FineTerrain const&) = delete;
    V10FineTerrain& operator=(V10FineTerrain const&) = delete;
    V10FineTerrain(V10FineTerrain&&) noexcept;
    V10FineTerrain& operator=(V10FineTerrain&&) noexcept;

    [[nodiscard]] double bed(double x_m, double y_m, V10FineTerrainLevel level) const;
    // parent_center_height is the parent bed at (x_m, y_m) after periodic wrapping.
    [[nodiscard]] double bedWithParentCenter(
        double x_m, double y_m, V10FineTerrainLevel level, double parent_center_height) const;
    [[nodiscard]] double bedWithParentCenterAndUplift(
        double x_m, double y_m, V10FineTerrainLevel level, double parent_center_height, double& uplift) const;
    [[nodiscard]] V10FineTerrainReceipt const& receipt() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl const> m_impl;
};

} // namespace generation::detail
