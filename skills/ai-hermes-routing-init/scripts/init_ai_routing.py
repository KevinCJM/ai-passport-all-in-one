#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

ROUTING_FILES = [
    "AGENTS.md",
    "docs/repo_map.json",
    "docs/task_routes.json",
    "docs/pitfalls.json",
    "docs/ai_routing_evolution_policy.json",
    "skills/ai-hermes-self-evolve/README.md",
    "skills/ai-hermes-self-evolve/SKILL.md",
    "skills/ai-hermes-self-evolve/agents/openai.yaml",
    "skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py",
    "skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py",
    "skills/ai-hermes-self-evolve/scripts/route_task.py",
    "skills/ai-hermes-routing-init/README.md",
    "skills/ai-hermes-routing-init/SKILL.md",
    "skills/ai-hermes-routing-init/agents/openai.yaml",
    "skills/ai-hermes-routing-init/scripts/init_ai_routing.py",
    "skills/ai-hermes-routing-init/scripts/evolve_ai_routing.py",
    "skills/ai-hermes-routing-init/scripts/validate_ai_routing.py",
    "skills/ai-hermes-routing-init/scripts/route_task.py",
]

PROJECT_SKILL_FILES = [path for path in ROUTING_FILES if path.startswith("skills/")]

ANCHOR_CANDIDATES = [
    "README.md",
    "pyproject.toml",
    "package.json",
    "requirements.txt",
    "src",
    "service",
    "app",
    "tests",
]

EVOLVE_JSON_FIELDS = [
    "changed_files",
    "ignored_files",
    "covered_files",
    "uncovered_files",
    "routing_only_violations",
    "suggested_updates",
    "missing_required_files",
    "next_action",
    "exit_code_reason",
]

AGENTS_PROTOCOL_BEGIN = "<!-- AI-HERMES-ROUTING-PROTOCOL:BEGIN -->"
AGENTS_PROTOCOL_END = "<!-- AI-HERMES-ROUTING-PROTOCOL:END -->"
AGENTS_ROUTING_CONCEPTS = {
    "# Required Read Order",
    "# AI Routing Validation",
    "# AI Routing Self-Evolution",
    "docs/task_routes.json",
    "docs/repo_map.json",
    "docs/pitfalls.json",
    "Keep routing facts in JSON only",
    "skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py",
    "skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py",
}

AGENTS_PROTOCOL_SECTION = f"""{AGENTS_PROTOCOL_BEGIN}
# AI Hermes Routing Protocol

## Purpose

Machine-first routing protocol for downstream agents operating from the current working directory.

## Scope Boundary

- Treat `.` as the writable project boundary unless higher-priority instructions say otherwise.
- External folders may be read for task understanding, comparison, or integration analysis; do not route edits outside the target project.
- Treat external services, DB schema, and invisible callers/callees as `out_of_scope` unless directly observed from readable files.
- Keep routing facts in JSON files under `docs/`; keep `AGENTS.md` protocol-only.

## Required Read Order

1. `AGENTS.md`
2. `docs/repo_map.json`
3. `docs/task_routes.json`
4. `docs/pitfalls.json`
5. Routed code, tests, and configs

## Routing Ownership

- `docs/task_routes.json` owns task matching, module expansion, and operational-list merge policy.
- `docs/repo_map.json` owns module facts, operational file lists, tests, configs, and regression commands.
- `docs/pitfalls.json` owns hidden contracts, recurring pitfalls, affected modules, and safe checks.
- `AGENTS.md` owns protocol, required read order, scope rules, and tool workflow only.
- Do not duplicate module-level file, test, config, or regression lists in `docs/task_routes.json`.

## Default Operating Sequence

1. Match the task in `docs/task_routes.json`.
2. Load `first_read_modules` from the selected route.
3. Expand into `expand_to_modules` only when route rule codes trigger.
4. Resolve `first_read_files`, `then_check_files`, `related_tests`, `related_configs`, and `minimum_regression` from `docs/repo_map.json` using `docs/task_routes.json` merge policy.
5. Load linked pitfalls from `docs/pitfalls.json`.
6. Verify claims from code, tests, configs, or command output before promoting them to routing memory.

## AI Routing Validation

- Use `skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py` after editing `AGENTS.md`, `docs/repo_map.json`, `docs/task_routes.json`, `docs/pitfalls.json`, or matching service routing files.
- The validator checks route/module/pitfall references, routed path existence, git-tracked reproducibility for stable references, minimum regression command targets, and `grounding.fact_status` values.

# AI Routing Self-Evolution

- Treat `docs/ai_routing_evolution_policy.json` as governance only; routing facts belong in `docs/task_routes.json`, `docs/repo_map.json`, and `docs/pitfalls.json`.
- Update `AGENTS.md` only when protocol, required read order, scope rules, or tool workflow changes.
- Promote verified hidden contracts and recurring pitfalls to the correct JSON owner.
- Use `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py` after code, test, config, tool, or routing changes to check coverage.
- For routing-only work, run `skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only` with explicit changed paths.
- Re-run `skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py` after routing file changes.

## Output Discipline

- Keep routing facts in JSON only.
- Keep `AGENTS.md` protocol-only.
- Stop exploration once routing is sufficient for first-pass narrowing.
{AGENTS_PROTOCOL_END}
"""

AGENTS_TEMPLATE = AGENTS_PROTOCOL_SECTION


@dataclass
class WriteResult:
    path: str
    action: str


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _json_dump(data: Any) -> str:
    return json.dumps(data, ensure_ascii=False, indent=2) + "\n"


def _rel(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def _existing_anchors(root: Path) -> list[str]:
    anchors = [item for item in ANCHOR_CANDIDATES if (root / item).exists()]
    if not anchors:
        anchors = ["AGENTS.md"]
    return anchors


def _has_git(root: Path) -> bool:
    return (root / ".git").exists() or subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--is-inside-work-tree"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode == 0


def _run(cmd: Sequence[str], root: Path) -> dict[str, Any]:
    completed = subprocess.run(
        list(cmd),
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    return {
        "command": " ".join(cmd),
        "exit_code": completed.returncode,
        "stdout": completed.stdout.strip(),
        "stderr": completed.stderr.strip(),
    }


def _write_text(path: Path, text: str, *, overwrite: bool, dry_run: bool) -> WriteResult:
    if path.exists() and not overwrite:
        return WriteResult(str(path), "kept_existing")
    if dry_run:
        return WriteResult(str(path), "would_overwrite" if path.exists() else "would_create")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return WriteResult(str(path), "overwritten" if path.exists() and overwrite else "created")


def _write_json(path: Path, data: Any, *, overwrite: bool, dry_run: bool) -> WriteResult:
    return _write_text(path, _json_dump(data), overwrite=overwrite, dry_run=dry_run)


def _is_complete_routing_protocol(text: str) -> bool:
    return all(concept in text for concept in AGENTS_ROUTING_CONCEPTS)


def _marker_bounds(text: str) -> tuple[int, int] | None:
    begin_count = text.count(AGENTS_PROTOCOL_BEGIN)
    end_count = text.count(AGENTS_PROTOCOL_END)
    if begin_count != 1 or end_count != 1:
        return None
    begin_index = text.find(AGENTS_PROTOCOL_BEGIN)
    end_index = text.find(AGENTS_PROTOCOL_END)
    if begin_index < 0 or end_index < begin_index:
        return None
    return begin_index, end_index + len(AGENTS_PROTOCOL_END)


def _ensure_agents(root: Path, *, overwrite: bool, dry_run: bool) -> WriteResult:
    path = root / "AGENTS.md"
    if overwrite or not path.exists():
        return _write_text(path, AGENTS_TEMPLATE, overwrite=overwrite, dry_run=dry_run)
    text = path.read_text(encoding="utf-8")
    marker_bounds = _marker_bounds(text)
    if marker_bounds:
        start, end = marker_bounds
        marker_block = text[start:end]
        if _is_complete_routing_protocol(marker_block):
            return WriteResult(str(path), "kept_existing")
        if dry_run:
            return WriteResult(str(path), "would_repair_protocol")
        repaired = text[:start].rstrip() + "\n\n" + AGENTS_PROTOCOL_SECTION + "\n" + text[end:].lstrip()
        path.write_text(repaired, encoding="utf-8")
        return WriteResult(str(path), "repaired_protocol")
    has_any_marker = AGENTS_PROTOCOL_BEGIN in text or AGENTS_PROTOCOL_END in text
    if has_any_marker:
        return WriteResult(str(path), "needs_manual_marker_repair")
    if _is_complete_routing_protocol(text):
        return WriteResult(str(path), "kept_existing")
    if dry_run:
        return WriteResult(str(path), "would_append")
    with path.open("a", encoding="utf-8") as handle:
        if not text.endswith("\n"):
            handle.write("\n")
        handle.write("\n" + AGENTS_PROTOCOL_SECTION)
    return WriteResult(str(path), "appended_protocol")


def _repo_map(root: Path) -> dict[str, Any]:
    anchors = _existing_anchors(root)
    entry_anchor = anchors[0]
    tests = [item for item in anchors if item == "tests"]
    return {
        "schema_version": "1.2",
        "scope": {
            "root": ".",
            "mode": "subtree_only",
            "parent_inspection_allowed": False,
            "notes": ["initial AI Hermes routing scaffold", "implementation facts require code confirmation"],
        },
        "modules": [
            {
                "id": "project_entrypoints",
                "name": "project_entrypoints",
                "path": entry_anchor,
                "kind": "project_overview",
                "purpose": "Initial project anchors discovered during AI Hermes routing bootstrap; implementation behavior still needs code confirmation.",
                "module_tags": ["project", "bootstrap", "needs_code_confirmation"],
                "entry_files": anchors,
                "key_symbols": [],
                "child_modules": [],
                "first_read_files": anchors,
                "then_check_files": tests,
                "upstream_dependencies": [],
                "downstream_dependencies": [],
                "out_of_scope_dependencies": [],
                "related_tests": tests,
                "related_configs": [item for item in anchors if item.endswith((".md", ".toml", ".json", ".txt"))],
                "risk_flags": ["bootstrap_facts_require_confirmation"],
                "edit_risk": "unknown",
                "blast_radius": ["unknown until routed code is sampled"],
                "pitfall_ids": ["P01"],
                "minimum_regression": [],
                "routing_confidence": "low",
                "scope_flags": ["subtree_only", "initial_bootstrap"],
                "grounding": {
                    "fact_status": "needs_code_confirmation",
                    "evidence": anchors,
                    "unsampled_paths": anchors,
                },
            },
            {
                "id": "ai_routing_files",
                "name": "ai_routing_files",
                "path": "docs/repo_map.json",
                "kind": "routing_docset",
                "purpose": "AI Hermes routing files and project-local skill scripts created by the routing initializer.",
                "module_tags": ["ai_routing", "bootstrap", "skills", "scripts"],
                "entry_files": ROUTING_FILES,
                "key_symbols": ["validate_docset", "build_report", "main"],
                "child_modules": [],
                "first_read_files": ["AGENTS.md", "docs/repo_map.json", "docs/task_routes.json", "docs/pitfalls.json"],
                "then_check_files": [
                    "docs/ai_routing_evolution_policy.json",
                    "skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py",
                    "skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py",
                    "skills/ai-hermes-self-evolve/scripts/route_task.py",
                    "skills/ai-hermes-routing-init/scripts/init_ai_routing.py",
                ],
                "upstream_dependencies": [],
                "downstream_dependencies": [],
                "out_of_scope_dependencies": ["unverified external runtime behavior"],
                "related_tests": [],
                "related_configs": ["AGENTS.md", "docs/ai_routing_evolution_policy.json"],
                "risk_flags": ["routing_memory_drift", "bootstrap_facts_require_confirmation", "routing_only_scope_leak"],
                "edit_risk": "medium",
                "blast_radius": ["AI Hermes routing", "future agent task narrowing"],
                "pitfall_ids": ["P01", "P02"],
                "minimum_regression": [
                    "python3 -m py_compile skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py skills/ai-hermes-self-evolve/scripts/route_task.py skills/ai-hermes-routing-init/scripts/init_ai_routing.py",
                    "python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only --changed-file docs/repo_map.json --changed-file skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --json",
                    "python3 skills/ai-hermes-self-evolve/scripts/route_task.py --route-id R02 --mode context",
                    "python3 skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py",
                ],
                "routing_confidence": "high",
                "scope_flags": ["subtree_only", "routing_memory_coverage_check"],
                "grounding": {
                    "fact_status": "grounded",
                    "evidence": ROUTING_FILES,
                    "unsampled_paths": [],
                },
            },
        ],
    }


def _task_routes() -> dict[str, Any]:
    return {
        "schema_version": "1.2",
        "scope_mode": "subtree_only",
        "routing_policy": {
            "operational_list_owner": "docs/repo_map.json",
            "route_to_module_field": "first_read_modules",
            "expand_module_field": "expand_to_modules",
            "operational_list_resolution": {
                "apply_to_fields": ["first_read_files", "then_check_files", "related_tests", "related_configs", "minimum_regression"],
                "selected_modules_order": "first_read_modules_then_expand_to_modules_when_triggered",
                "merge_strategy": "stable_order_union",
                "dedupe": "exact_string",
                "route_level_override": "forbidden",
            },
            "grounding_gate": {
                "blocking_fact_status": ["mixed", "needs_code_confirmation"],
                "required_action": "read_grounding_unsampled_paths_before_claim",
            },
        },
        "rule_catalog": {
            "expand_search": {"read_reference_map_first": {"action": "read_reference_map"}},
            "stop_and_verify": {"run_minimum_regression": {"action": "run_minimum_regression"}},
            "fact_check": {
                "read_reference_map_first": {"action": "read_reference_map"},
                "read_unsampled_module_paths": {"action": "read_grounding_unsampled_paths"},
            },
        },
        "routes": [
            {
                "id": "R01",
                "task_type": "general_project_work",
                "match_keywords": ["project", "code", "test", "config", "README", "implementation"],
                "negative_keywords": ["repo_map", "task_routes", "pitfalls", "AI routing"],
                "route_priority": 10,
                "first_read_modules": ["project_entrypoints"],
                "expand_to_modules": [],
                "pitfall_ids": ["P01"],
                "expand_search_codes": ["read_reference_map_first"],
                "stop_and_verify_codes": ["run_minimum_regression"],
                "fact_check_codes": ["read_unsampled_module_paths"],
                "scope_flags": ["subtree_only", "initial_bootstrap"],
            },
            {
                "id": "R02",
                "task_type": "ai_routing_initialization_or_maintenance",
                "match_keywords": ["AGENTS.md", "repo_map", "task_routes", "pitfalls", "AI routing", "Hermes", "evolve_ai_routing", "validate_ai_routing", "routing memory", "routing files"],
                "negative_keywords": [],
                "route_priority": 100,
                "first_read_modules": ["ai_routing_files"],
                "expand_to_modules": [],
                "pitfall_ids": ["P01", "P02"],
                "expand_search_codes": ["read_reference_map_first"],
                "stop_and_verify_codes": ["run_minimum_regression"],
                "fact_check_codes": ["read_reference_map_first"],
                "scope_flags": ["subtree_only", "routing_memory_coverage_check"],
            },
        ],
    }


def _pitfalls() -> dict[str, Any]:
    return {
        "schema_version": "1.1",
        "scope_mode": "subtree_only",
        "pitfalls": [
            {
                "id": "P01",
                "title": "initial_routing_facts_require_code_confirmation",
                "severity": "medium",
                "confidence": "high",
                "risk_codes": ["bootstrap_fact_uncertainty", "over_learning_from_scaffold"],
                "symptoms": ["routing facts are broader than verified code behavior", "future agents treat bootstrap anchors as grounded implementation facts"],
                "trigger_actions": ["create initial routing files", "edit routing facts before sampling code"],
                "related_paths": ["docs/repo_map.json", "docs/task_routes.json", "docs/pitfalls.json"],
                "affected_modules": ["project_entrypoints", "ai_routing_files"],
                "blast_radius": ["future agent task narrowing"],
                "check_before_edit": ["read sampled code before promoting facts", "keep fact_status as needs_code_confirmation until verified"],
                "safe_observation_methods": ["read grounding.unsampled_paths", "run smallest visible smoke command"],
            },
            {
                "id": "P02",
                "title": "routing_files_are_machine_owned_contracts",
                "severity": "medium",
                "confidence": "high",
                "risk_codes": ["routing_memory_drift", "routing_only_scope_leak"],
                "symptoms": ["task_routes duplicates repo_map operational lists", "routing-only changes include business code"],
                "trigger_actions": ["edit AGENTS.md or docs routing JSON", "prepare routing-only commit"],
                "related_paths": ROUTING_FILES,
                "affected_modules": ["ai_routing_files"],
                "blast_radius": ["AI Hermes routing", "future agent task narrowing"],
                "check_before_edit": ["keep routing facts in JSON owners", "run evolve and validate after routing changes"],
                "safe_observation_methods": ["python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only --changed-file <path> --json", "python3 skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py"],
            },
        ],
    }


def _policy() -> dict[str, Any]:
    ignored = [
        ".idea/**",
        ".pytest_cache/**",
        ".ruff_cache/**",
        "__pycache__/**",
        "**/__pycache__/**",
        "*.pyc",
        ".DS_Store",
    ]
    routing_globs = [
        "AGENTS.md",
        "docs/ai_routing_evolution_policy.json",
        "docs/repo_map.json",
        "docs/task_routes.json",
        "docs/pitfalls.json",
        "skills/ai-hermes-self-evolve/**",
        "skills/ai-hermes-routing-init/**",
    ]
    return {
        "schema_version": "1.0",
        "scope_mode": "subtree_writable_external_read_allowed",
        "role": "governance_only",
        "purpose": "Govern the AI Hermes routing self-evolution lifecycle without becoming a source of routing facts.",
        "authoritative_owners": {
            "task_matching_module_expansion_and_operational_list_merge": "docs/task_routes.json",
            "module_facts_and_operational_lists": "docs/repo_map.json",
            "pitfall_facts": "docs/pitfalls.json",
            "agent_protocol": "AGENTS.md",
        },
        "non_authoritative_for": ["task_matching", "module_operational_lists", "module_facts", "pitfall_facts", "runtime_behavior_claims"],
        "invariants": [
            "task_routes_remains_single_machine_owner",
            "policy_must_not_duplicate_module_operational_lists",
            "policy_must_not_duplicate_route_match_rules",
            "policy_must_not_duplicate_pitfall_facts",
            "agents_md_stays_protocol_only",
            "business_code_is_not_modified_by_routing_only_tasks",
            "stable_routing_references_are_git_tracked_or_explicit_artifacts",
        ],
        "commit_scope_guard": {"routing_only_allowed_globs": routing_globs},
        "cli_contracts": {
            "evolve_ai_routing": {
                "default_mode": "check_report_only",
                "must_not_modify_files": True,
                "stable_json_fields": EVOLVE_JSON_FIELDS,
                "smoke_commands": [
                    "python3 -m py_compile skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py skills/ai-hermes-self-evolve/scripts/route_task.py",
                    "python3 skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only --changed-file docs/repo_map.json --changed-file skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --json",
                    "python3 skills/ai-hermes-self-evolve/scripts/route_task.py --route-id R02 --mode context",
                    "python3 skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py",
                ],
            }
        },
        "change_detection": {"ignored_globs": ignored},
        "reproducibility": {"tracked_reference_check": {"enabled": True, "allowed_untracked_globs": ignored}},
    }


def _install_project_skills(
    root: Path,
    source_skills_dir: Path,
    *,
    overwrite: bool,
    dry_run: bool,
) -> list[WriteResult]:
    results: list[WriteResult] = []
    for rel in PROJECT_SKILL_FILES:
        source = source_skills_dir / rel.removeprefix("skills/")
        target = root / rel
        if not source.exists():
            results.append(WriteResult(str(target), "source_missing"))
            continue
        if target.exists() and not overwrite:
            results.append(WriteResult(str(target), "kept_existing"))
            continue
        if dry_run:
            results.append(WriteResult(str(target), "would_overwrite" if target.exists() else "would_create"))
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        target.chmod(0o755)
        results.append(WriteResult(str(target), "overwritten" if target.exists() and overwrite else "created"))
    return results


def _intent_to_add(root: Path, paths: Sequence[str]) -> dict[str, Any]:
    if not _has_git(root):
        return {"enabled": False, "reason": "not_a_git_repo", "paths": []}
    existing = [path for path in paths if (root / path).exists()]
    if not existing:
        return {"enabled": True, "exit_code": 0, "paths": []}
    completed = subprocess.run(
        ["git", "-C", str(root), "add", "-N", *existing],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    return {
        "enabled": True,
        "exit_code": completed.returncode,
        "paths": existing,
        "stdout": completed.stdout.strip(),
        "stderr": completed.stderr.strip(),
    }


def _internal_audit(root: Path) -> list[str]:
    problems: list[str] = []
    for rel in ROUTING_FILES:
        if not (root / rel).exists():
            problems.append(f"missing required file: {rel}")
    for rel in ["docs/repo_map.json", "docs/task_routes.json", "docs/pitfalls.json", "docs/ai_routing_evolution_policy.json"]:
        path = root / rel
        if path.exists():
            try:
                _load_json(path)
            except Exception as exc:  # noqa: BLE001 - report audit problem only.
                problems.append(f"invalid JSON in {rel}: {exc}")
    return problems


def _run_self_audit(root: Path) -> dict[str, Any]:
    validation = _run([sys.executable, "skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py"], root) if (root / "skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py").exists() else {"exit_code": None, "stderr": "missing skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py"}
    coverage_args = [
        sys.executable,
        "skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py",
        "--routing-only",
    ]
    for rel in ROUTING_FILES:
        coverage_args.extend(["--changed-file", rel])
    coverage_args.append("--json")
    coverage = _run(coverage_args, root) if (root / "skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py").exists() else {"exit_code": None, "stderr": "missing skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py"}
    return {"validation": validation, "coverage": coverage}


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create initial AI Hermes routing files and self-audit them.")
    parser.add_argument("--project-root", type=Path, default=Path.cwd(), help="Target repository root.")
    parser.add_argument("--dry-run", action="store_true", help="Report planned writes without changing files.")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing routing files and skill scripts.")
    parser.add_argument(
        "--no-install-skills",
        action="store_true",
        help="Do not copy bundled AI Hermes project-local skills into the target repo.",
    )
    parser.add_argument(
        "--no-install-tools",
        action="store_true",
        dest="no_install_skills",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--no-intent-to-add", action="store_true", help="Do not run git add -N for generated stable files.")
    parser.add_argument("--skip-self-audit", action="store_true", help="Do not run validation and coverage checks after writing.")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON.")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    root = args.project_root.expanduser().resolve()
    source_skills_dir = Path(__file__).resolve().parents[2]
    if not root.exists():
        print(f"project root does not exist: {root}", file=sys.stderr)
        return 2

    writes: list[WriteResult] = []
    writes.append(_ensure_agents(root, overwrite=args.overwrite, dry_run=args.dry_run))
    writes.append(_write_json(root / "docs/repo_map.json", _repo_map(root), overwrite=args.overwrite, dry_run=args.dry_run))
    writes.append(_write_json(root / "docs/task_routes.json", _task_routes(), overwrite=args.overwrite, dry_run=args.dry_run))
    writes.append(_write_json(root / "docs/pitfalls.json", _pitfalls(), overwrite=args.overwrite, dry_run=args.dry_run))
    writes.append(_write_json(root / "docs/ai_routing_evolution_policy.json", _policy(), overwrite=args.overwrite, dry_run=args.dry_run))
    if not args.no_install_skills:
        writes.extend(_install_project_skills(root, source_skills_dir, overwrite=args.overwrite, dry_run=args.dry_run))

    intent = {"enabled": False, "reason": "dry_run_or_disabled", "paths": []}
    if not args.dry_run and not args.no_intent_to_add:
        intent = _intent_to_add(root, [*ROUTING_FILES, *_existing_anchors(root)])

    audit = {"internal_problems": _internal_audit(root)} if not args.dry_run else {"internal_problems": []}
    if not args.dry_run and not args.skip_self_audit:
        audit.update(_run_self_audit(root))

    report = {
        "project_root": str(root),
        "writes": [{"path": _rel(root, Path(item.path)), "action": item.action} for item in writes],
        "git_intent_to_add": intent,
        "audit": audit,
    }

    validation_exit = audit.get("validation", {}).get("exit_code")
    coverage_exit = audit.get("coverage", {}).get("exit_code")
    internal_ok = not audit.get("internal_problems")
    ok = args.dry_run or (internal_ok and validation_exit in (0, None) and coverage_exit in (0, None))
    report["exit_code_reason"] = "ok" if ok else "audit_failed"

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print(f"AI Hermes routing init: {report['exit_code_reason']}")
        for item in report["writes"]:
            print(f"- {item['action']}: {item['path']}")
        for problem in audit.get("internal_problems", []):
            print(f"Problem: {problem}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
