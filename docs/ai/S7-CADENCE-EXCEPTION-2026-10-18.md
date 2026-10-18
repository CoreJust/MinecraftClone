# S7-only macOS cadence exception

The user approved a documented exception on 2026-10-18 for EarlyDev 0.1.0
Snapshot 7 macOS performance qualification. It allows this snapshot to proceed
despite the recorded cadence shortfall; it does not turn that shortfall into
a performance PASS or close the strict 120 Hz/99.8% objective.

The [approval record](S7-CADENCE-EXCEPTION-2026-10-18.json) preserves the exact
question, answer, root chat identity, reference raw hashes and phase metrics.
It is a local attestation of the conversation, not cryptographic proof of user
authority. The verifier reads it back and pins its bytes to prevent accidental
scope drift. Reference hashes preserve failed measurements; they do not bind
later benign source repairs to an obsolete executable. Every actual receipt
still needs current source, build, executable, package and raw-file provenance.

| Normal v29 phase | Driver rate | Lost display slots |
|---|---:|---:|
| Baseline stationary |119.732655 Hz|8/3601|
| Speed-200 movement |119.699315 Hz|9/3601|
| Recovered stationary |119.666095 Hz|10/3602|

All three phases fail the strict mean-rate and lost-slot requirements.
Radius 256 completeness was separately observed in 22.944 s. The exception waives
only the Mac performance driver's mean-rate and lost-slot thresholds. It adds
no substitute numeric floor. Median one-refresh-slot mode, sufficient samples,
valid driver history, all terrain/radius/physics/network/runtime checks,
provenance and other required gates remain mandatory. It does not waive visual
inspection or visibility requirements owned by other release checks.

Opt in only for this release:

```sh
python3 script/ci/verify_gameplay_gate.py --root . --receipt /absolute/path/aggregate.json \
  --s7-cadence-exception EarlyDev0.1.0-snapshot7
```

Windows policy is independent: add `--s7-windows-secondary` only when that
separately selected policy applies. Otherwise Windows proofs remain mandatory
and strict. The exception is verification-only; capture continues to retain
and report the strict failure. Merge accepts the same explicit option. With no
option, all strict cadence thresholds remain unchanged. Other release names
are rejected, and minor/major qualification must not select this S7 option.

An accepted report exposes top-level and performance `status: accepted_exception`,
while `presentation_timing_status: failed` and all raw/recomputed phase metrics
and failure reasons remain intact. It never labels failed performance passed.
The strict performance obligation continues in [MC-AI-0472](tasks/MC-AI-0472.md),
an unparented basic task depending on S7 [MC-AI-0038](tasks/MC-AI-0038.md),
not an S7-blocking child or an invented Snapshot 8 requirement.
