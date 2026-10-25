#pragma once
#include "V9WaterRouting.hpp"

#include <map>

namespace generation::detail
{
struct V9RefinementReach final
{
    int32_t start = 0;
    int32_t target = 0;
    std::array<double, 2> a{}, delta{};
    double length2 = 0, grade0 = 0, grade1 = 0;
    double water_stage0 = 0, water_stage1 = 0, halfwidth = 0, support = 0;
    double discharge_m3_s = 0;
    bool shore_truncated = false;
};
struct V9ProtectedContact final
{
    int32_t vertex_index = 0;
    std::array<double, 2> xy_m{};
    double height_m = 0, radius_m = 0;
};
class V9WaterRefinement final
{
  public:
    static std::expected<V9WaterRefinement, V9WaterFailure>
    create(V9WaterEquilibrium const& water, V9WaterRouting const& routing, double repose_slope = .65);
    [[nodiscard]] bool hasNativeRivers() const { return has_native_rivers_; }
    [[nodiscard]] double base(double x_m, double y_m) const;
    [[nodiscard]] double apply(double native_zero, double native_lod, double x_m, double y_m, uint32_t lod = 3) const;
    [[nodiscard]] std::span<V9RefinementReach const> reaches() const { return reaches_; }
    [[nodiscard]] std::span<V9ProtectedContact const> protectedContacts() const { return contacts_; }

  private:
    struct Ridge
    {
        int32_t v0, v1;
        std::array<double, 2> a, delta;
    };
    bool has_native_rivers_ = false;
    uint32_t n_ = 0;
    double domain_ = 0, cell_ = 0, repose_ = .65;
    int bins_ = 0;
    std::vector<double> bed_, fx_, fy_, stages_;
    std::vector<int32_t> owners_, vertex_owners_;
    std::vector<Ridge> ridges_;
    std::vector<V9RefinementReach> reaches_;
    std::vector<V9ProtectedContact> contacts_;
    std::map<int, std::vector<size_t>> ridge_index_, reach_index_;
    void interpolateSlopes(std::vector<int32_t> const& protected_vertices);
    [[nodiscard]] double interpolate(double x, double y) const;
    [[nodiscard]] double ridgeBarrier(double z, double x, double y) const;
    [[nodiscard]] std::pair<double, int32_t> standing(double x, double y) const;
    [[nodiscard]] int bin(double x, double y) const;
};
} // namespace generation::detail
