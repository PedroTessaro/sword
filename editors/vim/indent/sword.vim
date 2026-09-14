" Where a line of Sword lands.
"
" One sentence: a line sits one level in from the statement that opened the
" bracket it is inside, and a bracket with something already after it is lined
" up with instead. The rest of this file is that sentence, plus the cases where
" the bracket is not on the line the statement started on.

if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=SwordIndent(v:lnum)
setlocal indentkeys=0},0),0],!^F,o,O,<:>
setlocal autoindent nolisp nosmartindent

let b:undo_indent = "setlocal indentexpr< indentkeys< ai< lisp< si<"

if exists("*SwordIndent")
  finish
endif

" A bracket inside a string or a comment is not a bracket. Asking the syntax
" highlighter is the obvious way to tell; masking the line and looking at the
" same column gives the same answer in two thirds of the time.
let s:skip = 's:Masked()'

" A statement ends when its last token could be the last of one: a name, a
" literal, a closing bracket, `return`, `break` or `continue`. That is the
" lexer's rule for inserting a semicolon, and here it is what says whether the
" next line carries this one on. The keywords below end in a letter and would
" pass for names otherwise, which is the whole reason this list exists.
let s:ends = '[A-Za-z0-9_)\]}]$'
let s:hanging = '\<\%(package\|import\|func\|if\|else\|for\|in\|struct\|interface' .
      \ '\|enum\|extern\|defer\|errdefer\|scope\|spawn\|parallel\|reduce\|lock' .
      \ '\|const\|switch\|case\|default\|try\|catch\|orelse\|mut\)$'

" A statement broken over lines is lined up under what it is about, which for
" these is the first thing after the keyword.
let s:keyword = '^\s*\%(}\s*else\s\+\)\=\%(if\|for\|return\|switch\)\s\+'

" The line with everything that only looks like code masked out, at the width it
" had: a column in the answer has to mean the same column in the file. What this
" cannot see is a line in the middle of a /* */, which is what s:Code is for —
" this one only ever looks at the line it was given.
function! s:Bare(lnum) abort
  let text = getline(a:lnum)
  let text = substitute(text, '"\%(\\.\|[^"\\]\)*"', '\=repeat("x", strlen(submatch(0)))', 'g')
  let text = substitute(text, '/\*.\{-}\*/', '\=repeat(" ", strlen(submatch(0)))', 'g')
  let text = substitute(text, '/\*.*$', '\=repeat(" ", strlen(submatch(0)))', '')
  return substitute(text, '//.*$', '\=repeat(" ", strlen(submatch(0)))', '')
endfunction

" The `/*` this line begins inside of, or [0, 0]. Two hundred lines back is as
" far as a comment is believed to reach, which keeps this bounded. The syntax
" highlighter cannot be asked instead: it answers nothing for a line whose first
" column is a space, even in the middle of a comment.
function! s:CommentOpener(lnum) abort
  let floor = max([1, a:lnum - 200])
  let view = winsaveview()
  call cursor(a:lnum, 1)
  let open = searchpos('/\*', 'bnW', floor)
  let shut = searchpos('\*/', 'bnW', floor)
  call winrestview(view)
  if open[0] == 0 || shut[0] > open[0] || (shut[0] == open[0] && shut[1] > open[1])
    return [0, 0]
  endif
  return open
endfunction

" Inside a comment a line is not code, and the code around it carries on as if
" it were not there.
function! s:Code(lnum) abort
  return s:CommentOpener(a:lnum)[0] > 0 ? '' : s:Bare(a:lnum)
endfunction

" A label ends its line the way a statement does: what follows is the case's
" body, not the rest of the case.
let s:label = '^\s*\%(case\>.*\|default\s*\):$'

function! s:Ends(lnum) abort
  let text = substitute(s:Code(a:lnum), '\s*$', '', '')
  return (text =~# s:ends && text !~# s:hanging) || text =~# s:label
endfunction

function! s:Opens(lnum) abort
  return substitute(s:Code(a:lnum), '\s*$', '', '') =~# '[({[]$'
endfunction

" Whether the bracket under the cursor is one, or only looks like one.
function! s:Masked() abort
  let text = getline(".")
  if text !~# '["/]'
    return 0
  endif
  return s:Bare(line("."))[col(".") - 1] !=# text[col(".") - 1]
endfunction

" The innermost bracket still open above this line, as [line, column].
function! s:Opener(lnum) abort
  let view = winsaveview()
  call cursor(a:lnum, 1)
  let best = searchpairpos('{', '', '}', 'bnW', s:skip)
  " A `(` or `[` still open cannot be older than the block it sits in, and
  " looking further back than that is most of what this file costs.
  let floor = best[0] > 0 ? best[0] : 1
  for pair in [['(', ')'], ['\[', '\]']]
    let at = searchpairpos(pair[0], '', pair[1], 'bnW', s:skip, floor)
    if at[0] > best[0] || (at[0] == best[0] && at[1] > best[1])
      let best = at
    endif
  endfor
  call winrestview(view)
  return best
endfunction

" Where the statement holding this line began. A signature broken over three
" lines opens its body at the indent of the `func`, not of the fragment that
" happens to carry the brace.
function! s:StatementStart(lnum) abort
  let at = a:lnum
  while at > 1
    let above = prevnonblank(at - 1)
    if above == 0 || s:Code(above) =~# '^\s*$' || s:Ends(above) || s:Opens(above)
      break
    endif
    let at = above
  endwhile
  return at
endfunction

function! SwordIndent(lnum) abort
  let prev = prevnonblank(a:lnum - 1)
  if prev == 0
    return 0
  endif

  " Inside /* */ the comment decides. A line carrying on the column of stars goes
  " under the one in `/*`; anything else in there the author wrote deliberately.
  let at = s:CommentOpener(a:lnum)
  if at[0] > 0
    return getline(a:lnum) =~# '^\s*\*' ? virtcol([at[0], at[1] + 1]) - 1 : -1
  endif

  let line = s:Bare(a:lnum)
  let open = s:Opener(a:lnum)
  let base = 0

  if open[0] > 0
    let base = indent(s:StatementStart(open[0]))

    " A closing bracket, and the labels of a switch, belong to the line that
    " opened the block rather than to what is inside it.
    if line =~# '^\s*[)\]}]' || line =~# '^\s*\%(case\>\|default\s*:\)'
      return base
    endif

    " Something already follows the bracket: go under it. That is how a struct
    " literal too long for one line is written.
    let rest = strpart(s:Bare(open[0]), open[1])
    if rest =~# '\S'
      return virtcol([open[0], open[1] + match(rest, '\S') + 1]) - 1
    endif
  endif

  " The line above did not finish what it was saying, and a comma does not count:
  " inside brackets that is the next item, not the rest of the last one.
  if !s:Ends(prev) && !s:Opens(prev) && s:Code(prev) !~# ',\s*$' && s:Code(prev) !~# '^\s*$'
    let start = s:StatementStart(prev)
    let lead = matchend(getline(start), s:keyword)
    if lead > 0
      return virtcol([start, lead + 1]) - 1
    endif
    return indent(start) + shiftwidth()
  endif

  return open[0] > 0 ? base + shiftwidth() : 0
endfunction
