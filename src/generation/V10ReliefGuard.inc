double applyGuardedInlandRelief(detail::V10ProviderState const& value,
    V10NativeBedSample const& native, river_field_candidate::RiverChannelSample const& channel,
    detail::V10FineTerrainLevel const level, double const attenuation, bool const allow_uplift)
{
    double const x = native.xy_m[0U];
    double const y = native.xy_m[1U];
    double uplift = 0.0;
    double const fine = fineTerrain(value).bedWithParentCenterAndUplift(x, y, level, native.foundation_m, uplift);
    validateHeight(fine);
    double const approved = channel.bed.bed_m + attenuation * (fine - native.foundation_m);
    validateHeight(approved);
    if (!allow_uplift || uplift == 0.0 || (uplift < 0.0 && !channel.containing_courses.empty()))
    {
        return approved;
    }

    // These are the same source contacts copied into the prepared river field.
    constexpr double period = detail::V10_FINE_TERRAIN_PERIOD_M;
    constexpr double contact_feather_m = 8.0;
    double contact_weight = 1.0;
    for (auto const& contact : value.water.refinement.protectedContacts())
    {
        double const dx = std::remainder(x - contact.xy_m[0U], period);
        if (std::abs(dx) >= contact.radius_m + contact_feather_m)
        {
            continue;
        }
        double const dy = std::remainder(y - contact.xy_m[1U], period);
        double const distance = std::hypot(dx, dy);
        contact_weight = std::min(contact_weight, smoothUnit((distance - contact.radius_m) / contact_feather_m));
        if (contact_weight == 0.0)
        {
            return approved;
        }
    }

    auto const water = detail::queryV10WaterPoint(value.water, x, y, approved, channel.containing_courses);
    if (!water)
    {
        throw std::runtime_error("private relief water guard failed");
    }
    if (water->ocean || water->lake || water->river)
    {
        return approved;
    }
    double const standing_weight = smoothUnit((approved - native.standing_stage_m - 4.0) / 12.0);
    double const height = approved + attenuation * uplift * contact_weight * standing_weight;
    validateHeight(height);
    return height;
}
