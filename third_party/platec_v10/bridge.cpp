#include "bridge.h"

#include "src/lithosphere.hpp"
#include "src/plate.hpp"

#include <stdexcept>

struct mc_v10_platec_handle final
{
    mc_v10_platec_engine::lithosphere engine{
        42L, 256U, 256U, 0.65F, 60U, 0.02F, 1000000U, 0.33F, 2U, 10U,
    };
};

extern "C" mc_v10_platec_handle* mc_v10_platec_create(void)
{
    return new mc_v10_platec_handle;
}

extern "C" void mc_v10_platec_destroy(mc_v10_platec_handle* handle)
{
    delete handle;
}

extern "C" int mc_v10_platec_is_finished(mc_v10_platec_handle const* handle)
{
    return handle->engine.isFinished() ? 1 : 0;
}

extern "C" void mc_v10_platec_update(mc_v10_platec_handle* handle)
{
    handle->engine.update();
}

extern "C" float const* mc_v10_platec_heights(mc_v10_platec_handle const* handle)
{
    return handle->engine.getTopography();
}

extern "C" uint32_t const* mc_v10_platec_plates(mc_v10_platec_handle const* handle)
{
    return handle->engine.getPlatesMap();
}

extern "C" uint32_t mc_v10_platec_copy_directions(
    mc_v10_platec_handle const* handle, mc_v10_platec_direction* output, uint32_t capacity
)
{
    uint32_t const count = handle->engine.getPlateCount();
    if (output == nullptr)
    {
        return count;
    }
    if (capacity < count)
    {
        throw std::invalid_argument("V10 Platec direction capacity is smaller than the active count");
    }
    for (uint32_t id = 0U; id < count; ++id)
    {
        auto const* current_plate = handle->engine.getPlate(id);
        if (current_plate == nullptr)
        {
            throw std::runtime_error("V10 Platec returned an absent current plate");
        }
        auto const direction = current_plate->velocityUnitVector();
        output[id] = {id, direction.x(), direction.y()};
    }
    return count;
}
