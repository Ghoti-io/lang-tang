// Starts the tang command as the debug adapter for a "tang" launch
// configuration: `tang --dap FILE` for a template (the default) and
// `tang --script --dap FILE` when the configuration says `"script": true`. The
// tang command must be on the PATH of VS Code.
//
// tests/unit/test_tang_dap.cpp reads the two DebugAdapterExecutable calls below
// and runs each command line against the real tang command. Keep each call in
// this shape: the command name as a string literal, then an array of string
// literals and the identifier `file`.
const vscode = require("vscode");

function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("tang", {
      createDebugAdapterDescriptor(session) {
        const file = session.configuration.program;
        if (session.configuration.script) {
          return new vscode.DebugAdapterExecutable("tang", ["--script", "--dap"]);
        }
        return new vscode.DebugAdapterExecutable("tang", ["--dap", file]);
      },
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
