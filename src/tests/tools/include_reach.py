"""include_reach.py <src root> <header> [<header> ...]

Transitive include weight and reach for the decoupling campaign's counters (design section 8).
For each header: the number of headers in its transitive closure, and the number of game .cpp
files whose own closure contains it, i.e. the translation units a change to it rebuilds.
Includes are resolved relative to the including file first, then by basename against every
header under src/, which is how the flat include directories of the game target resolve them.
"""
import collections
import os
import re
import sys

ROOT = sys.argv[1]
WANT = sys.argv[2:]
DIRS = ["game", "motion", "shared", "proto", "mangosd", "realmd"]
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)

by_name = {}
all_files = []
for d in DIRS:
    for dirpath, _, names in os.walk(os.path.join(ROOT, d)):
        for name in names:
            if name.endswith((".h", ".hpp", ".cpp")):
                rel = os.path.relpath(os.path.join(dirpath, name), ROOT).replace("\\", "/")
                all_files.append(rel)
                if name.endswith((".h", ".hpp")):
                    by_name.setdefault(name, rel)
                    by_name.setdefault(rel, rel)

edges = collections.defaultdict(set)
for rel in all_files:
    try:
        text = open(os.path.join(ROOT, rel), encoding="utf-8", errors="ignore").read()
    except OSError:
        continue
    here = os.path.dirname(rel)
    for match in INCLUDE_RE.finditer(text):
        inc = match.group(1)
        local = os.path.normpath(os.path.join(here, inc)).replace("\\", "/")
        target = by_name.get(local) or by_name.get(inc) or by_name.get(os.path.basename(inc))
        if target and target != rel:
            edges[rel].add(target)

memo = {}


def closure(start):
    if start in memo:
        return memo[start]
    seen, stack = set(), [start]
    while stack:
        current = stack.pop()
        for nxt in edges.get(current, ()):
            if nxt not in seen:
                seen.add(nxt)
                stack.append(nxt)
    memo[start] = seen
    return seen


game_cpps = [f for f in all_files if f.endswith(".cpp") and f.startswith("game/")]


def resolve(name):
    return by_name.get(name) or by_name.get(os.path.basename(name)) or name


print(f"{'header':44s} {'closure':>7s} {'game cpps reaching it':>22s}")
for wanted in WANT:
    header = resolve(wanted)
    size = len(closure(header))
    reach = sum(1 for cpp in game_cpps if header in closure(cpp))
    print(f"{header:44s} {size:7d} {reach:22d}")
