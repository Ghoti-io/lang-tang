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
   Run and Debug, "Debug the current Tang file". The program runs as a script
   (`tang --script --dap FILE`). Stepping in, over and out, the call stack and
   the variables are the adapter's.

The `launch.json` that "Debug the current Tang file" offers is:

```json
{
  "type": "tang",
  "request": "launch",
  "name": "Debug the current Tang file",
  "program": "${file}"
}
```

## What is checked

`make check-vscode` checks the manifest (`package.json`) and runs
`tests/gates/vscode` fixtures against the checker, so that the checker is seen to
fail. `tests/unit/test_tang_dap.cpp` reads the command line out of `extension.js`
and drives a `configure`, `stopped`, `disconnect` session over it with the real
`tang` command. Neither starts VS Code: the step in VS Code itself is a manual
check.
