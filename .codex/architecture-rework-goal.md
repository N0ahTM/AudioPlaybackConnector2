Role: Lead architect and integration owner.

Goal: Consolidate the documented migration into Phases 0-7 and complete remaining Phases 3-7 sequentially on `rework/architecture`, preserving all locked product behavior, passing every phase gate, receiving independent approval after every phase, and leaving a clean, locally PR-ready branch.

Source of truth:
- Start at `docs/architecture-rework/README.md`; follow its loading table.
- Preserve every P01-P10 decision.
- Never infer approval for G01-G07.
- P03 requires the tray Connecting animation.
- Code size, file count, function length, and abstraction count are never goals.

Consolidated phase order:
- Phase 0: Baseline
- Phase 1: Models and API
- Phase 2: Settings
- Phase 3: Devices
- Phase 4: Application and Control (4A Application, then 4B Control)
- Phase 5: Resources and Tray (5A Resources, then 5B Tray)
- Phase 6: UI and Supporting Services (6A Notifications, 6B Settings UI/services, 6C Logging/crash)
- Phase 7: Cleanup and Final Verification

Workflow:
- Follow `migration.md` phases in dependency order.
- Before each phase, characterize affected legacy behavior and tests.
- Delegate narrow normal slices to `phase_worker`.
- Delegate device/resource/settings-persistence/pipe/lifecycle concurrency slices to `concurrency_worker`.
- Parallelize read-only exploration; never let write agents edit overlapping files or the same worktree.
- Use a separate worktree and one focused commit for each independent write task.
- Integrate worker commits into this branch; do not push or open a PR without explicit approval.
- Run the phase-specific `verification.md` checks before deleting the legacy owner.
- After each phase, ask a fresh `final_reviewer` to review the phase diff against its base. Fix all blocking findings and repeat review.
- Do not start a later consolidated phase until the preceding phase is implemented, its legacy owners are deleted, all applicable validation passes, the worktree is clean, and a fresh reviewer returns `APPROVED`.

Final acceptance:
- Run all applicable automated, manual, security, package, x64, ARM64, and performance checks.
- Ask a fresh `final_reviewer` to review the complete branch against its original base.
- Complete only when it returns `APPROVED`, all required evidence matches the candidate ref, all phases are complete, and no old owner, duplicate pipeline, or temporary adapter remains.

Stop rules:
- Stop and ask for a decision when a Gxx gate is required.
- Report a blocker instead of weakening compatibility, security, privacy, adaptive resources, Bluetooth correctness, atomic settings, or validation.
- Never report an unrun check as passed.

Progress updates: report only phase outcome, validation evidence, reviewer status, blockers, and next phase.
