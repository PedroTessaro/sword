// What a person using the extension would notice, checked from inside VS Code.
const assert = require('assert');
const path = require('path');
const vscode = require('vscode');

const root = () => vscode.workspace.workspaceFolders[0].uri.fsPath;
const at = (...parts) => vscode.Uri.file(path.join(root(), ...parts));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function until(what, probe, ms = 30000) {
  const deadline = Date.now() + ms;
  for (;;) {
    const value = await probe();
    if (value) return value;
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await sleep(100);
  }
}

async function open(...parts) {
  const document = await vscode.workspace.openTextDocument(at(...parts));
  await vscode.window.showTextDocument(document);
  return document;
}

// A buffer that keys and commands go to. A terminal left over from a run can
// keep the focus, and then typing lands nowhere.
async function scratch(content) {
  vscode.window.terminals.forEach((terminal) => terminal.dispose());
  const document = await vscode.workspace.openTextDocument({ language: 'sword', content });
  await vscode.window.showTextDocument(document);
  await vscode.commands.executeCommand('workbench.action.focusActiveEditorGroup');
  return document;
}

async function exitOf(terminal, name) {
  const found = terminal ?? vscode.window.terminals.find((t) => t.name === name);
  const status = await until(`${name} to finish`, () => found.exitStatus, 120000);
  return status.code;
}

// VS Code applies none of its indentation rules to a line it has not tokenized
// yet: Enter copies the line above, and a `}` typed there stays where it lands.
// A person never types faster than the tokenizer; a test does, so it waits
// between keys the way a person would.
const TOKENIZED = 60;

// One key into the editor holding `document`. Something else in the window can
// take the focus between two keys, and a key typed then is lost.
async function key(document, ch) {
  await sleep(TOKENIZED);
  if (vscode.window.activeTextEditor?.document !== document) await vscode.window.showTextDocument(document);
  await vscode.commands.executeCommand('workbench.action.focusActiveEditorGroup');
  await vscode.commands.executeCommand('type', { text: ch });
}

// Types into a fresh Sword buffer one character at a time, the way the editor
// sees a person type, and hands back the text.
async function typed(text) {
  const document = await scratch('');
  for (const ch of text) await key(document, ch);
  const result = document.getText();
  await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
  return result;
}

const checks = {
  async 'a .sword file is Sword'() {
    const document = await open('run', 'hello.sword');
    assert.strictEqual(document.languageId, 'sword');
  },

  async 'swordls reports an undefined name'() {
    const document = await open('broken', 'broken.sword');
    const found = await until('diagnostics', () => {
      const list = vscode.languages.getDiagnostics(document.uri);
      return list.length ? list : null;
    });
    assert.match(found[0].message, /undefined identifier 'missing'/);
  },

  async 'completion after mem. offers what the package exports'() {
    const document = await open('complete', 'complete.sword');
    const line = document.getText().split('\n').findIndex((l) => l.includes('mem.NewSystem'));
    const column = document.lineAt(line).text.indexOf('mem.') + 4;
    await until('completion to offer Alloc', async () => {
      const list = await vscode.commands.executeCommand(
        'vscode.executeCompletionItemProvider',
        document.uri,
        new vscode.Position(line, column),
        '.'
      );
      return list && list.items.some((item) => (item.label.label ?? item.label) === 'Alloc');
    });
  },

  async 'semantic tokens arrive'() {
    const document = await open('complete', 'complete.sword');
    await until('semantic tokens', async () => {
      const tokens = await vscode.commands.executeCommand('vscode.provideDocumentSemanticTokens', document.uri);
      return tokens && tokens.data.length > 0;
    });
  },

  async 'each test has a run link'() {
    const document = await open('lib', 'lib_test.sword');
    const lenses = await vscode.commands.executeCommand('vscode.executeCodeLensProvider', document.uri);
    assert.deepStrictEqual(
      lenses.map((lens) => [lens.command.title, lens.command.arguments[1]]),
      [['run test', 'TestPasses'], ['run test', 'TestFails']]
    );
  },

  async 'run test runs that test alone'() {
    const file = at('lib', 'lib_test.sword');
    const passing = await vscode.commands.executeCommand('sword.runTest', file, 'TestPasses');
    assert.strictEqual(await exitOf(passing, 'Sword: test'), 0);
    const failing = await vscode.commands.executeCommand('sword.runTest', file, 'TestFails');
    assert.strictEqual(await exitOf(failing, 'Sword: test'), 1);
  },

  async 'run tests runs the whole package'() {
    const terminal = await vscode.commands.executeCommand('sword.runTests', at('lib', 'lib_test.sword'));
    assert.strictEqual(await exitOf(terminal, 'Sword: test'), 1);
  },

  async 'run file builds the program and runs it'() {
    const terminal = await vscode.commands.executeCommand('sword.runFile', at('run', 'hello.sword'));
    assert.strictEqual(await exitOf(terminal, 'Sword: run'), 42);
  },

  async 'Enter follows the scope, and a switch label comes back out'() {
    const text = await typed(
      'func main() int {\nswitch 1 {\ncase 1:\nif true {\nreturn 1\n}\ndefault:\nreturn 0\n}\n}'
    );
    assert.strictEqual(
      text,
      [
        'func main() int {',
        '    switch 1 {',
        '    case 1:',
        '        if true {',
        '            return 1',
        '        }',
        '    default:',
        '        return 0',
        '    }',
        '}',
      ].join('\n')
    );
  },

  async 'Reindent Lines puts a flattened switch back'() {
    const want = [
      'func main() int {',
      '    switch 1 {',
      '    case 1:',
      '        return 1',
      '    default:',
      '        return 0',
      '    }',
      '}',
    ].join('\n');
    const content = want.replace(/^ +/gm, '');
    const document = await scratch(content);
    await sleep(TOKENIZED * 20);
    await vscode.commands.executeCommand('editor.action.reindentlines');
    assert.strictEqual(document.getText(), want);
    await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
  },

  async 'Enter after a comment goes back to the block'() {
    const document = await scratch('func main() int {\n    /*\n     * note\n     */');
    await sleep(TOKENIZED * 20);
    const end = document.lineAt(3).range.end;
    vscode.window.activeTextEditor.selection = new vscode.Selection(end, end);
    for (const ch of '\nreturn 0') await key(document, ch);
    assert.strictEqual(document.getText(), 'func main() int {\n    /*\n     * note\n     */\n    return 0');
    await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
  },

  async 'a brace after a URL in a string still opens a block'() {
    const text = await typed('if HasPrefix(s, "http://") {\nx = 1');
    assert.strictEqual(text, 'if HasPrefix(s, "http://") {\n    x = 1');
  },

  async 'Enter in a block comment carries the stars'() {
    const text = await typed('/*\nfirst\nsecond');
    assert.strictEqual(text, '/*\n * first\n * second');
  },
};

exports.run = async function () {
  const failed = [];
  for (const [name, check] of Object.entries(checks)) {
    try {
      await check();
      console.log(`ok    ${name}`);
    } catch (err) {
      failed.push(name);
      console.log(`FAIL  ${name}\n      ${String(err.message).split('\n').join('\n      ')}`);
    }
  }
  console.log(`${Object.keys(checks).length - failed.length} passed, ${failed.length} failed`);
  if (failed.length) throw new Error(`${failed.length} extension checks failed`);
};
