# Convention of how versions are organized

Minecraft clone versions come like the following: 
`Epoch.Major.Minor:snapshot`

A new epoch means large breaking changes and a new stage of game development. They are given complete names (e.g. the first epoch 0 is `EarlyDev`).

A new major version means significant portion of content that makes up a complete set of new mechanics.

A new minor version adds some separate contents and improvements, but those may be incomplete.

A snapshot is a minimal delivery unit with a few changes or improvements. It might be relatively unstable.

Snapshots are encoded like `i(yy.mm.dd)` where `i` is the snapshot index within that minor version.

# Versions in git

There are 2 main branches: `main` and `dev`. `dev` receives all the latest commits. When a snapshot is complete, it is merged into `main` with:

```
git merge --no-ff dev -m "$MAJOR_NAME $EPOCH.$MAJOR.$MINOR:$SNAPSHOT_INDEX(YY.MM.DD)"
git tag -a $MAJOR_NAME/$EPOCH.$MAJOR.$MINOR/$SNAPSHOT_INDEX_YY.MM.DD -m "$MAJOR_NAME $EPOCH.$MAJOR.$MINOR $MINOR_NAME snapshot $SNAPSHOT_INDEX(YY.MM.DD)"
```

Each minor version must have a separate release with executables.
