# AI development entrypoint

The AI line implements the existing [product roadmap](../ROADMAP.md). The owner supplies direction and product choices; agents perform scoped implementation, validation, and maintenance.

| Need | Read or run |
|---|---|
| Project commands | [Skills and deterministic commands](SKILLS.md) |
| Choose work | `python3 script/ai_tasks.py ready`; [backlog](BACKLOG.md) |
| Choose a model | [routing and cost policy](MODELS.md); `python3 script/ai_run.py --dry-run terra` |
| Execute or resume | [workflow](WORKFLOW.md); [task template](tasks/TEMPLATE.md) |
| Find implementation | [code map](../code/README.md); search the generated index only as needed |
| Plan or close a version | [release policy](RELEASES.md), [current task hierarchy](tasks/MC-AI-0034.md), [delivery roadmap](ROADMAP.md) |
| Build and verify | [build guide](../BUILD.md); `python3 script/ai_check.py` |
| Keep docs focused | [maintenance](MAINTENANCE.md) |
| Extend automation | [tools, hooks, and future skills](AUTOMATION.md) |
| Commit a task | [commit protocol](COMMITS.md) |
| Deliver a snapshot | [branch/version convention](../VERSION_CONVENTION.md) |

## First session

```sh
git status --short --branch
python3 script/ai_setup.py
python3 script/ai_tasks.py ready
```

The project Codex config selects `gpt-5.6-terra`; an explicit task/app model override wins. Start a new session to reload project instructions/config. Other agents use the same Markdown entrypoints and Python tools.

## Initial state

`ai-dev` forks `dev` at `79b2687`; `ai-main` forks `main` at `28010a9`. They were created locally on 2026-09-09. Existing staged/unstaged source work was preserved and is not a released snapshot. The initial debug build and 136 CTest tests passed; interactive rendering and cross-platform checks were not established by that run.

No game features are implemented by this setup. The tools validate documentation, task state, and delivery evidence; they do not guarantee autonomous correctness. Start by establishing snapshot 3's runtime baseline, then work through small acceptance-driven slices.
