#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mc_v10_platec_handle;

struct mc_v10_platec_direction
{
    uint32_t plate_id;
    float x;
    float y;
};

// Private fixed seed42/256 reference factory; the V10 source wrapper serializes
// calls because upstream owns per-engine-family scratch. No legacy engine types.
struct mc_v10_platec_handle* mc_v10_platec_create(void);
void mc_v10_platec_destroy(struct mc_v10_platec_handle* handle);
int mc_v10_platec_is_finished(struct mc_v10_platec_handle const* handle);
void mc_v10_platec_update(struct mc_v10_platec_handle* handle);
float const* mc_v10_platec_heights(struct mc_v10_platec_handle const* handle);
uint32_t const* mc_v10_platec_plates(struct mc_v10_platec_handle const* handle);

// Null output queries the current active count. Otherwise capacity must cover
// that count; copies engine values without retaining caller memory.
uint32_t mc_v10_platec_copy_directions(
    struct mc_v10_platec_handle const* handle,
    struct mc_v10_platec_direction* output, uint32_t capacity
);

#ifdef __cplusplus
}
#endif
