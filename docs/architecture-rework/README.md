# Architecture Rework

Goal: preserve required behavior while replacing the current ~25,000-line/138-file structure with cohesive owners and lower total comprehension cost. The 16,000–19,000-line/55–70-file target is directional, never a quota.

## Required reading

Load only what the task needs:

| Task | Read |
|---|---|
| Any implementation/review | [AI rules](ai-rules.md) + [product decisions](decisions.md) |
| Structure, ownership, dependencies | Above + [architecture](architecture.md) |
| Planning/implementing a migration phase | Above + active phase in [migration](migration.md) |
| Testing, completion, release claim | Relevant section in [verification](verification.md) |
| Performance work | [baseline](performance-baseline-2026-08-22.md) + [measurement script](measure-performance-baseline.ps1) |

MUST NOT load the performance baseline for unrelated work. MUST NOT infer approval for a `Gxx` decision in `decisions.md`.

## Completion

- Startup-to-tray flow is traceable through `AppRuntime` → `AppController` → feature owners → UI.
- Every mutable state/concurrency mechanism has one owner.
- UI and CLI use the same application commands.
- All `Pxx` decisions pass [verification](verification.md).
- Old owners, duplicate pipelines, and indefinite adapters are removed.
- Documentation matches shipping code.
