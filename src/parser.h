#pragma once

#include "ast.h"
#include "diag.h"
#include "lexer.h"

// Appends the file's declarations to `unit`, which may already hold the
// declarations of other files in the same package.
bool parse(const std::vector<Token> &tokens, Ast &ast, Node *unit);
