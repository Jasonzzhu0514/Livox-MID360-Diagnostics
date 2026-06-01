#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path


def extract_section(changelog: Path, version: str) -> str:
    text = changelog.read_text(encoding="utf-8")
    heading = re.compile(rf"^##\s+(?:\[{re.escape(version)}\]|{re.escape(version)})(?:\s+-\s+.*)?\s*$", re.MULTILINE)
    match = heading.search(text)
    if not match:
        raise SystemExit(f"未在 {changelog} 中找到版本 {version} 的更新日志")

    start = match.end()
    next_heading = re.search(r"^##\s+", text[start:], re.MULTILINE)
    end = start + next_heading.start() if next_heading else len(text)
    return text[start:end].strip()


def main() -> int:
    parser = argparse.ArgumentParser(description="从 CHANGELOG.md 提取指定版本的 Release 说明")
    parser.add_argument("--version", required=True)
    parser.add_argument("--changelog", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--current-tag", required=True)
    parser.add_argument("--previous-tag", default="")
    args = parser.parse_args()

    body = extract_section(Path(args.changelog), args.version)
    title = f"Livox MID360 Diagnostics v{args.version}"
    lines = [f"# {title}", "", body]

    if args.previous_tag:
        compare_url = f"https://github.com/{args.repo}/compare/{args.previous_tag}...{args.current_tag}"
        lines.extend(["", f"完整变更对比：{compare_url}"])

    Path(args.output).write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
