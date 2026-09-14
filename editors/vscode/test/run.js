// Runs the extension inside a real VS Code, against this repository's own shield
// and swordls, in a copy of test/fixture so that nothing is written into the
// tree. The first run downloads VS Code into .vscode-test.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { runTests } = require('@vscode/test-electron');

async function main() {
  const extension = path.resolve(__dirname, '..');
  const repo = path.resolve(extension, '..', '..');
  const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'sword-vscode-'));
  fs.cpSync(path.join(__dirname, 'fixture'), workspace, { recursive: true });
  fs.mkdirSync(path.join(workspace, '.vscode'));
  fs.writeFileSync(
    path.join(workspace, '.vscode', 'settings.json'),
    JSON.stringify(
      {
        'sword.serverPath': path.join(repo, 'swordls'),
        'sword.shieldPath': path.join(repo, 'shield'),
        // Typing is checked for where lines land, and a closing bracket the
        // editor inserts by itself would be typed over instead.
        'editor.autoClosingBrackets': 'never',
      },
      null,
      2
    )
  );
  try {
    await runTests({
      extensionDevelopmentPath: extension,
      extensionTestsPath: path.join(__dirname, 'suite.js'),
      launchArgs: [workspace, '--disable-extensions', '--disable-workspace-trust'],
    });
  } finally {
    fs.rmSync(workspace, { recursive: true, force: true });
  }
}

main().catch((err) => {
  console.error(err.message || err);
  process.exit(1);
});
