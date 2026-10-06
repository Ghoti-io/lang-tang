// Starts `tang --script --dap FILE` as the debug adapter for a "tang" launch
// configuration. The tang command must be on the PATH of VS Code.
//
// tests/unit/test_tang_dap.cpp reads the DebugAdapterExecutable call below and
// runs that command line against the real tang command, so what is here and
// what the command accepts cannot drift apart unseen. Keep the call in this one
// shape: the command name as a string literal, then an array of string
// literals and the identifier `file`.
const vscode = require("vscode");

function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("tang", {
      createDebugAdapterDescriptor(session) {
        const file = session.configuration.program;
        return new vscode.DebugAdapterExecutable("tang", ["--script", file]);
      },
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
