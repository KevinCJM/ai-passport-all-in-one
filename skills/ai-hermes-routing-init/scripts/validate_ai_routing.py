#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fnmatch
import importlib.util
import json
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


LEGAL_FACT_STATUS = {
    "grounded",
    "mixed",
    "needs_code_confirmation",
    "unknown",
    "out_of_scope",
}

PATH_LIST_FIELDS = {
    "entry_files",
    "first_read_files",
    "then_check_files",
    "related_tests",
    "related_configs",
    "read_before_edit",
}

MODULE_REF_FIELDS = {
    "child_modules",
    "upstream_dependencies",
    "downstream_dependencies",
    "affected_modules",
}

PITFALL_REF_FIELDS = {"pitfall_ids"}

POLICY_PATH = "docs/ai_routing_evolution_policy.json"
ROUTING_DATA_FILES = [
    "AGENTS.md",
    "docs/repo_map.json",
    "docs/task_routes.json",
    "docs/pitfalls.json",
    "docs/ai_routing_evolution_policy.json",
]
AGENTS_PROTOCOL_BEGIN = "<!-- AI-HERMES-ROUTING-PROTOCOL:BEGIN -->"
AGENTS_PROTOCOL_END = "<!-- AI-HERMES-ROUTING-PROTOCOL:END -->"
AGENTS_SELF_EVOLUTION_HEADER = "# AI Routing Self-Evolution"
REQUIRED_AGENTS_ROUTING_PROTOCOL_SNIPPETS = {
    "# AI Hermes Routing Protocol",
    "## Required Read Order",
    "## Routing Ownership",
    "## AI Routing Validation",
    "# AI Routing Self-Evolution",
    "Keep routing facts in JSON only",
    "Keep `AGENTS.md` protocol-only",
    "docs/task_routes.json",
    "docs/repo_map.json",
    "docs/pitfalls.json",
}
REQUIRED_AGENTS_SELF_EVOLUTION_SNIPPETS = {
    "governance only",
    "skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py",
    "verified hidden contracts",
    "Update `AGENTS.md` only when protocol",
    "skills/ai-hermes-self-evolve/scripts/evolve_ai_routing.py --routing-only",
    "skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py",
}
REQUIRED_POLICY_INVARIANTS = {
    "task_routes_remains_single_machine_owner",
    "policy_must_not_duplicate_module_operational_lists",
    "policy_must_not_duplicate_route_match_rules",
    "policy_must_not_duplicate_pitfall_facts",
    "agents_md_stays_protocol_only",
    "business_code_is_not_modified_by_routing_only_tasks",
    "stable_routing_references_are_git_tracked_or_explicit_artifacts",
}
REQUIRED_EVOLVE_JSON_FIELDS = {
    "changed_files",
    "ignored_files",
    "covered_files",
    "uncovered_files",
    "routing_only_violations",
    "suggested_updates",
    "missing_required_files",
    "next_action",
    "exit_code_reason",
}


@dataclass(frozen=True)
class DocSet:
    name: str
    docs_dir: Path
    path_root: Path


@dataclass
class Problem:
    docset: str
    path: str
    message: str

    def format(self) -> str:
        return f"[{self.docset}] {self.path}: {self.message}"


@dataclass
class ReproducibilityContext:
    project_root: Path
    enabled: bool
    tracked_paths: set[str]
    allowed_untracked_globs: list[str]
    reported_untracked_paths: set[str]


def _load_json(path: Path, docset: str, problems: list[Problem]) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        problems.append(Problem(docset, str(path), "missing required JSON file"))
    except json.JSONDecodeError as exc:
        problems.append(Problem(docset, str(path), f"invalid JSON: {exc}"))
    return {}


def _missing_routing_data_files(project_root: Path) -> list[str]:
    return [rel for rel in ROUTING_DATA_FILES if not (project_root / rel).exists()]


def _marker_block(text: str) -> tuple[str | None, str | None]:
    begin_count = text.count(AGENTS_PROTOCOL_BEGIN)
    end_count = text.count(AGENTS_PROTOCOL_END)
    if begin_count == 0 and end_count == 0:
        return None, None
    if begin_count != 1 or end_count != 1:
        return None, "malformed AI Hermes routing protocol marker block"
    begin_index = text.find(AGENTS_PROTOCOL_BEGIN)
    end_index = text.find(AGENTS_PROTOCOL_END)
    if begin_index < 0 or end_index < begin_index:
        return None, "malformed AI Hermes routing protocol marker block order"
    return text[begin_index : end_index + len(AGENTS_PROTOCOL_END)], None


def _iter_refs(value: Any) -> Iterable[str]:
    if isinstance(value, str):
        yield value
    elif isinstance(value, Mapping):
        module_id = value.get("module_id") or value.get("id")
        if isinstance(module_id, str):
            yield module_id
    elif isinstance(value, Sequence) and not isinstance(value, (bytes, bytearray)):
        for item in value:
            yield from _iter_refs(item)


def _path_tokens(value: str) -> list[str]:
    return [part.strip() for part in value.split("+") if part.strip()]


def _resolve(root: Path, raw_path: str) -> Path:
    return (root / raw_path).resolve()


def _rel_to_project(project_root: Path, path: Path) -> str | None:
    try:
        return path.resolve().relative_to(project_root).as_posix()
    except ValueError:
        return None


def _matches_any_glob(path: str, patterns: Sequence[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def _load_git_tracked_paths(project_root: Path) -> set[str] | None:
    completed = subprocess.run(
        ["git", "-C", str(project_root), "-c", "core.quotePath=false", "ls-files", "-z"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        return None
    return {
        item.decode("utf-8", errors="surrogateescape")
        for item in completed.stdout.split(b"\0")
        if item
    }


def _is_tracked_or_contains_tracked(rel_path: str, tracked_paths: set[str]) -> bool:
    if rel_path in tracked_paths:
        return True
    prefix = f"{rel_path.rstrip('/')}/"
    return any(path.startswith(prefix) for path in tracked_paths)


def _check_reproducible_reference(
    *,
    docset: DocSet,
    owner_path: str,
    raw_path: str,
    resolved_path: Path,
    repro: ReproducibilityContext | None,
    problems: list[Problem],
) -> None:
    if repro is None or not repro.enabled:
        return
    rel_path = _rel_to_project(repro.project_root, resolved_path)
    if rel_path is None:
        return
    if _matches_any_glob(rel_path, repro.allowed_untracked_globs):
        return
    if _is_tracked_or_contains_tracked(rel_path, repro.tracked_paths):
        return
    if rel_path in repro.reported_untracked_paths:
        return
    repro.reported_untracked_paths.add(rel_path)
    problems.append(
        Problem(
            docset.name,
            owner_path,
            f"referenced path exists but is not git tracked or explicitly allowed as artifact: {raw_path}",
        )
    )


def _is_probable_path(token: str) -> bool:
    if token.startswith("-"):
        return False
    if "://" in token:
        return False
    if token in {"python", "pytest", "bash", "sh"}:
        return False
    suffixes = (
        ".py",
        ".json",
        ".yaml",
        ".yml",
        ".toml",
        ".md",
        ".sh",
        ".txt",
        ".ini",
        ".cfg",
    )
    return "/" in token or token.startswith(".") or token.endswith(suffixes)


def _check_path(
    *,
    docset: DocSet,
    owner_path: str,
    raw_path: str,
    repro: ReproducibilityContext | None,
    problems: list[Problem],
) -> None:
    for token in _path_tokens(raw_path):
        path = _resolve(docset.path_root, token)
        if not path.exists():
            problems.append(
                Problem(docset.name, owner_path, f"referenced path does not exist: {token}")
            )
            continue
        _check_reproducible_reference(
            docset=docset,
            owner_path=owner_path,
            raw_path=token,
            resolved_path=path,
            repro=repro,
            problems=problems,
        )


def _check_path_list(
    *,
    docset: DocSet,
    owner_path: str,
    values: Any,
    repro: ReproducibilityContext | None,
    problems: list[Problem],
) -> None:
    if values is None:
        return
    if not isinstance(values, list):
        problems.append(Problem(docset.name, owner_path, "path list field must be an array"))
        return
    for item in values:
        if isinstance(item, str):
            _check_path(
                docset=docset,
                owner_path=owner_path,
                raw_path=item,
                repro=repro,
                problems=problems,
            )


def _command_executable_exists(executable: str) -> bool:
    if executable in {"python", "python3"}:
        return True
    if executable == "pytest":
        return shutil.which("pytest") is not None or importlib.util.find_spec("pytest") is not None
    if "/" in executable:
        return Path(executable).expanduser().exists()
    return shutil.which(executable) is not None


def _check_command(
    *,
    docset: DocSet,
    owner_path: str,
    command: str,
    repro: ReproducibilityContext | None,
    problems: list[Problem],
) -> None:
    try:
        parts = shlex.split(command)
    except ValueError as exc:
        problems.append(Problem(docset.name, owner_path, f"invalid command syntax: {exc}"))
        return
    if not parts:
        problems.append(Problem(docset.name, owner_path, "empty regression command"))
        return

    if not _command_executable_exists(parts[0]):
        problems.append(
            Problem(docset.name, owner_path, f"command executable not found: {parts[0]}")
        )

    skip_next = False
    for index, token in enumerate(parts[1:], start=1):
        if skip_next:
            skip_next = False
            continue
        if token in {"-m", "-c", "--config", "--rootdir"}:
            skip_next = True
            continue
        if token.startswith("-"):
            continue
        if index >= 2 and parts[index - 1] == "-m":
            continue
        if _is_probable_path(token):
            _check_path(
                docset=docset,
                owner_path=owner_path,
                raw_path=token,
                repro=repro,
                problems=problems,
            )


def _check_commands(
    *,
    docset: DocSet,
    owner_path: str,
    values: Any,
    repro: ReproducibilityContext | None,
    problems: list[Problem],
) -> None:
    if values is None:
        return
    if not isinstance(values, list):
        problems.append(Problem(docset.name, owner_path, "minimum_regression must be an array"))
        return
    for command in values:
        if isinstance(command, str):
            _check_command(
                docset=docset,
                owner_path=owner_path,
                command=command,
                repro=repro,
                problems=problems,
            )


def _check_unique_ids(
    *,
    docset: str,
    owner_path: str,
    rows: Sequence[Mapping[str, Any]],
    label: str,
    problems: list[Problem],
) -> set[str]:
    seen: set[str] = set()
    for row in rows:
        row_id = row.get("id")
        if not isinstance(row_id, str) or not row_id:
            problems.append(Problem(docset, owner_path, f"{label} missing string id"))
            continue
        if row_id in seen:
            problems.append(Problem(docset, owner_path, f"duplicate {label} id: {row_id}"))
        seen.add(row_id)
    return seen


def _check_grounding(
    *,
    docset: DocSet,
    module: Mapping[str, Any],
    owner_path: str,
    repro: ReproducibilityContext | None,
    problems: list[Problem],
) -> None:
    grounding = module.get("grounding")
    module_id = module.get("id", "<unknown>")
    if not isinstance(grounding, Mapping):
        problems.append(Problem(docset.name, owner_path, f"module {module_id} missing grounding"))
        return
    fact_status = grounding.get("fact_status")
    if fact_status not in LEGAL_FACT_STATUS:
        problems.append(
            Problem(
                docset.name,
                owner_path,
                f"module {module_id} has invalid fact_status: {fact_status!r}",
            )
        )
    unsampled = grounding.get("unsampled_paths", [])
    if not isinstance(unsampled, list):
        problems.append(
            Problem(docset.name, owner_path, f"module {module_id} unsampled_paths must be an array")
        )
    else:
        _check_path_list(
            docset=docset,
            owner_path=f"{owner_path}:{module_id}:grounding.unsampled_paths",
            values=unsampled,
            repro=repro,
            problems=problems,
        )


def _check_route_codes(
    *,
    docset_name: str,
    task_routes: Mapping[str, Any],
    problems: list[Problem],
) -> None:
    rule_catalog = task_routes.get("rule_catalog")
    if not isinstance(rule_catalog, Mapping):
        return
    mapping = {
        "expand_search_codes": "expand_search",
        "stop_and_verify_codes": "stop_and_verify",
        "fact_check_codes": "fact_check",
    }
    for route in task_routes.get("routes", []):
        route_id = route.get("id", "<unknown>")
        for field, catalog_name in mapping.items():
            catalog = rule_catalog.get(catalog_name, {})
            valid_codes = set(catalog) if isinstance(catalog, Mapping) else set()
            for code in route.get(field) or []:
                if code not in valid_codes:
                    problems.append(
                        Problem(
                            docset_name,
                            "task_routes.json",
                            f"route {route_id} references missing {catalog_name} code: {code}",
                        )
                    )


def _check_string_list_contains(
    *,
    docset: str,
    owner_path: str,
    field_name: str,
    values: Any,
    required_values: set[str],
    problems: list[Problem],
) -> None:
    if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
        problems.append(Problem(docset, owner_path, f"{field_name} must be a string array"))
        return
    missing = sorted(required_values - set(values))
    for value in missing:
        problems.append(
            Problem(docset, owner_path, f"{field_name} missing required value: {value}")
        )


def _load_reproducibility_context(
    project_root: Path,
    problems: list[Problem],
) -> ReproducibilityContext:
    policy_path = project_root / POLICY_PATH
    policy = _load_json(policy_path, "root", problems) if policy_path.exists() else {}
    if not isinstance(policy, Mapping):
        policy = {}

    change_detection = policy.get("change_detection")
    ignored_globs = []
    if isinstance(change_detection, Mapping):
        raw_ignored = change_detection.get("ignored_globs")
        if isinstance(raw_ignored, list):
            ignored_globs = [item for item in raw_ignored if isinstance(item, str)]

    repro_policy = policy.get("reproducibility")
    tracked_policy = (
        repro_policy.get("tracked_reference_check")
        if isinstance(repro_policy, Mapping)
        else {}
    )
    if not isinstance(tracked_policy, Mapping):
        tracked_policy = {}

    enabled = tracked_policy.get("enabled", True) is not False
    raw_allowed = tracked_policy.get("allowed_untracked_globs", ignored_globs)
    allowed_untracked_globs = (
        [item for item in raw_allowed if isinstance(item, str)]
        if isinstance(raw_allowed, list)
        else ignored_globs
    )

    tracked_paths = _load_git_tracked_paths(project_root) if enabled else set()
    if enabled and tracked_paths is None:
        problems.append(
            Problem(
                "root",
                "git",
                "cannot inspect git tracked files for reproducibility validation",
            )
        )
        tracked_paths = set()

    return ReproducibilityContext(
        project_root=project_root,
        enabled=enabled,
        tracked_paths=tracked_paths,
        allowed_untracked_globs=allowed_untracked_globs,
        reported_untracked_paths=set(),
    )


def validate_root_governance(
    project_root: Path,
    repro: ReproducibilityContext | None,
) -> list[Problem]:
    problems: list[Problem] = []
    root_docset = DocSet("root", project_root / "docs", project_root)

    agents_path = project_root / "AGENTS.md"
    try:
        agents_text = agents_path.read_text(encoding="utf-8")
    except FileNotFoundError:
        problems.append(Problem("root", "AGENTS.md", "missing AGENTS.md"))
        agents_text = ""
    marker_block, marker_problem = _marker_block(agents_text)
    if marker_problem:
        problems.append(
            Problem(
                "root",
                "AGENTS.md",
                marker_problem,
            )
        )
    if marker_block:
        missing_protocol_snippets = sorted(
            snippet
            for snippet in REQUIRED_AGENTS_ROUTING_PROTOCOL_SNIPPETS
            if snippet not in marker_block
        )
        for snippet in missing_protocol_snippets:
            problems.append(
                Problem(
                    "root",
                    "AGENTS.md",
                    f"AI Hermes routing protocol marker block missing snippet: {snippet}",
                )
            )
    if AGENTS_SELF_EVOLUTION_HEADER not in agents_text:
        problems.append(
            Problem(
                "root",
                "AGENTS.md",
                f"missing self-evolution protocol header: {AGENTS_SELF_EVOLUTION_HEADER}",
            )
        )
    missing_snippets = sorted(
        snippet for snippet in REQUIRED_AGENTS_SELF_EVOLUTION_SNIPPETS if snippet not in agents_text
    )
    for snippet in missing_snippets:
        problems.append(
            Problem("root", "AGENTS.md", f"missing self-evolution protocol snippet: {snippet}")
        )

    policy = _load_json(project_root / POLICY_PATH, "root", problems)
    if not isinstance(policy, Mapping):
        problems.append(Problem("root", POLICY_PATH, "policy must be a JSON object"))
        return problems
    if policy.get("role") != "governance_only":
        problems.append(Problem("root", POLICY_PATH, "role must be governance_only"))

    owners = policy.get("authoritative_owners")
    if not isinstance(owners, Mapping):
        problems.append(Problem("root", POLICY_PATH, "authoritative_owners must be an object"))
    else:
        expected = {
            "task_matching_module_expansion_and_operational_list_merge": "docs/task_routes.json",
            "module_facts_and_operational_lists": "docs/repo_map.json",
            "pitfall_facts": "docs/pitfalls.json",
            "agent_protocol": "AGENTS.md",
        }
        for key, value in expected.items():
            if owners.get(key) != value:
                problems.append(
                    Problem(
                        "root",
                        POLICY_PATH,
                        f"authoritative_owners.{key} must be {value}",
                    )
                )

    _check_string_list_contains(
        docset="root",
        owner_path=POLICY_PATH,
        field_name="invariants",
        values=policy.get("invariants"),
        required_values=REQUIRED_POLICY_INVARIANTS,
        problems=problems,
    )

    guard = policy.get("commit_scope_guard")
    if not isinstance(guard, Mapping):
        problems.append(Problem("root", POLICY_PATH, "commit_scope_guard must be an object"))
    else:
        globs = guard.get("routing_only_allowed_globs")
        if not isinstance(globs, list) or not all(isinstance(item, str) for item in globs):
            problems.append(
                Problem(
                    "root",
                    POLICY_PATH,
                    "commit_scope_guard.routing_only_allowed_globs must be a string array",
                )
            )

    change_detection = policy.get("change_detection")
    if change_detection is not None:
        if not isinstance(change_detection, Mapping):
            problems.append(Problem("root", POLICY_PATH, "change_detection must be an object"))
        else:
            ignored_globs = change_detection.get("ignored_globs")
            if not isinstance(ignored_globs, list) or not all(
                isinstance(item, str) for item in ignored_globs
            ):
                problems.append(
                    Problem(
                        "root",
                        POLICY_PATH,
                        "change_detection.ignored_globs must be a string array",
                    )
                )

    reproducibility = policy.get("reproducibility")
    if reproducibility is not None:
        if not isinstance(reproducibility, Mapping):
            problems.append(Problem("root", POLICY_PATH, "reproducibility must be an object"))
        else:
            tracked_check = reproducibility.get("tracked_reference_check")
            if not isinstance(tracked_check, Mapping):
                problems.append(
                    Problem(
                        "root",
                        POLICY_PATH,
                        "reproducibility.tracked_reference_check must be an object",
                    )
                )
            else:
                if not isinstance(tracked_check.get("enabled"), bool):
                    problems.append(
                        Problem(
                            "root",
                            POLICY_PATH,
                            "reproducibility.tracked_reference_check.enabled must be boolean",
                        )
                    )
                allowed = tracked_check.get("allowed_untracked_globs")
                if not isinstance(allowed, list) or not all(
                    isinstance(item, str) for item in allowed
                ):
                    problems.append(
                        Problem(
                            "root",
                            POLICY_PATH,
                            "reproducibility.tracked_reference_check.allowed_untracked_globs must be a string array",
                        )
                    )

    evolve_contract = (
        policy.get("cli_contracts", {}).get("evolve_ai_routing")
        if isinstance(policy.get("cli_contracts"), Mapping)
        else None
    )
    if not isinstance(evolve_contract, Mapping):
        problems.append(
            Problem("root", POLICY_PATH, "cli_contracts.evolve_ai_routing must be an object")
        )
    else:
        if evolve_contract.get("must_not_modify_files") is not True:
            problems.append(
                Problem(
                    "root",
                    POLICY_PATH,
                    "cli_contracts.evolve_ai_routing.must_not_modify_files must be true",
                )
            )
        _check_string_list_contains(
            docset="root",
            owner_path=POLICY_PATH,
            field_name="cli_contracts.evolve_ai_routing.stable_json_fields",
            values=evolve_contract.get("stable_json_fields"),
            required_values=REQUIRED_EVOLVE_JSON_FIELDS,
            problems=problems,
        )
        _check_commands(
            docset=root_docset,
            owner_path=f"{POLICY_PATH}:cli_contracts.evolve_ai_routing.smoke_commands",
            values=evolve_contract.get("smoke_commands"),
            repro=repro,
            problems=problems,
        )

    return problems


def validate_docset(
    docset: DocSet,
    repro: ReproducibilityContext | None,
) -> list[Problem]:
    problems: list[Problem] = []
    repo_map = _load_json(docset.docs_dir / "repo_map.json", docset.name, problems)
    task_routes = _load_json(docset.docs_dir / "task_routes.json", docset.name, problems)
    pitfalls = _load_json(docset.docs_dir / "pitfalls.json", docset.name, problems)

    modules = repo_map.get("modules", []) if isinstance(repo_map, Mapping) else []
    routes = task_routes.get("routes", []) if isinstance(task_routes, Mapping) else []
    pitfall_rows = pitfalls.get("pitfalls", []) if isinstance(pitfalls, Mapping) else []

    if not isinstance(modules, list):
        problems.append(Problem(docset.name, "repo_map.json", "modules must be an array"))
        modules = []
    if not isinstance(routes, list):
        problems.append(Problem(docset.name, "task_routes.json", "routes must be an array"))
        routes = []
    if not isinstance(pitfall_rows, list):
        problems.append(Problem(docset.name, "pitfalls.json", "pitfalls must be an array"))
        pitfall_rows = []

    module_ids = _check_unique_ids(
        docset=docset.name,
        owner_path="repo_map.json",
        rows=modules,
        label="module",
        problems=problems,
    )
    route_ids = _check_unique_ids(
        docset=docset.name,
        owner_path="task_routes.json",
        rows=routes,
        label="route",
        problems=problems,
    )
    pitfall_ids = _check_unique_ids(
        docset=docset.name,
        owner_path="pitfalls.json",
        rows=pitfall_rows,
        label="pitfall",
        problems=problems,
    )
    del route_ids

    for module in modules:
        module_id = module.get("id", "<unknown>")
        owner = f"repo_map.json:{module_id}"
        path_value = module.get("path")
        if isinstance(path_value, str):
            _check_path(
                docset=docset,
                owner_path=owner,
                raw_path=path_value,
                repro=repro,
                problems=problems,
            )
        for field in PATH_LIST_FIELDS:
            _check_path_list(
                docset=docset,
                owner_path=f"{owner}:{field}",
                values=module.get(field),
                repro=repro,
                problems=problems,
            )
        _check_commands(
            docset=docset,
            owner_path=f"{owner}:minimum_regression",
            values=module.get("minimum_regression"),
            repro=repro,
            problems=problems,
        )
        for field in MODULE_REF_FIELDS:
            for ref in _iter_refs(module.get(field)):
                if ref not in module_ids:
                    problems.append(
                        Problem(docset.name, owner, f"unknown module reference in {field}: {ref}")
                    )
        for ref in _iter_refs(module.get("pitfall_ids")):
            if ref not in pitfall_ids:
                problems.append(Problem(docset.name, owner, f"unknown pitfall id: {ref}"))
        _check_grounding(
            docset=docset,
            module=module,
            owner_path=owner,
            repro=repro,
            problems=problems,
        )

    for route in routes:
        route_id = route.get("id", "<unknown>")
        owner = f"task_routes.json:{route_id}"
        for field in ("first_read_modules", "expand_to_modules"):
            for ref in _iter_refs(route.get(field)):
                if ref not in module_ids:
                    problems.append(
                        Problem(docset.name, owner, f"unknown module reference in {field}: {ref}")
                    )
        for ref in _iter_refs(route.get("pitfall_ids")):
            if ref not in pitfall_ids:
                problems.append(Problem(docset.name, owner, f"unknown pitfall id: {ref}"))

    for pitfall in pitfall_rows:
        pitfall_id = pitfall.get("id", "<unknown>")
        owner = f"pitfalls.json:{pitfall_id}"
        for ref in _iter_refs(pitfall.get("affected_modules")):
            if ref not in module_ids:
                problems.append(
                    Problem(docset.name, owner, f"unknown affected module: {ref}")
                )
        _check_path_list(
            docset=docset,
            owner_path=f"{owner}:related_paths",
            values=pitfall.get("related_paths"),
            repro=repro,
            problems=problems,
        )

    _check_route_codes(
        docset_name=docset.name, task_routes=task_routes, problems=problems
    )
    return problems


def default_docsets(project_root: Path) -> list[DocSet]:
    docsets = [DocSet("root", project_root / "docs", project_root)]
    service_docs = project_root / "service" / "docs"
    if service_docs.exists():
        docsets.append(DocSet("service", service_docs, project_root / "service"))
    return docsets


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate AI routing maps, module references, pitfalls, paths, and regression commands."
    )
    parser.add_argument(
        "--project-root",
        type=Path,
        default=Path.cwd(),
        help="Repository root containing docs/ and service/docs/.",
    )
    parser.add_argument(
        "--docset",
        choices=["root", "service", "all"],
        default="all",
        help="Limit validation to one routing docset.",
    )
    parser.add_argument(
        "--skip-reproducibility",
        action="store_true",
        help="Skip git-tracked checks for routing references.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    project_root = args.project_root.expanduser().resolve()
    missing_routing_data_files = _missing_routing_data_files(project_root)
    docsets = default_docsets(project_root)
    if args.docset != "all":
        docsets = [docset for docset in docsets if docset.name == args.docset]

    problems: list[Problem] = []
    repro = (
        ReproducibilityContext(project_root, False, set(), [], set())
        if args.skip_reproducibility
        else _load_reproducibility_context(project_root, problems)
    )
    for docset in docsets:
        problems.extend(validate_docset(docset, repro))
    if args.docset in {"root", "all"}:
        problems.extend(validate_root_governance(project_root, repro))

    if problems:
        print("AI routing validation failed:", file=sys.stderr)
        for problem in problems:
            print(f"- {problem.format()}", file=sys.stderr)
        if missing_routing_data_files:
            print(
                "Next action: run $ai-hermes-routing-init before AI Hermes self-evolution.",
                file=sys.stderr,
            )
        return 1

    names = ", ".join(docset.name for docset in docsets)
    print(f"AI routing validation passed: {names}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
