---
name: ai-hermes-routing-init
description: Create initial AI Hermes routing files and self-audit them. Use when a repository lacks AGENTS.md, docs/repo_map.json, docs/task_routes.json, docs/pitfalls.json, docs/ai_routing_evolution_policy.json, skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py, or skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py; when bootstrapping AI Hermes routing memory for a new repo; when the user asks to create routing files, initialize routing memory, scaffold repo_map/task_routes/pitfalls, or close the routing initialization/self-evolution loop.
---

# AI Hermes Routing Init

## Scope

Use this skill to bootstrap AI Hermes routing memory in a target repository. Keep edits inside the target repository root. Do not promote guessed implementation behavior as fact.

This skill creates the initial routing layer; `ai-hermes-self-evolve` maintains it after code, tests, tools, or routing facts change.

## AGENTS Protocol

`AGENTS.md` is part of the routing layer. If it is missing, create it. If it exists but lacks AI Hermes routing-layer instructions, append the generic routing protocol before validation.

The inserted section must stay generic and protocol-only. It must include the route read order, routing ownership rules, validation workflow, and self-evolution workflow. It must include the validator-required concepts: `governance only`, `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py`, `verified hidden contracts`, ``Update `AGENTS.md` only when protocol``, `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only`, and `skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py`.

Use the idempotent marker block `AI-HERMES-ROUTING-PROTOCOL` when adding the protocol. If a repository already has a complete legacy routing protocol without the marker, do not duplicate it. If a well-formed marker block exists but is missing required routing protocol content, repair that block in place. If marker boundaries are malformed, report the problem for manual repair instead of appending a second protocol block.

## Workflow

1. Confirm the target root is the current repository unless the user provides another root.
2. Inspect existing routing files before writing:
   - `AGENTS.md`
   - `docs/repo_map.json`
   - `docs/task_routes.json`
   - `docs/pitfalls.json`
   - `docs/ai_routing_evolution_policy.json`
   - `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py`
   - `skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py`
   - `skills/ai-hermes-self-evolve/scripts/route_task.py`
3. Run the bundled initializer in preview mode first when the repo already has any routing file:

```bash
python3 skills/ai-hermes-routing-init/scripts/init_ai_routing.py --project-root "$PWD" --dry-run --json
```

4. Create missing routing files and project-local skill scripts:

```bash
python3 skills/ai-hermes-routing-init/scripts/init_ai_routing.py --project-root "$PWD" --json
```

5. Read the generated report. If `validation.exit_code` or `coverage.exit_code` is non-zero, inspect the listed problems before editing further.
6. Run the final checks with project-local skill scripts:

```bash
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only --changed-file AGENTS.md --changed-file docs/repo_map.json --changed-file docs/task_routes.json --changed-file docs/pitfalls.json --changed-file docs/ai_routing_evolution_policy.json --changed-file skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --changed-file skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py --changed-file skills/ai-hermes-self-evolve/scripts/route_task.py --json
python3 skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py
python3 skills/ai-hermes-self-evolve/scripts/route_task.py --route-id R02 --mode context
```

## Generated Facts Policy

The initializer may create only conservative seed facts:

- Existing top-level anchors such as `README.md`, `pyproject.toml`, `package.json`, `src/`, `service/`, `app/`, or `tests/`.
- AI Hermes routing and tooling files that it actually creates or observes.
- `grounding.fact_status: "needs_code_confirmation"` for project implementation modules unless code was separately read and verified.
- `grounding.fact_status: "grounded"` only for generated routing/tool files that pass validation or smoke checks.

Do not mark business/runtime behavior as grounded during bootstrap unless actual code and a test or smoke command were read and run.

## Bundled Resources

- `scripts/init_ai_routing.py`: create missing root routing files, install project-local AI Hermes skill scripts, and run self-audit.
- `scripts/evolve_ai_routing.py`: bundled routing coverage checker.
- `scripts/validate_ai_routing.py`: bundled routing validator.
- `scripts/route_task.py`: bundled route resolver.

When operating outside this repository, call the bundled scripts by absolute skill path and always pass `--project-root <target-repo>`.

## Safety Rules

- Default to `create_missing`; do not overwrite existing routing files unless the user explicitly asks and `--overwrite` is used.
- Use `git add -N` intent-to-add for generated stable routing files when available, so validation can check reproducibility without staging contents.
- Keep generated project facts minimal; future agents should refine them through `ai-hermes-self-evolve` after reading code.
- Do not create tests under `tests/` for AI Hermes tooling; use script smoke checks and `py_compile`.
