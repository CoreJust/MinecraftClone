# AI development

Read [docs/ai/README.md](docs/ai/README.md), then only the guide and backlog item relevant to the task.

## Scope and branches

- Work on `ai-dev` or `codex/ai-<task>` branches based on it. `ai-main` receives tested snapshots. `dev` and `main` retain the original development line.
- Inspect `git status` first. Preserve existing staged and unstaged work; never use bulk staging to collect unrelated changes.
- The active user request controls scope. Make reasonable reversible decisions locally. Publishing, remote changes, deletion of unrelated work, and history rewriting need explicit authorization.
- A task request authorizes local inspection, implementation, tests, and documentation updates. Do not stop for ceremonial plan or completion approval.

## Small context, fast execution

- Follow [model routing](docs/ai/MODELS.md): Astra/high for major decisions and human-facing guidance; Sol/high for local design and complex work; Terra/high for most coding and short important reviews; Luna/high for clear, verifiable bulk work and broad routine review. Default to Terra/high.
- Use [project skills](docs/ai/SKILLS.md) for task lookup, planning and implementation. Start with `python3 script/ai_tasks.py ready`. Load one task, its direct dependencies, and the relevant [code guide](docs/code/README.md). Search paths/symbols before reading whole files.
- Use independent agents only when they save elapsed work. Give disjoint file ownership, short packets, and no copied conversation. Serialize backlog writes. Workers run focused checks; the coordinator owns full integration and the required review gates.
- Every change needs a task; every commit needs exactly one `Task-ID` trailer, a Luna/high review of its staged tree, and minimal automatic checks. Minor/major completion also needs Terra/high review of the accumulated changes. Follow [the task policy](docs/ai/WORKFLOW.md) and any applicable external publishing gate.
- Keep task notes as current state, not a transcript. Record issues immediately in the [backlog](docs/ai/BACKLOG.md); do not implement unrelated findings.

## Delivery gates

- While iterating: focused tests and `python3 script/ai_check.py --fast`.
- Before reporting completed work: `python3 script/ai_check.py`. This builds, runs tests, and invokes `publish.py --checks-only` with the source version. Only dirty-tree and incorrect snapshot-file checks may be development exceptions; report them.
- Before an aggregate commit: `python3 script/ai_check.py --candidate --level snapshot` (or `minor`/`major`); after its commit use `--strict --level <level>`, plus runtime acceptance. A fast CI pass does not establish GPU or multiplayer correctness.
- When contracts change, update the owning guide; after reading it against source, run `python3 script/ai_docs.py refresh`, then `check`. A hash refresh is not semantic review.
- Complete backlog tasks only with acceptance evidence. Report changed scope, check results, and material limitations concisely.

## Code and tests

Follow [code conventions](docs/CODE_CONVENTIONS.md); preserve surrounding style. Constants use `SNAKE_CASE`, types `CamelCase`, functions `camelCase`, variables/files/namespaces `snake_case`, enum values `CamelCase`; do not rename existing files just to normalize style.
Comments explain a non-obvious contract, constraint, or algorithm, not visible syntax. Wrap either the whole expression on one line, the whole interior on one indented line, or one item per indented line; keep closing delimiters separate for multiline forms.
One test file corresponds to one suite and one source file or tightly related unit. Test observable contracts, common usage, and relevant edge cases; avoid implementation-mirroring assertions. `using namespace` is allowed in tests. Read an available `tests` skill before changing tests.
