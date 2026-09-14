// Starts swordls and lets VS Code paint with the semantic tokens it sends.
// The TextMate grammar next to this file colours the buffer until the server
// answers, and whenever it is not running.
const { workspace, window } = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');
const fs = require('fs');
const path = require('path');

let client;

function resolveServer(context) {
  const configured = workspace.getConfiguration('sword').get('serverPath');
  if (configured && configured !== 'swordls' && fs.existsSync(configured)) {
    return configured;
  }
  // Convenient when the extension sits inside the compiler's own tree.
  const sibling = path.resolve(context.extensionPath, '..', '..', 'swordls');
  if (fs.existsSync(sibling)) return sibling;
  return configured || 'swordls';
}

function activate(context) {
  const command = resolveServer(context);
  client = new LanguageClient(
    'swordls',
    'Sword Language Server',
    { run: { command }, debug: { command } },
    {
      documentSelector: [{ scheme: 'file', language: 'sword' }],
      synchronize: { fileEvents: workspace.createFileSystemWatcher('**/*.sword') },
    }
  );
  client.start().catch((err) => {
    window.showErrorMessage(`Sword: could not start ${command}: ${err.message}`);
  });
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
