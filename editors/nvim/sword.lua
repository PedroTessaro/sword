-- Drop this in your config and call require('sword').setup{}.
--
-- Semantic tokens are what make the colouring exact: the server says whether a
-- name is a type, a function or a parameter, and Neovim paints it with the
-- current colourscheme. The syntax file in editors/vim is the fallback for
-- before the server attaches.
local M = {}

local function find_server(explicit)
  if explicit then return explicit end
  local found = vim.fn.exepath('swordls')
  if found ~= '' then return found end
  -- Running straight out of the source tree.
  local here = debug.getinfo(1, 'S').source:sub(2)
  return vim.fn.fnamemodify(here, ':h:h:h') .. '/swordls'
end

function M.setup(opts)
  opts = opts or {}
  local cmd = find_server(opts.server)

  vim.filetype.add({ extension = { sword = 'sword' } })

  vim.api.nvim_create_autocmd('FileType', {
    pattern = 'sword',
    callback = function(args)
      if vim.fn.executable(cmd) == 0 then
        vim.notify('swordls not found at ' .. cmd, vim.log.levels.WARN)
        return
      end
      vim.lsp.start({
        name = 'swordls',
        cmd = { cmd },
        root_dir = vim.fs.dirname(args.file),
        on_attach = function(_, bufnr)
          -- Neovim sets omnifunc itself when the server offers completion, but
          -- only from 0.8 on and only if nothing else claimed it. Setting it
          -- here means <C-x><C-o> works either way.
          vim.bo[bufnr].omnifunc = 'v:lua.vim.lsp.omnifunc'
        end,
      }, { bufnr = args.buf })
    end,
  })
end

-- Neovim maps semantic token types onto @lsp.type.* groups. These links are
-- only a default: any colourscheme that defines them wins.
function M.link_defaults()
  local links = {
    ['@lsp.type.namespace']  = 'Include',
    ['@lsp.type.type']       = 'Type',
    ['@lsp.type.struct']     = 'Structure',
    ['@lsp.type.interface']  = 'Structure',
    ['@lsp.type.parameter']  = 'Identifier',
    ['@lsp.type.variable']   = 'Identifier',
    ['@lsp.type.property']   = 'Identifier',
    ['@lsp.type.function']   = 'Function',
    ['@lsp.type.method']     = 'Function',
    ['@lsp.type.keyword']    = 'Keyword',
    ['@lsp.type.string']     = 'String',
    ['@lsp.type.number']     = 'Number',
    ['@lsp.type.enum']       = 'Structure',
    ['@lsp.type.enumMember'] = 'Constant',
  }
  for group, target in pairs(links) do
    vim.api.nvim_set_hl(0, group, { link = target, default = true })
  end
end

return M
