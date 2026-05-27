#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


VERSION_RE = re.compile(r"^[0-9]+[.][0-9]+[.][0-9]+$")


def run_git(args: list[str]) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()


def changelog_has_version(changelog: Path, version: str) -> bool:
    text = changelog.read_text(encoding="utf-8")
    heading = re.compile(rf"^##\s+(?:\[{re.escape(version)}\]|{re.escape(version)})(?:\s+-\s+.*)?\s*$", re.MULTILINE)
    return heading.search(text) is not None


def main() -> int:
    parser = argparse.ArgumentParser(description="校验发布标签、VERSION 和 CHANGELOG 是否对应同一次发布")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version-file", default="VERSION")
    parser.add_argument("--changelog", default="CHANGELOG.md")
    parser.add_argument("--require-tag-at-head", action="store_true")
    args = parser.parse_args()

    if not args.tag.startswith("v"):
        raise SystemExit(f"标签必须使用 v<major>.<minor>.<patch> 格式：{args.tag}")

    tag_version = args.tag[1:]
    if not VERSION_RE.fullmatch(tag_version):
        raise SystemExit(f"标签必须使用 v<major>.<minor>.<patch> 格式：{args.tag}")

    file_version = Path(args.version_file).read_text(encoding="utf-8").strip()
    if file_version != tag_version:
        raise SystemExit(f"VERSION ({file_version}) 与标签版本 ({tag_version}) 不一致")

    if not changelog_has_version(Path(args.changelog), tag_version):
        raise SystemExit(f"{args.changelog} 缺少版本 {tag_version} 的更新日志段落")

    if args.require_tag_at_head:
        head_sha = run_git(["rev-parse", "HEAD"])
        tag_sha = run_git(["rev-list", "-n", "1", args.tag])
        if head_sha != tag_sha:
            raise SystemExit(f"当前提交 ({head_sha}) 不是标签 {args.tag} 指向的提交 ({tag_sha})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
