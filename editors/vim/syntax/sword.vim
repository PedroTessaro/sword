" Fallback colouring for when the language server is not running. It uses the
" standard highlight groups, so it follows whatever colourscheme is loaded.
if exists("b:current_syntax")
  finish
endif

syn keyword swordKeyword package import func return if else for in break continue
syn keyword swordKeyword struct interface enum extern defer errdefer scope spawn
syn keyword swordKeyword parallel reduce chan lock const
syn keyword swordKeyword switch case default
syn keyword swordKeyword try catch orelse
syn keyword swordStorage mut
syn keyword swordBoolean true false nil

syn keyword swordType void bool string error int uint any
syn keyword swordType atomic shared
syn keyword swordType i8 i16 i32 i64 u8 u16 u32 u64 f32 f64

syn match swordFunction "\<\h\w*\>\ze\s*("
syn match swordNumber "\<\d\(\d\|_\)*\>"
syn match swordNumber "\<0x[0-9A-Fa-f_]\+\>"
syn match swordNumber "\<0b[01_]\+\>"
syn match swordNumber "\<0o[0-7_]\+\>"
syn match swordNumber "\<\d[0-9_]*\.\d[0-9_]*\%([eE][-+]\?\d\+\)\?\>"
syn match swordError "\<error\.\h\w*\>"
syn match swordOperator "[-+*/%<>=!&|^~?]\|:=\|\.\."

syn region swordString start=+"+ skip=+\\.+ end=+"+ contains=swordEscape
syn match swordEscape "\\[nrt0\\\"']" contained

syn match swordComment "//.*$" contains=@Spell
syn region swordComment start="/\*" end="\*/" contains=swordComment,@Spell

hi def link swordKeyword  Keyword
hi def link swordStorage  StorageClass
hi def link swordBoolean  Boolean
hi def link swordType     Type
hi def link swordFunction Function
hi def link swordNumber   Number
hi def link swordString   String
hi def link swordEscape   SpecialChar
hi def link swordComment  Comment
hi def link swordOperator Operator
hi def link swordError    Constant

let b:current_syntax = "sword"
