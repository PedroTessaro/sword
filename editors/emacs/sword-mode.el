;;; sword-mode.el --- Major mode for the Sword language -*- lexical-binding: t; -*-

;; Version: 0.1.0
;; Package-Requires: ((emacs "29.1"))
;; Keywords: languages

;;; Commentary:

;; Editing support for Sword: colouring, indentation, `imenu', and the language
;; server wired to whichever LSP client is installed.
;;
;; The colouring here is a fallback, the same one the vim syntax file is: it
;; knows the grammar and nothing else.  With `swordls' attached the editor gets
;; the real classification of every name — a type, a parameter, a field — and
;; paints it with the theme in use.  Eglot does that by itself from 1.20 on, the
;; version Emacs 31 comes with, and before that brings diagnostics and completion
;; only; lsp-mode does it once `lsp-semantic-tokens-enable' is set.
;;
;;     (add-to-list 'load-path "/path/to/sword/editors/emacs")
;;     (require 'sword-mode)
;;     (add-hook 'sword-mode-hook #'eglot-ensure)

;;; Code:

(require 'imenu)

(defgroup sword nil
  "Support for the Sword language."
  :group 'languages
  :prefix "sword-")

(defcustom sword-indent-offset 4
  "Columns per level of indentation."
  :type 'integer
  :safe #'integerp)

(defcustom sword-server-command "swordls"
  "The language server, by name on PATH or as a full path."
  :type 'string)


;;; What the language is made of

(defconst sword--keywords
  '("package" "import" "func" "return" "if" "else" "for" "in" "break" "continue"
    "struct" "interface" "enum" "extern" "defer" "errdefer" "scope" "spawn"
    "parallel" "reduce" "lock" "const" "switch" "case" "select" "default"
    "try" "catch" "orelse" "mut"))

(defconst sword--types
  '("void" "bool" "string" "error" "int" "uint" "any" "atomic" "shared"
    "i8" "i16" "i32" "i64" "u8" "u16" "u32" "u64" "f32" "f64"))

(defconst sword--constants '("true" "false" "nil"))

(defconst sword--name "[A-Za-z_][A-Za-z0-9_]*")

(defvar sword-font-lock-keywords
  `((,(regexp-opt sword--keywords 'symbols) . font-lock-keyword-face)
    (,(regexp-opt sword--types 'symbols) . font-lock-type-face)
    (,(regexp-opt sword--constants 'symbols) . font-lock-constant-face)
    ;; The name a declaration introduces, receiver and all: `func (mut s *S) Do`.
    (,(concat "\\_<func\\_>\\s-*\\(?:(" sword--name "[^)]*)\\s-*\\)?\\(" sword--name "\\)")
     1 font-lock-function-name-face)
    (,(concat "\\_<\\(?:struct\\|interface\\|enum\\)\\_>\\s-+\\(" sword--name "\\)")
     1 font-lock-type-face)
    (,(concat "\\_<error\\_>\\s-+\\(" sword--name "\\)") 1 font-lock-constant-face)
    (,(concat "\\_<error\\.\\(" sword--name "\\)") 1 font-lock-constant-face)
    (,(concat "\\(" sword--name "\\)\\s-*(") 1 font-lock-function-name-face)
    ("\\_<0[xX][0-9A-Fa-f_]+\\_>" . font-lock-constant-face)
    ("\\_<0[bB][01_]+\\_>" . font-lock-constant-face)
    ("\\_<0[oO][0-7_]+\\_>" . font-lock-constant-face)
    ("\\_<[0-9][0-9_]*\\(?:\\.[0-9_]+\\)?\\(?:[eE][-+]?[0-9]+\\)?\\_>"
     . font-lock-constant-face))
  "Colouring for when the language server is not attached.")

(defvar sword-mode-syntax-table
  (let ((table (make-syntax-table)))
    (modify-syntax-entry ?_ "_" table)
    (modify-syntax-entry ?\\ "\\" table)
    (modify-syntax-entry ?\" "\"" table)
    ;; // to the end of the line, /* */ around anything, and those nest — `n` is
    ;; the flag that says so, and the lexer counts depth the same way.
    (modify-syntax-entry ?/ ". 124b" table)
    (modify-syntax-entry ?* ". 23n" table)
    (modify-syntax-entry ?\n "> b" table)
    table)
  "Syntax table for `sword-mode'.")

(defvar sword-imenu-generic-expression
  `(("func" ,(concat "^func\\s-+\\(?:(" sword--name "[^)]*)\\s-*\\)?\\(" sword--name "\\)") 1)
    ("type" ,(concat "^\\(?:struct\\|interface\\|enum\\)\\s-+\\(" sword--name "\\)") 1)
    ("error" ,(concat "^error\\s-+\\(" sword--name "\\)") 1)
    ("const" ,(concat "^const\\s-+\\(" sword--name "\\)") 1))
  "What `imenu' offers to jump to.")


;;; Reading a line

;; A statement ends when its last token could be the last of one: a name, a
;; literal, a closing bracket, `return', `break' or `continue'.  That is the
;; lexer's rule for inserting a semicolon, and here it is what says whether the
;; next line carries this one on.  The keywords below end in a letter and would
;; pass for names otherwise, which is the whole reason for the list.
(defconst sword--hanging
  '("package" "import" "func" "if" "else" "for" "in" "struct" "interface" "enum"
    "extern" "defer" "errdefer" "scope" "spawn" "parallel" "reduce" "lock"
    "const" "switch" "case" "default" "try" "catch" "orelse" "mut"))

(defconst sword--label-re "^[ \t]*\\(?:case\\_>.*\\|default[ \t]*\\):[ \t]*$")

;; A statement broken over lines is lined up under what it is about, which for
;; these is the first thing after the keyword.
(defconst sword--aligned-re
  "^[ \t]*\\(?:}[ \t]*else[ \t]+\\)?\\(?:if\\|for\\|return\\|switch\\)[ \t]+")

(defun sword--code-end ()
  "Position after the last code character of this line, or nil if it has none.
A comment is not code, whether it runs to the end of the line, was closed
on it, or began several lines above."
  (save-excursion
    (let ((bol (line-beginning-position))
          (state nil))
      (end-of-line)
      (when (nth 4 (syntax-ppss))
        (goto-char (nth 8 (syntax-ppss))))
      (skip-chars-backward " \t")
      (while (and (> (point) bol)
                  (nth 4 (setq state (save-excursion (syntax-ppss (1- (point)))))))
        (goto-char (nth 8 state))
        (skip-chars-backward " \t"))
      (and (> (point) bol) (point)))))

(defun sword--last-code-char ()
  "The last code character of this line, or nil."
  (let ((end (sword--code-end)))
    (and end (char-before end))))

(defun sword--label-line-p ()
  "Whether this line is a `case' or `default' label."
  (let ((end (sword--code-end)))
    (and end
         (string-match-p sword--label-re
                         (buffer-substring-no-properties (line-beginning-position) end)))))

(defun sword--ends-statement-p ()
  "Whether the line at point finishes a statement."
  (let ((char (sword--last-code-char)))
    (cond
     ((null char) nil)
     ((sword--label-line-p) t)
     ;; A quote here is the one that closed a string, which is a value.
     ((memq char '(?\) ?\] ?} ?\")) t)
     ((string-match-p "[A-Za-z0-9_]" (string char))
      (not (member (save-excursion
                     (goto-char (sword--code-end))
                     (buffer-substring-no-properties (progn (skip-syntax-backward "w_") (point))
                                                     (sword--code-end)))
                   sword--hanging)))
     (t nil))))

(defun sword--opens-bracket-p ()
  "Whether this line's last code character opens a bracket."
  (memq (sword--last-code-char) '(?{ ?\( ?\[)))

(defun sword--previous-code-line ()
  "Beginning of the nearest non-blank line above this one, or nil."
  (save-excursion
    (beginning-of-line)
    (skip-chars-backward " \t\n")
    (and (not (bobp)) (line-beginning-position))))

(defun sword--statement-start (pos)
  "Beginning of the line where the statement holding POS began.
A signature broken over three lines opens its body at the indent of the
`func', not of the fragment that happens to carry the brace."
  (save-excursion
    (goto-char pos)
    (beginning-of-line)
    (let (above)
      (while (and (setq above (sword--previous-code-line))
                  (save-excursion
                    (goto-char above)
                    (and (sword--code-end)
                         (not (sword--ends-statement-p))
                         (not (sword--opens-bracket-p)))))
        (goto-char above))
      (point))))


;;; Indentation

(defun sword--column-at (pos)
  (save-excursion (goto-char pos) (current-column)))

(defun sword--statement-indent (pos)
  (save-excursion
    (goto-char (sword--statement-start pos))
    (current-indentation)))

(defun sword--column-after (open)
  "Column of the first code character after the bracket at OPEN, or nil."
  (save-excursion
    (goto-char (1+ open))
    (skip-chars-forward " \t")
    (and (not (eolp))
         (not (looking-at-p "//\\|/\\*"))
         (current-column))))

(defun sword--continuation-indent (above)
  "Where a line carrying on the statement at ABOVE belongs."
  (let ((start (sword--statement-start above)))
    (save-excursion
      (goto-char start)
      (if (looking-at sword--aligned-re)
          (progn (goto-char (match-end 0)) (current-column))
        (+ (current-indentation) sword-indent-offset)))))

(defun sword--calculate-indent ()
  "The column this line belongs at, or nil to leave it alone."
  (save-excursion
    (back-to-indentation)
    (catch 'sword--done
      (let* ((state (syntax-ppss))
             (open (nth 1 state))
             (base 0))
        ;; A string over several lines is the author's to lay out.
        (when (nth 3 state)
          (throw 'sword--done nil))
        ;; Inside /* */ the comment decides: a line carrying on the column of
        ;; stars goes under the one in `/*', a line that already has text stays
        ;; where the author put it, and a fresh one follows the line above.
        (when (nth 4 state)
          (throw 'sword--done
                 (cond
                  ((looking-at-p "\\*") (1+ (sword--column-at (nth 8 state))))
                  ((not (eolp)) nil)
                  (t (let ((above (sword--previous-code-line)))
                       (and above
                            (save-excursion
                              (goto-char above)
                              (current-indentation))))))))
        (unless (sword--previous-code-line)
          (throw 'sword--done 0))
        (when open
          (setq base (sword--statement-indent open))
          ;; A closing bracket, and the labels of a switch, belong to the line
          ;; that opened the block rather than to what is inside it.
          (when (or (looking-at-p "[]})]") (sword--label-line-p))
            (throw 'sword--done base))
          ;; Something already follows the bracket: go under it.  That is how a
          ;; struct literal too long for one line is written.
          (let ((after (sword--column-after open)))
            (when after (throw 'sword--done after))))
        ;; The line above did not finish what it was saying, and a comma does not
        ;; count: inside brackets that is the next item, not the rest of the last.
        (let ((above (sword--previous-code-line)))
          (when (and above
                     (save-excursion
                       (goto-char above)
                       (and (sword--code-end)
                            (not (sword--ends-statement-p))
                            (not (sword--opens-bracket-p))
                            (not (eq (sword--last-code-char) ?,)))))
            (throw 'sword--done (sword--continuation-indent above))))
        (if open (+ base sword-indent-offset) 0)))))

(defun sword-indent-line ()
  "Indent the current line of Sword."
  (interactive)
  (let ((column (sword--calculate-indent)))
    (when column
      (if (<= (current-column) (current-indentation))
          (indent-line-to column)
        (save-excursion (indent-line-to column))))))


;;; The language server

(defun sword-server-path ()
  "Where `swordls' is, preferring PATH and falling back on the source tree."
  (or (executable-find sword-server-command)
      (and (file-name-absolute-p sword-server-command)
           (file-executable-p sword-server-command)
           sword-server-command)
      ;; Running straight out of the compiler's own tree.
      (let ((sibling (expand-file-name
                      "../../swordls"
                      (file-name-directory (or load-file-name buffer-file-name "")))))
        (and (file-executable-p sibling) sibling))
      sword-server-command))

(defvar eglot-server-programs)

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               (cons 'sword-mode (lambda (&rest _) (list (sword-server-path))))))

(defvar lsp-language-id-configuration)
(declare-function lsp-register-client "lsp-mode")
(declare-function make-lsp-client "lsp-mode")
(declare-function lsp-stdio-connection "lsp-mode")

(with-eval-after-load 'lsp-mode
  (add-to-list 'lsp-language-id-configuration '(sword-mode . "sword"))
  (lsp-register-client
   (make-lsp-client
    :new-connection (lsp-stdio-connection #'sword-server-path)
    :activation-fn (lambda (filename &optional _) (string-match-p "\\.sword\\'" filename))
    :server-id 'swordls)))


;;; The mode

;;;###autoload
(define-derived-mode sword-mode prog-mode "Sword"
  "Major mode for the Sword language."
  :syntax-table sword-mode-syntax-table
  (setq-local font-lock-defaults '(sword-font-lock-keywords))
  (setq-local indent-line-function #'sword-indent-line)
  (setq-local indent-tabs-mode nil)
  (setq-local tab-width sword-indent-offset)
  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "\\(?://+\\|/\\*+\\)\\s *")
  (setq-local comment-multi-line t)
  (setq-local electric-indent-chars
              (append '(?} ?\) ?\] ?:) electric-indent-chars))
  (setq-local imenu-generic-expression sword-imenu-generic-expression)
  (imenu-add-to-menubar "Sword"))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.sword\\'" . sword-mode))

(provide 'sword-mode)

;;; sword-mode.el ends here
