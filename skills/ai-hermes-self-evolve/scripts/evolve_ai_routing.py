#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fnmatch
import json
import shlex
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

POLICY_PATH = "docs/ai_routing_evolution_policy.json"
ROUTING_DATA_FILES = [
    "AGENTS.md",
    "docs/repo_map.json",
    "docs/task_routes.json",
    "docs/pitfalls.json",
    "docs/ai_routing_evolution_policy.json",
]
ROUTING_INIT_NEXT_ACTION = "Run $ai-hermes-routing-init before AI Hermes self-evolution."
DEFAULT_ROUTING_ONLY_ALLOWED_GLOBS = [
    "AGENTS.md",
    "docs/ai_routing_evolution_policy.json",
    "docs/repo_map.json",
    "docs/task_routes.json",
    "docs/pitfalls.json",
    "service/AGENTS.md",
    "service/docs/repo_map.json",
    "service/docs/task_routes.json",
    "service/docs/pitfalls.json",
    "skills/ai-hermes-self-evolve/**",
    "skills/ai-hermes-routing-init/**",
    "docs/ai_user_project_memory_policy.json",
    "skills/ai-hermes-user-project-memory/**",
]

DEFAULT_IGNORED_CHANGED_GLOBS = [
    ".antigravitycli/**",
    ".idea/**",
    ".detailed_design_runtime/**",
    ".development_runtime/**",
    ".requirements_clarification_runtime/**",
    ".requirements_review_runtime/**",
    ".routing_init_runtime/**",
    ".task_split_runtime/**",
    ".tmux_stage_locks/**",
    ".tmux_workflow/**",
    ".pytest_cache/**",
    ".ruff_cache/**",
    "__pycache__/**",
    "**/__pycache__/**",
    "*.pyc",
    "*_流水记录.jsonl",
    "*_原始需求.md",
    "*_需求澄清.md",
    "*_详细设计.md",
    "*_任务单.json",
    "*_任务单.md",
    "*_评审记录*.json",
    "*_评审记录*.md",
    "*_任务单评审记录*.md",
    "*_详设评审记录*.md",
    "*_开发前期.json",
    "*_开发前期.md",
    "*_与人类交流.md",
    "*_人机交互澄清记录.md",
    "*_需求分析师反馈.md",
    ".DS_Store",
]

MODULE_PATH_FIELDS = {
    "path",
    "entry_files",
    "first_read_files",
    "then_check_files",
    "related_tests",
    "related_configs",
    "read_before_edit",
}
GROUNDING_PATH_FIELDS = {"evidence", "unsampled_paths"}
PITFALL_PATH_FIELDS = {"related_paths"}


@dataclass(frozen=True)
class DocSet:
    name: str
    docs_dir: Path
    path_root: Path


@dataclass(frozen=True)
class CoverageMatch:
    docset: str
    owner: str
    source: str
    raw_path: str


@dataclass(frozen=True)
class CoverageEntry:
    path: Path
    raw_path: str
    docset: str
    owner: str
    source: str


@dataclass(frozen=True)
class CoveredFile:
    path: str
    matches: list[dict[str, str]]


@dataclass(frozen=True)
class UncoveredFile:
    path: str
    suggested_updates: list[str]


@dataclass(frozen=True)
class RoutingOnlyViolation:
    path: str
    reason: str


@dataclass(frozen=True)
class ChangedEntry:
    path: str
    status: str | None = None
    old_path: str | None = None
    deleted: bool = False


class GitDiffError(RuntimeError):
    pass


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _missing_routing_data_files(project_root: Path) -> list[str]:
    return [rel for rel in ROUTING_DATA_FILES if not (project_root / rel).exists()]


def _path_tokens(value: str) -> list[str]:
    return [part.strip() for part in value.split("+") if part.strip()]


def _looks_like_path(value: str) -> bool:
    if not value or value.startswith("-") or "://" in value:
        return False
    if value.startswith("directory scan of "):
        return True
    if " " in value:
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
    return "/" in value or value.startswith(".") or value.endswith(suffixes)


def _normalize_observed_path(value: str) -> str | None:
    value = value.strip()
    if value.startswith("directory scan of "):
        value = value.removeprefix("directory scan of ").strip()
    return value if _looks_like_path(value) else None


def _resolve(path_root: Path, raw_path: str) -> Path:
    return (path_root / raw_path).resolve()


def _is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _rel(project_root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(project_root).as_posix()
    except ValueError:
        return str(path)


def _changed_arg_to_rel(project_root: Path, raw: str) -> str | None:
    path = Path(raw)
    abs_path = path if path.is_absolute() else project_root / path
    try:
        return abs_path.resolve().relative_to(project_root).as_posix()
    except ValueError:
        return None


def _run_git_paths(project_root: Path, args: Sequence[str]) -> list[str]:
    completed = subprocess.run(
        ["git", "-C", str(project_root), "-c", "core.quotePath=false", *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        return []
    return [
        item.decode("utf-8", errors="surrogateescape")
        for item in completed.stdout.split(b"\0")
        if item
    ]


def _run_git_changed_entries(project_root: Path, diff_args: Sequence[str]) -> list[ChangedEntry]:
    completed = subprocess.run(
        [
            "git",
            "-C",
            str(project_root),
            "-c",
            "core.quotePath=false",
            "diff",
            "-M",
            "--name-status",
            "-z",
            *diff_args,
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        message = completed.stderr.decode("utf-8", errors="replace").strip()
        if not message:
            message = completed.stdout.decode("utf-8", errors="replace").strip()
        raise GitDiffError(message or "git diff failed")

    fields = [
        item.decode("utf-8", errors="surrogateescape")
        for item in completed.stdout.split(b"\0")
        if item
    ]
    entries: list[ChangedEntry] = []
    index = 0
    while index < len(fields):
        status = fields[index]
        index += 1
        code = status[:1]
        if code in {"R", "C"}:
            if index + 1 >= len(fields):
                raise GitDiffError(f"malformed git diff --name-status output for {status}")
            old_path = fields[index]
            new_path = fields[index + 1]
            index += 2
            if code == "R":
                entries.append(ChangedEntry(path=old_path, status=status, deleted=True))
            entries.append(ChangedEntry(path=new_path, status=status, old_path=old_path))
        else:
            if index >= len(fields):
                raise GitDiffError(f"malformed git diff --name-status output for {status}")
            path = fields[index]
            index += 1
            entries.append(ChangedEntry(path=path, status=status, deleted=code == "D"))
    return entries


def _collect_changed_file_paths(project_root: Path, explicit_files: Sequence[str]) -> list[str]:
    if explicit_files:
        normalized = [_changed_arg_to_rel(project_root, item) for item in explicit_files]
        return sorted({item for item in normalized if item})

    changed: set[str] = set()
    for git_args in (
        ["diff", "--name-only", "-z"],
        ["diff", "--cached", "--name-only", "-z"],
        ["ls-files", "--others", "--exclude-standard", "-z"],
    ):
        changed.update(_run_git_paths(project_root, git_args))
    return sorted(changed)


def collect_changed_files(project_root: Path, explicit_files: Sequence[str]) -> list[str]:
    return _collect_changed_file_paths(project_root, explicit_files)


def collect_changed_entries(
    project_root: Path,
    explicit_files: Sequence[str],
    *,
    diff_range: str | None = None,
    base_ref: str | None = None,
    head_ref: str = "HEAD",
) -> list[ChangedEntry]:
    if diff_range:
        return _run_git_changed_entries(project_root, [diff_range])
    if base_ref:
        return _run_git_changed_entries(project_root, [f"{base_ref}..{head_ref}"])
    return [ChangedEntry(path=path) for path in _collect_changed_file_paths(project_root, explicit_files)]


def _iter_path_values(row: Mapping[str, Any], fields: set[str]) -> Iterable[tuple[str, str]]:
    for field in fields:
        value = row.get(field)
        if isinstance(value, str):
            for token in _path_tokens(value):
                normalized = _normalize_observed_path(token)
                if normalized:
                    yield field, normalized
        elif isinstance(value, Sequence) and not isinstance(value, (bytes, bytearray, str)):
            for item in value:
                if isinstance(item, str):
                    normalized = _normalize_observed_path(item)
                    if normalized:
                        yield field, normalized


def _command_path_tokens(command: str) -> Iterable[str]:
    try:
        parts = shlex.split(command)
    except ValueError:
        return []
    tokens: list[str] = []
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
        normalized = _normalize_observed_path(token)
        if normalized:
            tokens.append(normalized)
    return tokens


def _iter_regression_paths(row: Mapping[str, Any]) -> Iterable[tuple[str, str]]:
    value = row.get("minimum_regression")
    if not isinstance(value, Sequence) or isinstance(value, (bytes, bytearray, str)):
        return
    for command in value:
        if isinstance(command, str):
            for token in _command_path_tokens(command):
                yield "minimum_regression", token


def default_docsets(project_root: Path) -> list[DocSet]:
    return [
        DocSet("root", project_root / "docs", project_root),
        DocSet("service", project_root / "service" / "docs", project_root / "service"),
    ]


def build_coverage_index(project_root: Path) -> list[CoverageEntry]:
    entries: list[CoverageEntry] = []
    for docset in default_docsets(project_root):
        repo_map_path = docset.docs_dir / "repo_map.json"
        pitfalls_path = docset.docs_dir / "pitfalls.json"
        if repo_map_path.exists():
            repo_map = _load_json(repo_map_path)
            for module in repo_map.get("modules", []):
                if not isinstance(module, Mapping):
                    continue
                module_id = str(module.get("id", "<unknown>"))
                for field, raw_path in _iter_path_values(module, MODULE_PATH_FIELDS):
                    entries.append(
                        CoverageEntry(
                            path=_resolve(docset.path_root, raw_path),
                            raw_path=raw_path,
                            docset=docset.name,
                            owner=module_id,
                            source=f"module.{field}",
                        )
                    )
                grounding = module.get("grounding")
                if isinstance(grounding, Mapping):
                    for field, raw_path in _iter_path_values(grounding, GROUNDING_PATH_FIELDS):
                        entries.append(
                            CoverageEntry(
                                path=_resolve(docset.path_root, raw_path),
                                raw_path=raw_path,
                                docset=docset.name,
                                owner=module_id,
                                source=f"module.grounding.{field}",
                            )
                        )
                for field, raw_path in _iter_regression_paths(module):
                    entries.append(
                        CoverageEntry(
                            path=_resolve(docset.path_root, raw_path),
                            raw_path=raw_path,
                            docset=docset.name,
                            owner=module_id,
                            source=f"module.{field}",
                        )
                    )
        if pitfalls_path.exists():
            pitfalls = _load_json(pitfalls_path)
            for pitfall in pitfalls.get("pitfalls", []):
                if not isinstance(pitfall, Mapping):
                    continue
                pitfall_id = str(pitfall.get("id", "<unknown>"))
                for field, raw_path in _iter_path_values(pitfall, PITFALL_PATH_FIELDS):
                    entries.append(
                        CoverageEntry(
                            path=_resolve(docset.path_root, raw_path),
                            raw_path=raw_path,
                            docset=docset.name,
                            owner=pitfall_id,
                            source=f"pitfall.{field}",
                        )
                    )
    return entries


def _matches_coverage(changed_abs: Path, entry: CoverageEntry) -> bool:
    covered_abs = entry.path
    if covered_abs.exists() and covered_abs.is_dir():
        return _is_relative_to(changed_abs, covered_abs)
    return changed_abs == covered_abs


def find_coverage(project_root: Path, changed_file: str, entries: Sequence[CoverageEntry]) -> list[CoverageMatch]:
    changed_abs = (project_root / changed_file).resolve()
    matches: list[CoverageMatch] = []
    for entry in entries:
        if _matches_coverage(changed_abs, entry):
            matches.append(
                CoverageMatch(
                    docset=entry.docset,
                    owner=entry.owner,
                    source=entry.source,
                    raw_path=entry.raw_path,
                )
            )
    return matches


def _load_allowed_globs(project_root: Path) -> list[str]:
    policy_file = project_root / POLICY_PATH
    if not policy_file.exists():
        return DEFAULT_ROUTING_ONLY_ALLOWED_GLOBS
    policy = _load_json(policy_file)
    globs = (
        policy.get("commit_scope_guard", {}).get("routing_only_allowed_globs")
        if isinstance(policy, Mapping)
        else None
    )
    if not isinstance(globs, list) or not all(isinstance(item, str) for item in globs):
        return DEFAULT_ROUTING_ONLY_ALLOWED_GLOBS
    return globs


def _load_ignored_changed_globs(project_root: Path) -> list[str]:
    policy_file = project_root / POLICY_PATH
    if not policy_file.exists():
        return DEFAULT_IGNORED_CHANGED_GLOBS
    policy = _load_json(policy_file)
    globs = (
        policy.get("change_detection", {}).get("ignored_globs")
        if isinstance(policy, Mapping)
        else None
    )
    if not isinstance(globs, list) or not all(isinstance(item, str) for item in globs):
        return DEFAULT_IGNORED_CHANGED_GLOBS
    return globs


def _load_deleted_paths(project_root: Path) -> set[str]:
    deleted: set[str] = set()
    for git_args in (
        ["diff", "--name-only", "--diff-filter=D", "-z"],
        ["diff", "--cached", "--name-only", "--diff-filter=D", "-z"],
    ):
        deleted.update(_run_git_paths(project_root, git_args))

    for git_args in (["diff", "--name-status", "-z"], ["diff", "--cached", "--name-status", "-z"]):
        completed = subprocess.run(
            ["git", "-C", str(project_root), "-c", "core.quotePath=false", *git_args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if completed.returncode != 0:
            continue
        fields = [
            item.decode("utf-8", errors="surrogateescape")
            for item in completed.stdout.split(b"\0")
            if item
        ]
        index = 0
        while index < len(fields):
            status = fields[index]
            if status.startswith(("R", "C")) and index + 2 < len(fields):
                old_path = fields[index + 1]
                if not (project_root / old_path).exists():
                    deleted.add(old_path)
                index += 3
            elif status.startswith("D") and index + 1 < len(fields):
                deleted.add(fields[index + 1])
                index += 2
            else:
                index += 2
    return deleted


def _matches_any_glob(path: str, patterns: Sequence[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def filter_ignored_files(
    changed_files: Sequence[str],
    ignored_globs: Sequence[str],
) -> tuple[list[str], list[str]]:
    kept: list[str] = []
    ignored: list[str] = []
    for path in changed_files:
        if _matches_any_glob(path, ignored_globs):
            ignored.append(path)
        else:
            kept.append(path)
    return kept, ignored


def filter_ignored_entries(
    changed_entries: Sequence[ChangedEntry],
    ignored_globs: Sequence[str],
) -> tuple[list[ChangedEntry], list[str]]:
    kept: list[ChangedEntry] = []
    ignored: list[str] = []
    for entry in changed_entries:
        if _matches_any_glob(entry.path, ignored_globs):
            ignored.append(entry.path)
        else:
            kept.append(entry)
    return kept, ignored


def _is_allowed_routing_path(path: str, allowed_globs: Sequence[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) for pattern in allowed_globs)


def _suggest_updates(path: str) -> list[str]:
    suggestions = [
        "Add or update the owning module in docs/repo_map.json with evidence and minimum_regression."
    ]
    if path.startswith("tests/") or "/test_" in path:
        suggestions.append("Link the test through related_tests and minimum_regression.")
    if path.startswith("tools/"):
        suggestions.append("Register the tool in the AI routing tool module before relying on it.")
    if path.startswith("skills/ai-hermes-"):
        suggestions.append("Register the project-local AI Hermes skill script in docs/repo_map.json before relying on it.")
    if path.startswith("docs/") or path.startswith("service/docs/"):
        suggestions.append("Keep routing facts in JSON and re-run skills/ai-hermes-self-evolve/scripts/validate_ai_routing.py.")
    if path.endswith("AGENTS.md"):
        suggestions.append("Keep AGENTS.md protocol-only; move module facts and pitfalls into JSON.")
    return suggestions


def build_report(
    project_root: Path,
    changed_files: Sequence[str] | Sequence[ChangedEntry],
    routing_only: bool,
    ignored_files: Sequence[str] = (),
) -> dict[str, Any]:
    changed_entries = [
        item if isinstance(item, ChangedEntry) else ChangedEntry(path=str(item))
        for item in changed_files
    ]
    coverage_index = build_coverage_index(project_root)
    allowed_globs = _load_allowed_globs(project_root)
    deleted_paths = _load_deleted_paths(project_root)
    resolved_ignored_files = list(ignored_files)
    ignored_file_reasons: dict[str, str] = {}
    covered_files: list[CoveredFile] = []
    uncovered_files: list[UncoveredFile] = []
    routing_only_violations: list[RoutingOnlyViolation] = []

    for entry in changed_entries:
        changed_file = entry.path
        changed_abs = (project_root / changed_file).resolve()
        matches = find_coverage(project_root, changed_file, coverage_index)
        if matches:
            covered_files.append(
                CoveredFile(
                    path=changed_file,
                    matches=[asdict(match) for match in matches],
                )
            )
        elif entry.deleted:
            resolved_ignored_files.append(changed_file)
            ignored_file_reasons[changed_file] = (
                "renamed_from_in_diff_range"
                if entry.status and entry.status.startswith("R")
                else "deleted_in_diff_range"
            )
            continue
        elif changed_file in deleted_paths and not changed_abs.exists():
            resolved_ignored_files.append(changed_file)
            ignored_file_reasons[changed_file] = "deleted_in_worktree_or_index"
            continue
        else:
            uncovered_files.append(
                UncoveredFile(path=changed_file, suggested_updates=_suggest_updates(changed_file))
            )
        if routing_only and not _is_allowed_routing_path(changed_file, allowed_globs):
            routing_only_violations.append(
                RoutingOnlyViolation(
                    path=changed_file,
                    reason="path is outside routing-only allowed globs",
                )
            )

    suggested_updates: list[str] = []
    if uncovered_files:
        suggested_updates.append("Update docs/repo_map.json or docs/pitfalls.json so changed files are represented by routing memory.")
    if routing_only_violations:
        suggested_updates.append("Remove, ignore, or separately handle non-routing changes before a routing-only commit.")

    if routing_only_violations:
        exit_code_reason = "routing_only_violations"
    elif uncovered_files:
        exit_code_reason = "uncovered_files"
    else:
        exit_code_reason = "ok"

    return {
        "changed_files": [entry.path for entry in changed_entries],
        "ignored_files": resolved_ignored_files,
        "ignored_file_reasons": ignored_file_reasons,
        "covered_files": [asdict(item) for item in covered_files],
        "uncovered_files": [asdict(item) for item in uncovered_files],
        "routing_only_violations": [asdict(item) for item in routing_only_violations],
        "suggested_updates": suggested_updates,
        "missing_required_files": [],
        "next_action": None,
        "exit_code_reason": exit_code_reason,
    }


def build_not_initialized_report(
    changed_files: Sequence[str],
    ignored_files: Sequence[str],
    missing_required_files: Sequence[str],
) -> dict[str, Any]:
    return {
        "changed_files": list(changed_files),
        "ignored_files": list(ignored_files),
        "ignored_file_reasons": {},
        "covered_files": [],
        "uncovered_files": [],
        "routing_only_violations": [],
        "suggested_updates": [ROUTING_INIT_NEXT_ACTION],
        "missing_required_files": list(missing_required_files),
        "next_action": ROUTING_INIT_NEXT_ACTION,
        "exit_code_reason": "routing_not_initialized",
    }


def build_error_report(
    *,
    reason: str,
    message: str,
    changed_files: Sequence[str] = (),
    ignored_files: Sequence[str] = (),
) -> dict[str, Any]:
    return {
        "changed_files": list(changed_files),
        "ignored_files": list(ignored_files),
        "ignored_file_reasons": {},
        "covered_files": [],
        "uncovered_files": [],
        "routing_only_violations": [],
        "suggested_updates": [],
        "missing_required_files": [],
        "next_action": None,
        "exit_code_reason": reason,
        "error": message,
    }


def _print_text_report(report: Mapping[str, Any]) -> None:
    print("AI routing evolution check")
    print(f"- changed_files: {len(report['changed_files'])}")
    print(f"- ignored_files: {len(report['ignored_files'])}")
    print(f"- covered_files: {len(report['covered_files'])}")
    print(f"- uncovered_files: {len(report['uncovered_files'])}")
    print(f"- routing_only_violations: {len(report['routing_only_violations'])}")
    if report["uncovered_files"]:
        print("Uncovered files:")
        for item in report["uncovered_files"]:
            print(f"- {item['path']}")
    if report["routing_only_violations"]:
        print("Routing-only violations:")
        for item in report["routing_only_violations"]:
            print(f"- {item['path']}: {item['reason']}")
    if report.get("ignored_file_reasons"):
        print("Ignored file reasons:")
        for path, reason in report["ignored_file_reasons"].items():
            print(f"- {path}: {reason}")
    for suggestion in report["suggested_updates"]:
        print(f"Suggestion: {suggestion}")
    if report.get("missing_required_files"):
        print("Missing required routing files:")
        for path in report["missing_required_files"]:
            print(f"- {path}")
    if report.get("next_action"):
        print(f"Next action: {report['next_action']}")
    if report.get("error"):
        print(f"Error: {report['error']}")
    print(f"exit_code_reason: {report['exit_code_reason']}")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check whether changed files are represented by AI Hermes routing memory."
    )
    parser.add_argument(
        "--project-root",
        type=Path,
        default=Path.cwd(),
        help="Repository root containing AGENTS.md and docs/.",
    )
    parser.add_argument(
        "--changed-file",
        action="append",
        default=[],
        help="Explicit changed file to audit. Repeat to avoid scanning the whole dirty worktree.",
    )
    parser.add_argument(
        "--diff-range",
        help="Git diff range to audit with name-status preserved, e.g. origin/main..HEAD or origin/main...HEAD.",
    )
    parser.add_argument(
        "--base-ref",
        help="Base ref for range-aware audit. Uses <base-ref>..<head-ref>.",
    )
    parser.add_argument(
        "--head-ref",
        help="Head ref for --base-ref range-aware audit. Defaults to HEAD when --base-ref is set.",
    )
    parser.add_argument(
        "--routing-only",
        action="store_true",
        help="Fail if changed files include paths outside routing/tool governance files.",
    )
    parser.add_argument(
        "--include-ignored",
        action="store_true",
        help="Include local runtime and IDE artifacts that are ignored by default scanning.",
    )
    parser.add_argument("--json", action="store_true", help="Emit stable machine-readable JSON.")
    return parser.parse_args(argv)


def _validate_arg_combinations(args: argparse.Namespace) -> str | None:
    range_options = [bool(args.diff_range), bool(args.base_ref)]
    if sum(range_options) > 1:
        return "Use only one of --diff-range or --base-ref."
    if args.changed_file and (args.diff_range or args.base_ref or args.head_ref):
        return "Do not combine --changed-file with range audit options."
    if args.head_ref and not args.base_ref:
        return "Use --head-ref only together with --base-ref."
    return None


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    project_root = args.project_root.expanduser().resolve()
    invalid_args = _validate_arg_combinations(args)
    if invalid_args:
        report = build_error_report(reason="invalid_args", message=invalid_args)
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            _print_text_report(report)
        return 1

    try:
        changed_entries = collect_changed_entries(
            project_root,
            args.changed_file,
            diff_range=args.diff_range,
            base_ref=args.base_ref,
            head_ref=args.head_ref or "HEAD",
        )
    except GitDiffError as exc:
        report = build_error_report(reason="git_diff_failed", message=str(exc))
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            _print_text_report(report)
        return 1

    changed_files = [entry.path for entry in changed_entries]
    ignored_files: list[str] = []
    missing_required_files = _missing_routing_data_files(project_root)
    if missing_required_files:
        report = build_not_initialized_report(changed_files, ignored_files, missing_required_files)
    elif not args.changed_file and not args.diff_range and not args.base_ref and not args.include_ignored:
        ignored_globs = _load_ignored_changed_globs(project_root)
        changed_entries, ignored_files = filter_ignored_entries(changed_entries, ignored_globs)
        report = build_report(project_root, changed_entries, args.routing_only, ignored_files)
    else:
        report = build_report(project_root, changed_entries, args.routing_only, ignored_files)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        _print_text_report(report)

    return 0 if report["exit_code_reason"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
