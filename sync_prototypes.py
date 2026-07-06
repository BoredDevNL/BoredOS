#!/usr/bin/env python3

import subprocess
import re
import pathlib
import sys

root = pathlib.Path(".")

# Pobierz definicje funkcji z clang AST
cmd = [
    "clang",
    "-Xclang", "-ast-dump=json",
    "-fsyntax-only",
    "-I.",
    *[str(x) for x in root.rglob("*.c")]
]

print("Analizuję funkcje...")

result = subprocess.run(
    cmd,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    text=True
)

if result.returncode != 0:
    print("clang nie mógł przeanalizować projektu")
    sys.exit(1)


# Znajdź deklaracje funkcji
functions = {}

for line in result.stdout.splitlines():
    if '"kind": "FunctionDecl"' in line:
        pass


# Prostsza wersja:
# używamy clang -Xclang -ast-dump
dump = subprocess.run(
    [
        "clang",
        "-Xclang",
        "-ast-dump",
        "-fsyntax-only",
        "-I.",
        *[str(x) for x in root.rglob("*.c")]
    ],
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    text=True
).stdout


for m in re.finditer(
    r'FunctionDecl.*? ([a-zA-Z_][a-zA-Z0-9_]*) .*?\'(.+?)\'',
    dump
):
    name = m.group(1)
    proto = m.group(2)

    if "(" in proto:
        functions[name] = proto


print(f"Znaleziono {len(functions)} funkcji")


# Zamiana prototypów w .h
for h in root.rglob("*.h"):

    text = h.read_text()

    old = text

    for name, proto in functions.items():

        pattern = (
            r'([a-zA-Z_][\w\s\*]*\s+'
            + re.escape(name)
            + r'\s*\([^;{]*\);)'
        )

        text = re.sub(
            pattern,
            proto + ";",
            text
        )

    if text != old:
        print("Poprawiam:", h)
        h.write_text(text)

print("Gotowe")
