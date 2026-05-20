# Convention of how versions are organized

Minecraft clone versions come like the following: 
`Epoch.Major.Minor:snapshot`

A new epoch means large breaking changes and a new stage of game development. They are given complete names (e.g. the first epoch 0 is `EarlyDev`).

A new major version means significant portion of content that makes up a complete set of new mechanics.

A new minor version adds some separate contents and improvements, but those may be incomplete.

A snapshot is a minimal delivery unit with a few changes or improvements. It might be relatively unstable.

Snapshots are encoded like `i(yy.mm.dd)` where `i` is the snapshot index within that minor version.

Each minor version must have a separate release with executables.
