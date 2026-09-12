# Scenario quickstart

Save the following as `move-right.mcscenario`:

```text
scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
begin
input alice 1 0
wait 10
input alice 0 0
expect player alice position 5 4
end
```

This is also the exact [canonical sample](../../scenarios/canonical_sample.mcscenario).
It declares format version 1, selects `flat2d-v1`, records a seed, places one
player at `(4, 4)`, holds rightward input for five authoritative steps, stops
it, then checks the boundary-five position.

Read the script in this order:

1. The header (`scenario 1`) selects the source format.
2. The profile and seed establish deterministic configuration.
3. One or more `player` declarations create boundary-zero setup.
4. `begin` opens the finite command body; `end` closes both the body and file.
5. Commands run in written order. `input` changes an actor’s requested
   direction for following steps, `wait` advances exactly its stated number of
   steps, and `expect` reads the current boundary without advancing time.

Use [the language reference](REFERENCE.md) for all allowed syntax and
[execution model](EXECUTION.md) for the exact timing rule. Copy examples from
[the scenarios directory](../../scenarios/README.md); every `.mcscenario` file
there is intended to parse as-is.
