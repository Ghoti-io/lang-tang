#!/bin/sh
#
# Check the VS Code debug contribution under <root> (editors/vscode in this
# repository): that its manifest is JSON and declares what VS Code needs to
# offer "tang" as a debugger, and that its extension starts the tang command in
# its DAP mode. The command line itself is driven against the real tang command
# by tests/unit/test_tang_dap.cpp; this is what can be said without it.
#
# It fails on an empty population, naming it, and each failure names the file
# and what is wrong. tools/check-gates.sh runs it against tests/gates/vscode, a
# control and one planted defect for each check.
#
# Usage: tools/check-vscode.sh <directory holding package.json and extension.js>

set -u

root="${1:-}"
if [ -z "$root" ] || [ ! -f "$root/package.json" ]; then
  printf 'check-vscode: no package.json under %s, so this checks nothing\n' "${root:-(no directory given)}" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  printf 'check-vscode: python3 was not found, and the manifest cannot be read without it\n' >&2
  exit 1
fi

python3 - "$root" <<'PY'
import json, os, re, sys

root = sys.argv[1]
failures = []

def fail(message):
    failures.append(message)

manifest_path = os.path.join(root, "package.json")
try:
    with open(manifest_path) as f:
        manifest = json.load(f)
except ValueError as e:
    print("check-vscode: %s is not valid JSON: %s" % (manifest_path, e), file=sys.stderr)
    sys.exit(1)

contributes = manifest.get("contributes", {})
debuggers = [d for d in contributes.get("debuggers", []) if d.get("type") == "tang"]
if not debuggers:
    fail("%s: contributes.debuggers has no debugger of type \"tang\"" % manifest_path)
else:
    d = debuggers[0]
    launch = d.get("configurationAttributes", {}).get("launch", {})
    if "program" not in launch.get("required", []) or "program" not in launch.get("properties", {}):
        fail("%s: the tang debugger's launch attributes do not require a \"program\"" % manifest_path)
    script = launch.get("properties", {}).get("script", {})
    if script.get("type") != "boolean" or script.get("default") is not False:
        fail("%s: the tang debugger's launch attributes have no boolean \"script\" that defaults to false (a template)" % manifest_path)
    if not d.get("initialConfigurations"):
        fail("%s: the tang debugger offers no initialConfigurations" % manifest_path)
if not any(b.get("language") == "tang" for b in contributes.get("breakpoints", [])):
    fail("%s: contributes.breakpoints does not allow breakpoints in a tang file, so none can be set" % manifest_path)

main = manifest.get("main")
extension = os.path.join(root, main) if main else None
if not main:
    fail("%s: has no \"main\"" % manifest_path)
elif not os.path.isfile(extension):
    fail("%s: \"main\" names %s, which does not exist" % (manifest_path, main))
else:
    with open(extension) as f:
        source = f.read()
    calls = re.findall(r'DebugAdapterExecutable\(\s*"([^"]+)"\s*,\s*\[([^\]]*)\]\s*\)', source)
    if not calls:
        fail("%s: has no DebugAdapterExecutable(\"command\", [arguments]) call" % extension)
    scripts = 0
    templates = 0
    for command, text in calls:
        args = [a.strip() for a in text.split(",") if a.strip()]
        if command != "tang":
            fail("%s: the adapter command is \"%s\", not \"tang\"" % (extension, command))
        if '"--dap"' not in args:
            fail("%s: the adapter's arguments do not include \"--dap\"" % extension)
        if "file" not in args:
            fail("%s: the adapter's arguments do not include the program (the identifier `file`)" % extension)
        if '"--script"' in args:
            scripts += 1
        else:
            templates += 1
    if calls and scripts != 1:
        fail("%s: expected one adapter call with \"--script\" (a script), found %d" % (extension, scripts))
    if calls and templates != 1:
        fail("%s: expected one adapter call without \"--script\" (a template, which the checkpoint steps), found %d" % (extension, templates))
    if 'registerDebugAdapterDescriptorFactory("tang"' not in source:
        fail("%s: does not register a descriptor factory for \"tang\"" % extension)

for message in failures:
    print("check-vscode: " + message, file=sys.stderr)
sys.exit(1 if failures else 0)
PY
