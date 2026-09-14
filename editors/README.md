# Editor support

The server is `swordls`, built by `make` alongside the compiler. It speaks LSP
over stdin and stdout and gives you three things: **diagnostics**, the same errors
`shield` reports, underlined as you type; **semantic tokens** — the real
classification of every token, which the editor paints using your own colour
scheme; and **completion**, which knows the difference between a package, a
value's fields and the errors your program declares.

The server analyses the whole **package**, not the open file. A name declared in a
sibling file of the same directory is a name, not an error — and a file you have
not saved is read from the editor's buffer, so it is what you are typing that gets
checked.

The difference between semantic tokens and a regex grammar is that the server
knows what a regex cannot:

```sword
mut arena := mem.NewArena(backing[..])
//  ^^^^^ variable        ^^^^^^^ parameter
//           ^^^ namespace
//               ^^^^^^^^ function
```

Each editor below also gets a plain regex grammar, used before the server
attaches and whenever it is not running, and indentation that follows the
language — see [where a new line lands](#where-a-new-line-lands).

## Neovim

```lua
vim.opt.runtimepath:append('/path/to/sword/editors/vim')
local sword = dofile('/path/to/sword/editors/nvim/sword.lua')
sword.link_defaults()   -- optional: links @lsp.type.* to the standard groups
sword.setup{}           -- finds swordls on PATH or beside the repository
```

If you installed with `make install`, the paths are
`~/.local/share/sword/editors/vim` and `~/.local/share/sword/editors/nvim`.

The `editors/vim` directory is what indents: four spaces, a new line placed by
the brackets around it, and a comment that carries on when you press Enter
inside one. Neovim turns filetype indentation on by default.

Neovim's built-in LSP client already handles semantic tokens and completion, so
no plugin is needed. To check both are on, with a `.sword` file open:

```
:lua =vim.lsp.get_clients({ bufnr = 0 })[1].server_capabilities.semanticTokensProvider
:lua =vim.lsp.get_clients({ bufnr = 0 })[1].server_capabilities.completionProvider
:Inspect      " shows which group painted the token under the cursor
:set omnifunc?
```

`setup{}` sets `omnifunc` on attach, so `<C-x><C-o>` completes. For completion as
you type, point whichever completion plugin you use at the LSP source — the server
answers `textDocument/completion` and asks to be triggered on `.`.

## Vim

Vim has no built-in LSP client, so the regex colouring works on its own and
semantic tokens need one of the usual plugins.

```vim
set runtimepath+=/path/to/sword/editors/vim
filetype plugin indent on
```

The second line is what loads the indentation. Without it Enter copies the line
above at best, which is right until the first brace.

With [vim-lsp](https://github.com/prabirshrestha/vim-lsp):

```vim
let g:lsp_semantic_enabled = 1

au User lsp_setup call lsp#register_server({
    \ 'name': 'swordls',
    \ 'cmd': {server_info->['/path/to/swordls']},
    \ 'allowlist': ['sword'],
    \ })

" vim-lsp does not set omnifunc for you, which is why <C-x><C-o> does nothing
" until you say this.
autocmd FileType sword setlocal omnifunc=lsp#complete
```

vim-lsp links most semantic groups to standard highlight groups by itself, but
anything it does not recognise falls back to `Normal`, which looks worse than
no highlighting at all. `namespace` is the one Sword uses that it misses:

```vim
function! s:SwordSemanticColors() abort
  highlight default link LspSemanticNamespace           PreProc
  highlight default link LspSemanticDefaultLibraryType  Type
  highlight default link LspSemanticDeclarationFunction Function
  highlight default link LspSemanticDeclarationMethod   Function
  highlight default link LspSemanticReadonlyVariable    Identifier
endfunction

augroup sword_semantic
  autocmd!
  autocmd ColorScheme * call s:SwordSemanticColors()
augroup END
call s:SwordSemanticColors()
```

A colour scheme clears highlight groups, which is why that runs again on
`ColorScheme`.

With [coc.nvim](https://github.com/neoclide/coc.nvim), in `coc-settings.json`:

```json
{
  "languageserver": {
    "sword": {
      "command": "/path/to/swordls",
      "filetypes": ["sword"]
    }
  },
  "semanticTokens.filetypes": ["sword"]
}
```

## VS Code

Install **Sword** from the Extensions view. It is `tessaro.sword`, on the Visual
Studio Marketplace and on Open VSX, which is where Cursor and VSCodium look. It
needs `shield` and `swordls` installed, and [its own page](vscode/README.md) lists
what it does: besides what the server gives, running a file, running a package's
tests or a single one, and snippets.

To run it from this tree instead:

```sh
cd editors/vscode
npm install
npm run build
ln -s "$PWD" ~/.vscode/extensions/tessaro.sword-0.1.0
```

Reopen VS Code and open a `.sword` file. The extension uses `sword.serverPath`
and `sword.shieldPath` when they are set, then the `swordls` and `shield` two
directories above itself — which is where they are when it sits inside the
compiler's own tree — and then PATH.

To see how a token was classified, run `Developer: Inspect Editor Tokens and
Scopes` from the command palette. The *semantic token type* field is what the
server sent; *textmate scopes* is the fallback.

## Emacs

Emacs 29 or later.

```elisp
(add-to-list 'load-path "/path/to/sword/editors/emacs")
(require 'sword-mode)
```

If you installed with `make install`, the path is
`~/.local/share/sword/editors/emacs`.

`sword-mode` takes `*.sword` files, colours them, indents them, and offers the
functions, types, errors and constants of a file to `imenu`. It registers
`swordls` with Eglot and with lsp-mode as soon as either one is loaded, so what
is left is starting one of them.

With Eglot, which comes with Emacs:

```elisp
(add-hook 'sword-mode-hook #'eglot-ensure)
```

Eglot paints semantic tokens itself from version 1.20 on, which is the one Emacs
31 comes with; on 29 or 30, a newer Eglot from GNU ELPA does the same. An older
one brings diagnostics and completion, and the colouring stays with the mode's
grammar.

With [lsp-mode](https://github.com/emacs-lsp/lsp-mode):

```elisp
(setq lsp-semantic-tokens-enable t)
(add-hook 'sword-mode-hook #'lsp-deferred)
```

lsp-mode asks once which directory is the project. Any directory holding the
file is a good answer: the server works out the package from the file's own
directory, not from the project.

The server is `sword-server-command` — a name looked up on PATH, `swordls` by
default, or a full path — and failing that the `swordls` two directories above
the mode, which is where it sits inside the compiler's own tree.

To see how a name was painted, put the cursor on it and run `M-x describe-char`.
With the server attached the face is one of `eglot-semantic-parameter`,
`lsp-face-semhl-namespace` and the like; `font-lock-` faces are the fallback.

## Where a new line lands

Vim, Neovim and Emacs place a line the same way. It goes one level in from the
statement that opened the bracket it is inside, which puts the body of a
signature broken over two lines back at the indentation of the `func`. The labels
of a `switch` sit at the level of the `switch`. A bracket with something already
after it on its line is lined up with instead, and so is a condition carried
onto the next line after `if`, `for`, `return` or `switch`:

```sword
func (mut s *Server) handle(name string, limit u64,
                            verbose bool) !u64 {
    switch s.kind {
    case Shape.Round:
        s.n += 1
    default:
        s.n = 0
    }
    reply := Reply{Status: 200,
                   Body: s.body}
    mut room := mem.Alloc[u8](&s.arena, 64) orelse
        return error.OutOfMemory
    if s.open &&
       s.n < limit {
        return s.n
    }
    return 0
}
```

`=` in vim and `indent-region` in Emacs apply the same rule to lines that are
already written. A line inside `/* */` that starts with `*` goes under the star
of the `/*`; any other line in a comment is left where you put it.

VS Code indents by rules simpler than these: a line after an open bracket goes
one level in, and a closing bracket, a `case` and a `default` come back out. It
does not line up under a bracket or under a condition, so those lines are yours
to align.

## What the server classifies

| Type | Where it shows up |
|---|---|
| `namespace` | the `mem` in `mem.Alloc`, the `error` in `error.Name` |
| `type` | type annotations, with `defaultLibrary` on the builtins |
| `struct`, `interface` | the name in the declaration |
| `function`, `method` | declarations and calls |
| `parameter` | parameters, declared and used |
| `variable` | bindings, with `readonly` when they are not `mut` |
| `property` | struct fields and `.len` / `.ptr` |
| `enumMember` | the name after `error.` |
| `keyword`, `string`, `number` | straight from the lexer |

## What the server completes

Typing `.` asks; so does `<C-x><C-o>` in vim, or whatever your completion plugin
is bound to.

| After | You get |
|---|---|
| `mem.` | what that package exports — functions, types, constants |
| `error.` | every error your program declares, with the sentence it says |
| `conn.` | that value's fields and methods, and nothing unexported from another package |
| `b.items.` | `len` and `ptr`, walking the chain a field at a time |
| `Kind.` | the enum's members |
| nothing | keywords, the builtin types, this package's own names, the packages it imports, and the locals of the function you are in |

A buffer being typed into rarely parses — `conn.` is a field access with no field
yet — so completion analyses a patched copy with a placeholder where the cursor
is, and puts your text back afterwards. That is why it answers while the file is
still broken.

Punctuation and comments are left to the editor's own grammar — the server does
not send them, because a regex gets those right without help.
