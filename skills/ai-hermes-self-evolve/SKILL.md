---
name: ai-hermes-self-evolve
description: Use when the user explicitly asks to maintain AI Hermes routing memory or self-evolution; when handling repo_map, task_routes, pitfalls, fact_status, evolve_ai_routing, validate_ai_routing, routing coverage, stale facts, hidden contracts, recurring pitfalls, or AI Hermes protocol updates; and before commit, PR, merge, or release when project changes may require AI Hermes routing-memory checks.
---

# AI Hermes Self-Evolve

## Scope

Use this skill only after the AI Hermes routing data layer exists in the target repository:

- `AGENTS.md`
- `docs/repo_map.json`
- `docs/task_routes.json`
- `docs/pitfalls.json`
- `docs/ai_routing_evolution_policy.json` for full validation

If any routing data-layer file is missing, stop the self-evolution workflow and tell the user to run `$ai-hermes-routing-init` first. Do not silently initialize routing files from this skill.

Keep edits inside the current project subtree. External paths may be read only for comparison or integration analysis. Do not write routing facts outside this project.

## AGENTS Protocol

Before running validation, check whether `AGENTS.md` contains AI Hermes routing-layer instructions. A complete legacy protocol or the `AI-HERMES-ROUTING-PROTOCOL` marker block is acceptable.

If `AGENTS.md` is missing or lacks routing-layer instructions, treat that as an initialization gap and tell the user to run `$ai-hermes-routing-init` first. Only update `AGENTS.md` from this skill when the user explicitly asks for protocol maintenance in an already initialized routing layer.

Any routing protocol section must stay generic and protocol-only. It must include the validator-required concepts: `governance only`, `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py`, `verified hidden contracts`, ``Update `AGENTS.md` only when protocol``, `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only`, and `skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py`.

If a marker block exists, validation must confirm exactly one begin marker, exactly one end marker, begin before end, and required routing protocol snippets inside the marked block.

Do not add ordinary module facts, personal memory, local paths, or implementation claims to `AGENTS.md`; keep those in their JSON owners or private memory files.

## Required Tools

Use the AI Hermes routing CLIs in this order:

- Prefer the current repo's project-local skill scripts: `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py`, `skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py`, and `skills/ai-hermes-self-evolve/scripts/route_task.py` when they exist.
- Otherwise use this skill's bundled `scripts/evolve_ai_routing.py`, `scripts/validate_ai_routing.py`, and `scripts/route_task.py`.
- When using bundled scripts, set `SKILL_DIR` to the directory containing this `SKILL.md`, and pass `--project-root <target-repo>` so the scripts validate the repository, not the skill directory.

Example bundled fallback:

```bash
SKILL_DIR=<directory-containing-this-SKILL.md>
python3 "$SKILL_DIR/scripts/evolve_ai_routing.py" --project-root "$PWD" --changed-file <path> --json
python3 "$SKILL_DIR/scripts/validate_ai_routing.py" --project-root "$PWD"
python3 "$SKILL_DIR/scripts/route_task.py" --project-root "$PWD" --route-id <route-id> --mode context
```

## Trigger Handling

Run this workflow when the user explicitly asks for AI Hermes/routing-memory maintenance, or when a task discovers one of these signals:

- New, deleted, moved, or renamed important files.
- New tests, tools, config entries, project skills, deployment files, or regression commands.
- Hidden contracts, recurring pitfalls, cross-module dependencies, stale facts, or fact-status changes.
- `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py` reports `uncovered_files`, `routing_only_violations`, or other coverage risks.
- The user asks to commit, create a PR, merge, release, or run a pre-submit/pre-PR check after project changes.

Do not run the full self-evolution workflow before every ordinary code edit. For normal development, run the lightweight check near the end of the task or when one of the signals above appears.

## Workflow

1. **Load protocol**: Read `AGENTS.md`, `docs/repo_map.json`, `docs/task_routes.json`, and `docs/pitfalls.json`.
2. **Record baseline**: Capture `git status -sb` before deciding scope. Treat pre-existing dirty files as user-owned unless they are explicitly part of the task.
3. **Select target files**: Prefer user-specified paths or files changed by the current task. Do not default to the whole dirty worktree.
4. **Resolve submit scope**:
   - Commit: prefer `git diff --cached --name-only`.
   - PR: prefer `--diff-range <base>...HEAD` when the user provides a base.
   - Merge/release: prefer `--diff-range <base>..<head>` for the user-specified range.
   - If scope is unclear, report uncertainty and use current relevant changed files as candidates without editing unrelated pre-existing dirty files.
5. **Run coverage check**: Use explicit paths:

```bash
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --changed-file <path> --json
```

Repeat `--changed-file` for multiple current-tree paths. For PR, merge, or release range audits, use range-aware mode so deletions and renames retain their Git status:

```bash
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --diff-range <base>...HEAD --json
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --base-ref <base> --head-ref <head> --json
```

Do not pipe `git diff --name-only` output into repeated `--changed-file` calls for a range audit, because that loses deleted/renamed status and can create false `uncovered_files`. If project-local skill scripts are absent, run the bundled fallback from Required Tools with the same arguments and `--project-root "$PWD"`. Use `python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --json` only when the user explicitly requests a whole dirty-worktree audit.

6. **Audit semantic drift**: `covered_files` means only that a path is referenced. It does not prove the stored routing facts are still accurate.
7. **Collect evidence**: Read the relevant code, tests, configs, tools, skill files, and command output before updating routing memory.
8. **Update minimally**: Modify only the correct AI Hermes owner and only the fields needed.
9. **Validate**: Run routing validation and the smallest relevant regression or smoke command.
10. **Report**: State what changed, evidence used, commands run, remaining unknowns, and which pre-existing dirty files were not included.

## Evidence Rules

Allowed evidence sources:

- Current project source code.
- Current project tests.
- Current project configs.
- Current project tool scripts.
- Actual command output.

Stable routing references to tools, tests, skills, configs, or source files must be reproducible from git. Before promoting such paths into routing memory, confirm they are git tracked. Only local diagnostics or task artifacts may remain untracked, and only when they match explicit artifact or ignored globs in `docs/ai_routing_evolution_policy.json`.

Fact-status guidance:

- `grounded`: code/config read and verified by a test or smoke command.
- `mixed`: partial code/config read; unsampled paths remain.
- `needs_code_confirmation`: symptom observed, but code confirmation is incomplete.
- `out_of_scope`: external service, external repository, invisible runtime behavior, or unseen caller/callee.
- `unknown`: cannot confirm.

Do not write these into long-term routing memory:

- Secrets, tokens, cookies, passwords, Authorization headers, account credentials.
- `.env` contents, private runtime artifacts, personal task logs, or raw sensitive command output.
- Host-specific absolute paths unless they are unavoidable project-boundary facts.
- Unverified external behavior or README/design/chat claims not confirmed by code or command output.

Use relative paths, behavior summaries, `unknown`, `out_of_scope`, or `grounding.unsampled_paths` instead.

## Routing Ownership Rules

Write facts to the correct owner:

- `docs/repo_map.json`: module facts, file lists, dependencies, tests, configs, regression commands, and grounding evidence.
- `docs/task_routes.json`: task matching, route priority, `first_read_modules`, module expansion, and route verification codes.
- `docs/pitfalls.json`: hidden contracts, recurring pitfalls, high-risk triggers, affected modules, checks, and safe observation methods.
- `AGENTS.md`: protocol, required read order, scope rules, submit/PR workflow rules, and tool workflow only.

Docset selection:

- `service/**` implementation facts usually belong in `service/docs/*`.
- Root tools, root tests, project skills, AI Hermes tooling, and global protocol belong in root `docs/*` or `AGENTS.md`.
- Avoid duplicating the same fact in root and service docsets. Root may keep cross-layer routing entry points while service keeps service details.

## Semantic Drift Checks

Check both uncovered and covered changed files for:

- New or removed entry files, public APIs, CLI arguments, config keys, environment variables, or payload fields.
- New or obsolete tests, configs, tools, or minimum regression commands.
- Changed upstream/downstream dependencies or out-of-scope dependencies.
- New risk flags, hidden contracts, recurring pitfalls, or stale pitfall links.
- `grounding.fact_status`, `grounding.evidence`, and `grounding.unsampled_paths` accuracy.
- Route keyword or module-read-order changes needed for future agents.

Possible conclusions:

- `no_routing_change_needed`
- `update_repo_map`
- `update_task_routes`
- `update_pitfalls`
- `update_agents_protocol`
- `needs_more_evidence`

## Update Rules

Before editing AI Hermes files, confirm:

- The current task actually needs routing-memory maintenance.
- The fact is backed by code, tests, config, tool output, or command output.
- Stable referenced paths are git tracked, unless they are explicit local artifacts covered by policy ignored globs.
- The fact belongs in the selected owner and docset.
- The edit is the smallest viable change.
- External or uncertain behavior is not promoted as subtree truth.

For stale facts caused by delete, rename, move, module split, module merge, or test/config removal:

- Remove or update obsolete paths in `entry_files`, `first_read_files`, `then_check_files`, `related_tests`, `related_configs`, `minimum_regression`, and `grounding.evidence`.
- Update module references such as `child_modules`, dependencies, affected modules, and `pitfall_ids`.
- Use code, git diff, file existence, or validation output as evidence before deleting routing facts.

## Validation

Always run after AI Hermes file changes:

```bash
python3 skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py
python3 skills/ai-hermes-self-evolve/scripts/route_task.py --route-id <changed-or-relevant-route-id> --mode context
```

If project-local skill scripts are absent:

```bash
SKILL_DIR=<directory-containing-this-SKILL.md>
python3 "$SKILL_DIR/scripts/validate_ai_routing.py" --project-root "$PWD"
python3 "$SKILL_DIR/scripts/route_task.py" --project-root "$PWD" --route-id <changed-or-relevant-route-id> --mode context
```

If validation reports an untracked stable reference, do not silence it by artifact-allowlisting the file. Either make the referenced file part of the submitted git scope, remove the stale routing reference, or mark it as artifact-only only when it is truly a local diagnostic artifact.

For routing/tool governance files only:

```bash
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only --changed-file <routing-file> --json
```

If project-local skill scripts are absent, use the bundled `scripts/evolve_ai_routing.py` with `--project-root "$PWD"`.

For non-routing project files:

```bash
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --changed-file <project-file> --json
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --diff-range <base>..<head> --json
```

If project-local skill scripts are absent, use the bundled `scripts/evolve_ai_routing.py` with `--project-root "$PWD"`.

For project skills, run at least a read smoke check for `SKILL.md` and `agents/openai.yaml` when present:

```bash
python3 -c "from pathlib import Path; [Path(p).read_text(encoding='utf-8') for p in ['skills/<skill-name>/SKILL.md', 'skills/<skill-name>/agents/openai.yaml'] if Path(p).exists()]"
python3 -m py_compile skills/<skill-name>/scripts/*.py
```

If an affected module declares `minimum_regression`, run the smallest relevant command unless the user explicitly asks not to.

## Output Rules

Report in the user's language by default. Be direct and brief. Use bullets, not tables.

Include:

- AI Hermes files changed.
- Evidence source for each promoted fact.
- Validation commands and results.
- Remaining `unknown`, `needs_code_confirmation`, or `out_of_scope` facts.
- Pre-existing dirty files not included in the current self-evolution scope.

## Prohibited Actions

- Do not let `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py` write routing files.
- Do not treat `docs/ai_routing_evolution_policy.json` or this skill as a routing-fact source.
- Do not write ordinary module facts to `AGENTS.md`.
- Do not update routing memory from guesses, one-off symptoms, or unverified external behavior.
- Do not refactor business code while doing routing-only maintenance.
- Do not mix ordinary business files into `--routing-only` checks.
