# Editor support

The server is `swordls`, built by `make` alongside the compiler. It speaks LSP
over stdin and stdout and gives you two things: **diagnostics**, the same errors
`shield` reports, underlined as you type, and **semantic tokens** — the real
classification of every token, which the editor paints using your own colour
scheme.

The difference between semantic tokens and a regex grammar is that the server
knows what a regex cannot:

```sword
mut arena := mem.NewArena(backing[..])
//  ^^^^^ variable        ^^^^^^^ parameter
//           ^^^ namespace
//               ^^^^^^^^ function
```

Each editor below also gets a plain regex grammar, used before the server
attaches and whenever it is not running.

## Neovim

```lua
vim.opt.runtimepath:append('/path/to/sword/editors/vim')
local sword = dofile('/path/to/sword/editors/nvim/sword.lua')
sword.link_defaults()   -- optional: links @lsp.type.* to the standard groups
sword.setup{}           -- finds swordls on PATH or beside the repository
```

If you installed with `make install`, the paths are
`~/.local/share/sword/editors/vim` and `~/.local/share/sword/editors/nvim`.

Neovim's built-in LSP client already handles semantic tokens, so no plugin is
needed. To check it is on, with a `.sw` file open:

```
:lua =vim.lsp.get_clients({ bufnr = 0 })[1].server_capabilities.semanticTokensProvider
:Inspect      " shows which group painted the token under the cursor
```

## Vim

Vim has no built-in LSP client, so the regex colouring works on its own and
semantic tokens need one of the usual plugins.

```vim
set runtimepath+=/path/to/sword/editors/vim
```

One thing to know: vim's own filetype detection claims `*.sw` for Sway, and it
runs first. The `ftdetect` file here uses `setfiletype`, which does not
override an existing choice, so set it yourself:

```vim
autocmd BufRead,BufNewFile *.sw set filetype=sword
```

With [vim-lsp](https://github.com/prabirshrestha/vim-lsp):

```vim
let g:lsp_semantic_enabled = 1

au User lsp_setup call lsp#register_server({
    \ 'name': 'swordls',
    \ 'cmd': {server_info->['/path/to/swordls']},
    \ 'allowlist': ['sword'],
    \ })
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

```sh
cd editors/vscode
npm install
ln -s "$PWD" ~/.vscode/extensions/sword-0.1.0
```

Reopen VS Code and open a `.sw` file. The extension looks for `swordls` on
PATH, at `sword.serverPath`, or two directories above itself — which is where
it sits when the extension is inside the compiler's own tree.

To see how a token was classified, run `Developer: Inspect Editor Tokens and
Scopes` from the command palette. The *semantic token type* field is what the
server sent; *textmate scopes* is the fallback.

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

Punctuation and comments are left to the editor's own grammar — the server does
not send them, because a regex gets those right without help.
