# AI Hermes Routing Init Skill

Portable Codex skill for bootstrapping an AI Hermes routing layer in a target repository.

## What It Does

- Creates missing root routing files: `AGENTS.md`, `docs/repo_map.json`, `docs/task_routes.json`, `docs/pitfalls.json`, and `docs/ai_routing_evolution_policy.json`.
- Installs project-local AI Hermes routing skill scripts under `skills/ai-hermes-self-evolve/` and `skills/ai-hermes-routing-init/`.
- Creates `AGENTS.md` when it is missing, or appends a generic AI Hermes routing protocol when the existing file lacks routing-layer instructions.
- Uses an idempotent `AI-HERMES-ROUTING-PROTOCOL` marker block, avoids duplicating a complete legacy routing protocol, and repairs a well-formed but incomplete marker block.
- Runs self-audit checks so the generated routing layer is immediately validated.

## Contents

- `SKILL.md` - workflow and operating rules.
- `agents/openai.yaml` - Codex skill metadata.
- `scripts/init_ai_routing.py` - initializer for missing routing files and project-local skill scripts.
- `scripts/evolve_ai_routing.py` - bundled routing coverage checker.
- `scripts/validate_ai_routing.py` - bundled routing schema/path/reference validator.
- `scripts/route_task.py` - bundled route resolver smoke checker.

## Usage

Preview writes first when the target repo already has any routing file:

```bash
python3 skills/ai-hermes-routing-init/scripts/init_ai_routing.py --project-root "$PWD" --dry-run --json
```

Create missing routing files and project-local skill scripts:

```bash
python3 skills/ai-hermes-routing-init/scripts/init_ai_routing.py --project-root "$PWD" --json
```

Run final checks from the target repo:

```bash
python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only --changed-file AGENTS.md --changed-file docs/repo_map.json --changed-file docs/task_routes.json --changed-file docs/pitfalls.json --changed-file docs/ai_routing_evolution_policy.json --json
python3 skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py
python3 skills/ai-hermes-self-evolve/scripts/route_task.py --route-id R02 --mode context
```

When using the global skill outside the target repo, call it by absolute path and pass the target root:

```bash
SKILL_DIR=<directory-containing-this-README>
python3 "$SKILL_DIR/scripts/init_ai_routing.py" --project-root <target-repo> --dry-run --json
```

## Bootstrap Policy

- Generate only conservative seed facts.
- Mark implementation modules as `needs_code_confirmation` unless code was read and verified.
- Keep routing facts in JSON owners, not in `AGENTS.md` or this README.
- Keep the inserted `AGENTS.md` protocol generic: read order, routing ownership, validation, self-evolution, and output discipline only.
- Do not append a second protocol block when marker boundaries are malformed; report the marker issue for manual repair.
- Do not create AI Hermes tests under `tests/`; use `py_compile`, smoke checks, and routing validation.

## Portability Notes

- The bundled scripts use only Python standard library modules.
- The initializer does not require host-specific absolute paths.
- Target repositories with custom routing schemas may need local adaptation before full validation passes.
