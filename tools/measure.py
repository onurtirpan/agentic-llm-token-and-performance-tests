"""Measure the size of every Task Service implementation, across every tier.

Usage:
    python tools/measure.py            # every tier that exists
    python tools/measure.py small      # one tier
    python tools/measure.py small mid

The tool reports lines, characters, bytes and tokens for the application source
of each language. It reports the build manifest separately, because a manifest
is a fixed cost that does not scale with the logic.

Token counts use the tiktoken o200k_base encoder, and again cl100k_base as an
independent check. Neither is Claude's tokenizer. Absolute counts move with the
tokenizer; the ranking does not. Use the ratio columns.
"""

import json
import sys
from pathlib import Path

import tiktoken

ROOT = Path(__file__).resolve().parent.parent
ENCODER = tiktoken.get_encoding("o200k_base")
OLD_ENCODER = tiktoken.get_encoding("cl100k_base")

FRAMEWORKS = {
    "python": "FastAPI",
    "typescript": "Express",
    "csharp": "ASP.NET Minimal API",
    "go": "net/http",
    "php": "Slim",
    "java": "Spring Boot",
    "rust": "Axum",
    "zig": "std.http",
    "c": "raw winsock",
    "cpp": "raw winsock",
}

# Line comment markers per language. A block-comment opener is handled separately.
LINE_MARKERS = {
    "python": ("#",),
    "typescript": ("//",),
    "csharp": ("//",),
    "go": ("//",),
    "php": ("//", "#", "*"),
    "java": ("//",),
    "rust": ("//",),
    "zig": ("//",),
    "c": ("//", "*"),
    "cpp": ("//", "*"),
}

# Application-source extensions, and the manifest files, per language. Source is
# discovered by walking the tier directory, so a multi-file large-tier layout is
# picked up without listing every file.
EXTENSIONS = {
    "python": (".py",),
    "typescript": (".ts",),
    "csharp": (".cs",),
    "go": (".go",),
    "php": (".php",),
    "java": (".java",),
    "rust": (".rs",),
    "zig": (".zig",),
    "c": (".c", ".h"),
    "cpp": (".cpp", ".hpp", ".h"),
}

MANIFESTS = {
    "python": ["requirements.txt"],
    "typescript": ["package.json", "tsconfig.json"],
    "csharp": ["csharp.csproj"],
    "go": ["go.mod"],
    "php": ["composer.json"],
    "java": ["pom.xml", "src/main/resources/application.properties"],
    "rust": ["Cargo.toml"],
    "zig": ["build.zig", "build.zig.zon"],
    "c": [],
    "cpp": [],
}

# Build output and dependency trees never count as application source.
SKIP_DIRS = {"node_modules", "dist", "bin", "obj", "target", "vendor", ".zig-cache",
             "zig-out", "build", "__pycache__", ".git"}
# A Zig build script is a manifest, not application source.
SKIP_FILES = {"build.zig"}

TIERS = [("small", "impl"), ("mid", "impl-mid"), ("large", "impl-large")]
ORDER = list(FRAMEWORKS)


def is_comment(line: str, markers: tuple[str, ...]) -> bool:
    stripped = line.strip()
    if stripped.startswith(("/*", '"""', "'''", "<!--")):
        return True
    return any(stripped.startswith(marker) for marker in markers)


def measure(paths: list[Path], markers: tuple[str, ...]) -> dict:
    text = ""
    found = 0
    for path in paths:
        if path.exists():
            text += path.read_text(encoding="utf-8")
            found += 1
    if text == "":
        return {"files": 0, "lines": 0, "codeLines": 0, "characters": 0, "bytes": 0,
                "tokens": 0, "tokensCl100k": 0}
    lines = text.splitlines()
    code = [line for line in lines if line.strip() != "" and not is_comment(line, markers)]
    return {
        "files": found,
        "lines": len(lines),
        "codeLines": len(code),
        "characters": len(text),
        "bytes": len(text.encode("utf-8")),
        "tokens": len(ENCODER.encode(text)),
        "tokensCl100k": len(OLD_ENCODER.encode(text)),
    }


def source_files(base: Path, language: str) -> list[Path]:
    """Walk the tier directory for application source, skipping build output."""
    found = []
    for path in sorted(base.rglob("*")):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.relative_to(base).parts):
            continue
        if path.name in SKIP_FILES or path.suffix not in EXTENSIONS[language]:
            continue
        found.append(path)
    return found


def collect(tier: str, folder: str) -> list[dict]:
    rows = []
    for language in ORDER:
        base = ROOT / folder / language
        if not base.exists():
            continue
        markers = LINE_MARKERS[language]
        source = measure(source_files(base, language), markers)
        if source["tokens"] == 0:
            continue
        rows.append({
            "tier": tier,
            "language": language,
            "framework": FRAMEWORKS[language],
            "source": source,
            "manifest": measure([base / name for name in MANIFESTS[language]], markers),
        })
    return rows


def print_tier(tier: str, rows: list[dict]) -> None:
    rows = sorted(rows, key=lambda row: row["source"]["tokens"])
    least = rows[0]["source"]["tokens"]
    least_old = min(row["source"]["tokensCl100k"] for row in rows)
    print()
    print(f"## {tier} tier — application source only ({len(rows)} languages)")
    print()
    print("| Rank | Language | Framework | Lines | Code lines | Characters | Tokens | Ratio "
          "| cl100k | cl100k ratio |")
    print("|---:|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for rank, row in enumerate(rows, start=1):
        s = row["source"]
        print(f"| {rank} | {row['language']} | {row['framework']} | {s['lines']} "
              f"| {s['codeLines']} | {s['characters']} | {s['tokens']} "
              f"| {s['tokens'] / least:.2f}x | {s['tokensCl100k']} "
              f"| {s['tokensCl100k'] / least_old:.2f}x |")


def print_growth(all_rows: list[dict]) -> None:
    tiers = [tier for tier, _ in TIERS if any(row["tier"] == tier for row in all_rows)]
    if len(tiers) < 2:
        return
    index = {(row["tier"], row["language"]): row["source"]["tokens"] for row in all_rows}
    print()
    print("## Growth across tiers (tokens, and growth against the small tier)")
    print()
    header = "| Language |" + "".join(f" {tier} | vs small |" for tier in tiers)
    print(header)
    print("|---|" + "---:|" * (2 * len(tiers)))
    for language in ORDER:
        base = index.get(("small", language))
        if base is None:
            continue
        cells = ""
        for tier in tiers:
            value = index.get((tier, language))
            if value is None:
                cells += " — | — |"
            else:
                cells += f" {value} | {value / base:.2f}x |"
        print(f"| {language} |{cells}")


wanted = sys.argv[1:] or [tier for tier, _ in TIERS]
all_rows: list[dict] = []
for tier, folder in TIERS:
    if tier not in wanted:
        continue
    rows = collect(tier, folder)
    if not rows:
        print(f"skip {tier}: no implementation found under {folder}/")
        continue
    all_rows.extend(rows)
    print_tier(tier, rows)

if all_rows:
    print_growth(all_rows)
    output = ROOT / "results.json"
    output.write_text(json.dumps(all_rows, indent=2), encoding="utf-8")
    print()
    print(f"wrote {output}")
