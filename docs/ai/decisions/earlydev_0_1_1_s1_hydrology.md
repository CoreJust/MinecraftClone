# S1 — Terrain elaboration and water topology

After [geology](earlydev_0_1_1_s1_geology.md), separate solid bed, hydrological
spill surface and water surface. These data shape the map and constrain future
water; S1 still generates Air above solid terrain. This is a bounded
terrain/hydrology approximation, with explicit sediment and lake-water limits.

## Ocean, lakes and drainage

Choose sea offset once from median tectonic height; keep sea at geological zero
through shaping. Label below-sea cells with periodic eight-connectivity,
including diagonal seam neighbors. The largest component is the ocean; stable
cell-ID ordering breaks ties. Other below-sea depressions are inland basins,
not automatic sea outlets. The all-land fallback uses the single global minimum
as a closed sink, never an arbitrary map edge.

Run minimax Priority-Flood from ocean cells using `(spill_height,cell_id)` heap
ordering. Store original bed, filled spill height, flood parent and rank
separately. Nonflat cells route to the steepest lower filled neighbor using
cardinal/diagonal distances. Flat cells use the earlier flood parent. A
receiver must lower filled height or flood rank, so accumulation terminates.
Accumulate runoff in reverse rank; propagate terminal outlet IDs in forward
rank. Label lake components where spill exceeds original bed outside the sea.
Store original lake bed, common water/spill surface and outlet identity; filling
solid terrain to the spill height would erase the lake.

Initial runoff is uniform. Provisional climate then supplies
`Q_local=clip(0.05+max(P,0),0,8)` in normalized model flux. River cells exceed
`max(8,N²*0.0005)` accumulated flux outside sea/lakes. Final rainfall reroutes
final discharge. This single-flow D8 graph is deliberately bounded; diagonal
channel alignment is a visible approximation.

[Barnes et al.](https://arxiv.org/abs/1511.04463) supports Priority-Flood
filling/routing. Ocean seeding and toroidal connectivity are our adaptation;
the paper does not establish these terrain coefficients or this game's cost.

## Bounded terrain shaping

Run three route/erosion passes, rerouting each time. Stream-power-like incision
is proportional to `sqrt(Q/threshold)*positive_receiver_slope`, with a 256-world-unit reference
distance and diagonal distance correction, capped at
0.035 geological km/pass. A periodic Gaussian of channel incision widens
valleys with 384-world-unit sigma; cap combined ordinary cutting at 0.045 km.
Exclude lake interiors/ocean beds. Reroute after changes.

Use cold upstream catchments from provisional climate (`T<−6 °C`, source
height >0.35 km). Near the coast, sufficiently large cold discharge widens low
outlet valleys and may cut their bed below sea. Restrict outlet height to
≤0.24 km and coast reach to 1,536 world units. Cap all cutting at 0.08 km/pass.
A fjord candidate requires at least two adjacent newly submerged, cold-carved
route cells, now connected to the selected ocean and touching the original sea.
Retain the carving provenance through final climate/rerouting. A candidate is
not proof of historical glaciers or a realistic fjord cross-section.

Harbour candidates are diagnosed natural inlets: connected sea, nearby land on
opposite bearings within 768 world units, depth ≥0.05 geological km, inner
land fraction 0.30–0.75 within 512 units, outer fraction ≤0.55 within 2,048
units and at least 0.08 below the inner fraction. This describes sheltered
geometry; waves, tides, navigable entrances and shipping suitability are
unmodelled. It must not be presented as a port simulation.

All widths use world units scaled by atlas spacing, not fixed cell counts.
Continents/islands/seas/peninsulas come from crust, elevation and connectivity;
coastal carving elaborates them rather than independently inventing a second map.

## Limits and verification

Lakes are assumed filled to their spill surface; dry endorheic water balance,
deposition, sediment conservation and wave/coastal erosion are not simulated.
Incised material is removed in a detachment-limited sketch. These omissions do
not prevent deterministic topography/maps, but ecological/water-volume claims
would require additional algorithms. No complex glacier simulator is implied.

[Landlab's Fastscape source](https://landlab.csdms.io/_modules/landlab/components/stream_power/fastscape_stream_power.html)
supports the stream-power mechanism, not our explicit step size.
[USGS](https://pubs.usgs.gov/of/2004/1216/text.html) motivates cold glacial
submergence for fjord classification rather than random coastal notches.
Test sinks, flats, periodic components, steepest paths, basin/outlet identity,
precipitation response, retained lake beds and actual candidate criteria. Record
feature counts and failed/rejected maps in the
[experiment evidence](earlydev_0_1_1_s1_experiments.md).
