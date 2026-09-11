#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SEMI, OPEN_PAREN, CLOSE_PAREN } TypeSeparator;

typedef enum {
  EXIT,
} TypeKeyword;

typedef enum {
  INT,
} TypeLiteral;

typedef struct {
  TypeKeyword type;
} TokenKeyword;

typedef struct {
  TypeLiteral type;
  char *value;
} TokenLiteral;

typedef struct {
  TypeSeparator type;
} TokenSeparator;

TokenLiteral *generate_number(char current, FILE *file) {
  TokenLiteral *token = (TokenLiteral *)malloc(sizeof(TokenLiteral));
  token->type = INT;
  char *value = (char *)malloc(sizeof(char) * 4);
  int value_index = 0;
  while (isdigit(current) && current != EOF) {
    value[value_index] = current;
    value_index++;

    current = fgetc(file);
  }
  token->value = value;
  return token;
}

TokenKeyword *generate_keyword(char current, FILE *file) {
  TokenKeyword *token = (TokenKeyword *)malloc(sizeof(TokenKeyword));
  char *keyword = (char *)malloc(sizeof(char) * 4);
  int keyword_index = 0;
  while (isalpha(current) && current != EOF) {
    keyword[keyword_index] = current;
    keyword_index++;

    current = fgetc(file);
  }
  if (strcmp(keyword, "exit") == 0) {
    token->type = EXIT;
    printf("TOKEN TYPE EXIT\n");
  }
  return token;
}

void lexer(FILE *file) {
  int length;
  fseek(file, 0, SEEK_END);
  length = ftell(file);
  fseek(file, 0, SEEK_SET);
  char current_char = fgetc(file);

  while (current_char != EOF) {
    if (current_char == ';') {
      printf("FOUND SEMICOLON\n");
    } else if (current_char == '(') {
      printf("FOUND OPEN PARENTHESES\n");
    } else if (current_char == ')') {
      printf("FOUND CLOSED PARENTHESES\n");
    } else if (isdigit(current_char)) {
      TokenLiteral *test_token = generate_number(current_char, file);
      printf("TEST TOKEN VALUE: %s\n", test_token->value);
      free(test_token);
    } else if (isalpha(current_char)) {
      TokenKeyword *test_keyword = generate_keyword(current_char, file);
      free(test_keyword);
    }

    current_char = fgetc(file);
  }
}

int main() {
  FILE *file;
  file = fopen("test.sw", "r");

  lexer(file);
  fclose(file);

  return 0;
}
