#ifndef SEEDED_STRUCTURAL_WORLD_SURFACE_H
#define SEEDED_STRUCTURAL_WORLD_SURFACE_H

#include "cubic_periodic.h"
#include "centered_control.h"
#include "world_fields.h"
#include "worlds_noise.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

class SeededWorldSurface final
{
public:
    SeededWorldSurface(const std::string& control, const std::string& fields, const int32_t dendry_seed)
        : terrain_(std::make_unique<Noise<CenteredControl>>(
            std::make_unique<CenteredControl>(control), Point2D(0,0), Point2D(32,32),
            Point2D(0,0), Point2D(65536,65536), dendry_seed, .25, 3, .075, 3, 0.0,
            true, false, false, false, false)), fields_(fields),
          anchors4_(terrain_->hierarchyAllocationBudget()), anchors5_(terrain_->hierarchyAllocationBudget()),
          anchors6_(terrain_->hierarchyAllocationBudget())
    {}

    SeededWorldSurface(std::span<const double> control, std::span<const double> fields, const int32_t dendry_seed)
        : terrain_(std::make_unique<Noise<CenteredControl>>(
            std::make_unique<CenteredControl>(control), Point2D(0,0), Point2D(32,32),
            Point2D(0,0), Point2D(65536,65536), dendry_seed, .25, 3, .075, 3, 0.0,
            true, false, false, false, false)), fields_(fields),
          anchors4_(terrain_->hierarchyAllocationBudget()), anchors5_(terrain_->hierarchyAllocationBudget()),
          anchors6_(terrain_->hierarchyAllocationBudget())
    {}

    std::array<double, 13> evaluate(double x, double y, const uint32_t lod) const
    {
        if (lod > 3 || !std::isfinite(x) || !std::isfinite(y)) {
            throw std::invalid_argument("invalid hierarchy query");
        }
        x = canonical(x); y = canonical(y);
        std::array<double, 13> result{};
        result[0] = fields_.sample(x,y,0) + dendry(x,y,3) - fields_.sample(x,y,1);
        for (uint32_t l = 4; l <= lod + 3; ++l) {
            const uint32_t index = l - 4;
            const double spacing = 2048.0 / (uint32_t{1} << (l - 1));
            const auto period = static_cast<uint32_t>(65536.0 / spacing);
            const double raw = delta(x,y,l);
            const double filtered = cubicPeriodic(x / spacing,y / spacing,period,
                [&](uint32_t ax,uint32_t ay) { return anchor(ax,ay,l); });
            const double bound = .5 * spacing * std::max(0.0, fields_.sample(x,y,2));
            const double innovation = bound > 0 ? bound * std::tanh((raw-filtered)/bound) : 0;
            result[index+1] = result[index] + innovation;
            result[4+index*3] = raw;
            result[5+index*3] = filtered;
            result[6+index*3] = innovation;
        }
        return result;
    }
    const Noise<CenteredControl>& terrain() const { return *terrain_; }
    std::array<uint64_t, 3> anchorEntries() const
    {
        return {anchors4_.entries(), anchors5_.entries(), anchors6_.entries()};
    }
private:
    static double canonical(const double value)
    {
        const double result = std::fmod(value,65536.0);
        return result < 0 ? result + 65536.0 : result;
    }
    double dendry(const double x, const double y, const uint32_t level) const
    {
        return 2000.0 * terrain_->evaluateHierarchy(x/2048.0,y/2048.0,level);
    }
    double delta(const double x, const double y, const uint32_t level) const
    {
        const double cx=canonical(x); const double cy=canonical(y);
        return dendry(cx,cy,level) - dendry(cx,cy,level-1);
    }
    double anchor(const uint32_t x, const uint32_t y, const uint32_t level) const
    {
        const auto build = [&]() {
            const double spacing = 2048.0 / (uint32_t{1} << (level-1));
            static constexpr std::array<double, 3> WEIGHTS{1,2,1};
            double result = 0;
            for (uint32_t j=0;j<3;++j) {
                for (uint32_t i=0;i<3;++i) {
                    const double sx=(static_cast<int64_t>(x)+i-1)*spacing;
                    const double sy=(static_cast<int64_t>(y)+j-1)*spacing;
                    result += WEIGHTS[i]*WEIGHTS[j]*delta(sx,sy,level)/16.0;
                }
            }
            return result;
        };
        if (level==4) { return *anchors4_.get(x,y,build); }
        if (level==5) { return *anchors5_.get(x,y,build); }
        return *anchors6_.get(x,y,build);
    }
    std::unique_ptr<Noise<CenteredControl>> terrain_;
    const RegisteredWorldFields fields_;
    mutable BoundedCellMemo<double,256> anchors4_;
    mutable BoundedCellMemo<double,512> anchors5_;
    mutable BoundedCellMemo<double,1024> anchors6_;
};
#endif
