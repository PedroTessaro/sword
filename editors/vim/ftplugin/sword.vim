" What the language decides for you: four spaces, never a tab, and a comment
" that a linebreak stays inside.
if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

setlocal expandtab
setlocal shiftwidth=4
setlocal softtabstop=4
setlocal tabstop=4

setlocal commentstring=//\ %s
setlocal comments=s1:/*,mb:*,ex:*/,://
" Wrapping prose in the middle of code is never what you meant; continuing the
" comment you are in is.
setlocal formatoptions-=t
setlocal formatoptions+=croql

let b:undo_ftplugin = "setlocal et< sw< sts< ts< cms< com< fo<"
