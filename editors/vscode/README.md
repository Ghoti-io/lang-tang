# Tang debugging in VS Code

A minimal VS Code extension that makes `tang --dap` the debug adapter for a
"tang" launch configuration. It contains no debugger of its own: the adapter is
the `tang` command (runtime-debug's Debug Adapter Protocol server, hosted by
lang-tang), and this extension only says how to start it.

## Try it

1. Install the `tang` command and put it on the `PATH` of the shell that starts
   VS Code (`make install PREFIX=...` in this repository, then
   `PATH="$PREFIX/bin:$PATH" code .`).
2. Install this extension. For one session, without packaging it, open this
   directory (`libs/lang-tang/editors/vscode`) in VS Code and press F5, which
   starts an Extension Development Host; open a folder holding a `.tang` file
   there. To keep it, link the directory into `~/.vscode/extensions/`.
3. Open a `.tang` file, set a breakpoint in the gutter, and choose
   Run and Debug, "Debug the current Tang template". The file runs as a template
   (`tang --dap FILE`); "Debug the current Tang script" runs it as a script
   (`tang --script --dap FILE`). Stepping in, over and out, the call stack and
   the variables are the adapter's.

The `launch.json` that "Debug the current Tang template" offers is below; add
`"script": true` to run the file as a script.

```json
{
  "type": "tang",
  "request": "launch",
  "name": "Debug the current Tang template",
  "program": "${file}",
  "script": false
}
```

## What is checked

Neither check starts VS Code; the step in VS Code itself is a manual check.

- `tests/unit/test_tang_dap.cpp` reads the two command lines out of
  `extension.js` (one for a template, one for a script) and drives `configure`,
  `stopped`, `disconnect` over each against the real `tang` command. A command
  line `tang` does not accept fails it. It does not check the manifest beyond
  naming the debugger and its `script` attribute.
- `make check-vscode` checks the manifest's structure (valid JSON, a `tang`
  debugger with a required `program`, a boolean `script` that defaults to false,
  initial configurations, breakpoints, an existing `main`) and the shape of the
  calls in `extension.js`. `check-gates` runs it against `tests/gates/vscode`: a
  control, and a planted defect for each check.
