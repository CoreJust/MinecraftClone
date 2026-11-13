# Corrections to published snapshots

When the owner authorizes a correction with scoped qualification, base the work
on the currently published snapshot. Preserve the original annotated tag and
leave unfinished snapshot tasks open. Check the corrected source and promotion
tree, obtain a review for that tree, and verify the local Windows x64, macOS
arm64 and Android arm64 packages with basic tests and native runtime evidence.
Bind the artifacts and evidence to the corrected source in the correction
receipt. Use `script/ci/snapshot_correction.py` to validate the receipt and
create the next immutable correction tag. Pass the same receipt and tag through
`MC_SNAPSHOT_CORRECTION_RECEIPT` and `MC_SNAPSHOT_CORRECTION_TAG` to the
pre-push hook. This workflow applies to the scoped correction. Snapshot
finalization retains the aggregate gates above.

### MC-AI-0482 S1 analysis deferrals

MC-AI-0482 EarlyDev BasicBlocks 0.1.1:1 snapshot profile
`script/ci/s1_analysis_profile.py` defers TSan, MSan and both Windows rows.
Seven rows remain mandatory. Only pinned ASan DispatchPing64 evidence permits
`accepted_noise`, retaining its failed row/return code. Pinned cppcheck heap-reference false-positive retains its failed row. Defaults,
later snapshots and minor/major require eleven. Owner trace separates coordinator selections.
