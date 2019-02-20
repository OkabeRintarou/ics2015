#include "nemu.h"

/* We use the POSIX regex functions to process regular expressions.
 * Type 'man regex' for more information about POSIX regex functions.
 */
#include <sys/types.h>
#include <regex.h>
#include <assert.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef NDEBUG
#define ASSERT(cond, msg) do {\
  if (!(cond)) {\
    printf("error: %s:%d  %s\n", __FILE__, __LINE__, (msg));\
    exit(EXIT_FAILURE);\
  }\
} while (false)
#else
#define ASSERT(cond, msg)
#endif

enum {
  NOTYPE = 256,
  EQ,     // ==
  NEQ,    // !=
  AND,    // &&
  OR,     // ||
  NOT,    // !
  PLUS,   // +
  UPLUS,  // unary +
  MINUS,  // -
  UMINUS, // unary -
  MUL,    // *
  DIV,    // /
  DEREF,  // unary *, deference memory
  NUMBER,
  REG,
  LPAREN, // (
  RPAREN, // )
};

//static const char *token_type(int type) {
//  static char *types[] = {
//      "NOTYPE", "EQ", "NEQ", "AND", "OR", "NOT", "PLUS", "UPLUS",
//      "MINUS", "UMINUS", "MUL", "DIV", "DEREF", "NUMBER", "REG",
//  };
//  if (type < NOTYPE || type > REG) {
//    return "UNKNOW";
//  }
//
//  return types[type - NOTYPE];
//}

static struct rule {
  char *regex;
  int token_type;
} rules[] = {

    /*
    ** Pay attention to the precedence level of different rules.
    */

    {"(0x[0-9a-f]+|[0-9]+)",                 NUMBER},           // number
    {"&&",                                   AND},
    {"\\|\\|",                               OR},
    {" +",                                   NOTYPE},        // spaces
    {"\\+",                                  PLUS},          // plus
    {"-",                                    MINUS},            // minus
    {"\\*",                                  MUL},          // multiply
    {"/",                                    DIV},            // divide
    {"==",                                   EQ},            // equal
    {"!=",                                   NEQ},           // not equal
    {"!",                                    NOT},
    {"\\(",                                  LPAREN},
    {")",                                    RPAREN},
    {"\\$(eax|ecx|edx|ebx|esp|ebp|esi|edi)", REG},               // identifier
};

#define NR_REGEX (sizeof(rules) / sizeof(rules[0]) )

static regex_t re[NR_REGEX];

/* Rules are used for many times.
 * Therefore we compile them only once before any usage.
 */
void init_regex() {
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i++) {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED | REG_ICASE);
    if (ret != 0) {
      regerror(ret, &re[i], error_msg, 128);
      Assert(ret == 0, "regex compilation failed: %s\n%s", error_msg, rules[i].regex);
    }
  }
}

typedef struct token {
  int type;
  char str[32];
} Token;

Token tokens[32];
int nr_token;

static bool make_token(char *e) {
  int position = 0;
  int i;
  regmatch_t pmatch;

  nr_token = 0;

  while (e[position] != '\0') {
    /* Try all rules one by one. */
    for (i = 0; i < NR_REGEX; i++) {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0) {
        char *substr_start = e + position;
        int substr_len = pmatch.rm_eo;

        Log("match rules[%d] = \"%s\" at position %d with len %d: %.*s", i, rules[i].regex, position, substr_len,
            substr_len, substr_start);
        position += substr_len;

        if (nr_token >= 32) {
          printf("too much tokens.\n");
          return false;
        }

        if (rules[i].token_type != NOTYPE) {
          tokens[nr_token].type = rules[i].token_type;

          switch (rules[i].token_type) {
            case NUMBER:
            case REG:
              if (substr_len >= 32) {
                printf("too long token.\n");
                return false;
              }
              strncpy(tokens[nr_token].str, substr_start, substr_len);
              break;
            default:
              break;
          }
          nr_token++;
        }
        break;
      }
    }

    if (i == NR_REGEX) {
      printf("no match at position %d\n%s\n%*.s^\n", position, e, position, "");
      return false;
    }
  }

  return true;
}

/**
BNF:

 <expr> ::= <decimal-number>
      |     <hexadecimal-number>
      |     <reg_name>
      |     "(" <expr> ")"
      |     <or-expr>
      |     "+" <expr>
      |     "-" <expr>
      ;
 <or-expr> ::=  <and-expr>
             |  <and-expr> "||" <and-expr>
             ;
 <and-expr> ::= <test-expr>
            | <test-expr> "&&" <test-expr>
            ;

 <test-expr> ::=    <term>
             |      <term> "==" <term>
             |      <term> "!=" <term>
             ;
 <term> ::=     <factor>
        |       <factor> "*" <factor>
        |       <factor> "/" <factor>
        ;
 <factor> ::=   <primary>
            |   "!" <factor>
            |   "*" <factor>
            ;
 <primary>  ::=  <decimal-number>
             |   <hexadecimal-number>
             |   <reg_name>
             ;
 */

enum {
  EXPR_TYPE_NUMBER,
  EXPR_TYPE_REG,
  EXPR_TYPE_PAREN_EXPR,
  EXPR_TYPE_OR_EXPR,
  EXPR_TYPE_UPLUS_EXPR,
  EXPR_TYPE_UMINUS_EXPR,
};

struct expr_t;

struct expr_t {
  int expr_type;
  union {
    struct expr_t *expr;
    uint32_t value;
  };
};

static struct expr_t *new_expr(int type, struct expr_t *ex, uint32_t value) {
  struct expr_t *e = malloc(sizeof(struct expr_t));
  e->expr_type = type;

  if (type <= EXPR_TYPE_REG) {
    e->value = value;
  } else {
    e->expr = ex;
  }
  return e;
}

struct primary_t {
  uint32_t value;
};

static struct primary_t *new_primary(uint32_t v) {
  struct primary_t *p = malloc(sizeof(struct primary_t));
  p->value = v;
  return p;
}

struct factor_t {
  int op;
  struct expr_t *e;
};

static struct factor_t *new_factor(int op, struct expr_t *e) {
  struct factor_t *f = malloc(sizeof(struct factor_t));
  f->op = op;
  f->e = e;
  return f;
}

struct term_t {
  int op;
  struct factor_t *left;
  struct factor_t *right;
};

static struct term_t *new_term(int op, struct factor_t *lhs, struct factor_t *rhs) {
  struct term_t *t = malloc(sizeof(struct term_t));
  t->op = op;
  t->left = lhs;
  t->right = rhs;
  return t;
}

struct test_expr_t {
  int op;
  struct term_t *left;
  struct term_t *right;
};

struct test_expr_t *new_test_expr(int op, struct term_t *lhs, struct term_t *rhs) {
  struct test_expr_t *t = malloc(sizeof(struct test_expr_t));
  t->op = op;
  t->left = lhs;
  t->right = rhs;
  return t;
}

struct and_expr_t {
  struct test_expr_t *left;
  struct test_expr_t *right;
};

static struct and_expr_t *new_and_expr(struct test_expr_t *lhs, struct test_expr_t *rhs) {
  struct and_expr_t *a = malloc(sizeof(struct and_expr_t));
  a->left = lhs;
  a->right = rhs;
  return a;
}

struct or_expr_t {
  struct and_expr_t *left;
  struct and_expr_t *right;
};

static struct or_expr_t *new_or_expr(struct and_expr_t *lhs, struct and_expr_t *rhs) {
  struct or_expr_t *o = malloc(sizeof(struct or_expr_t));
  o->left = lhs;
  o->right = rhs;
  return o;
}

static uint32_t get_reg_value(const char *reg) {

  int i;
  for (i = R_EAX; i <= R_EDI; i++) {
    if (strcasecmp(reg, regsl[i]) == 0) {
      return cpu.gpr[i]._32;
    }
  }
  return 0;
}

static struct primary_t *parse_primary(int *index) {
  int i = *index;
  struct primary_t *p = NULL;
  if (i < nr_token) {
    if (tokens[i].type == EXPR_TYPE_NUMBER) {
      p = new_primary(atoi(tokens[i].str));
      *index = i + 1;
    } else if (tokens[i].type == EXPR_TYPE_REG) {
      p = new_primary(get_reg_value(tokens[i].str));
    }
  }
  return p;
}

static struct factor_t *parse_factor(int *index) {
  int i = *index;
  struct factor_t *f = NULL;
  struct primary_t *p;

  if (i < nr_token) {
    switch (tokens[i].type) {
      case NOT:
      case MUL:
        i++;
        f = parse_factor(&i);
        f = new_factor(tokens[i].type, (struct expr_t*)((void*)f));
        *index = i + 1;
        break;
      default:
        p = parse_primary(&i);
        f = new_factor(0, (struct expr_t*)((void*)p));
        *index = i + 1;
        break;
    }
  }

  return f;
}

static struct term_t *parse_term(int *index) {
  struct term_t *r = NULL;
  struct factor_t *lhs, *rhs;
  int i;

  lhs = parse_factor(index);
  if (!lhs) {
    return NULL;
  }

  i = *index;
  if (i < nr_token) {
    switch (tokens[i].type) {
      case MUL:
      case DIV:
        i++;
        rhs = parse_factor(&i);
        if (rhs) {
          r = new_term(tokens[i].type, lhs, rhs);
        }
        *index = i + 1;
        break;
      default:
        r = new_term(0, lhs, NULL);
        *index = i + 1;
        break;
    }

  } else {
    r = new_term(0, lhs, NULL);
    *index = i + 1;
  }
  return r;
}

static struct test_expr_t *parse_test_expr(int *index) {
  struct test_expr_t *r = NULL;
  struct term_t *lhs, *rhs;
  int i;

  lhs = parse_term(index);
  if (!lhs) {
    return NULL;
  }

  i = *index;


  if (i < nr_token) {
    switch (tokens[i].type) {
      case EQ:
      case NEQ:
        i++;
        rhs = parse_term(&i);
        if (rhs) {
          r = new_test_expr(tokens[i].type, lhs, rhs);
        }
        *index = i + 1;
        break;
      default:
        r = new_test_expr(0, lhs, NULL);
        *index = i + 1;
        break;
    }

  } else {
    r = new_test_expr(0, lhs, NULL);
    *index = i + 1;
  }
  return r;
}

static struct and_expr_t *parse_and_expr(int *index) {
  struct and_expr_t *r = NULL;
  struct test_expr_t *lhs, *rhs;
  int i;

  lhs = parse_test_expr(index);
  if (!lhs) {
    return NULL;
  }

  i = *index;
  if (i < nr_token && tokens[i].type == AND) {
    i++;
    rhs = parse_test_expr(&i);
    if (!rhs) {
      free(lhs);
    } else {
      r = new_and_expr(lhs, rhs);
    }
  } else {
    r = new_and_expr(lhs, NULL);
  }
  *index = i + 1;
  return r;
}

static struct or_expr_t *parse_or_expr(int *index) {
  struct or_expr_t *r = NULL;
  struct and_expr_t *lhs, *rhs;
  int i;

  lhs = parse_and_expr(index);
  if (!lhs) {
    return NULL;
  }

  i = *index;
  if (i < nr_token && tokens[i].type == OR) {
    i++;
    rhs = parse_and_expr(&i);
    if (!rhs) {
      free(lhs);
    } else {
      r = new_or_expr(lhs, rhs);
    }
  } else {
    r = new_or_expr(lhs, NULL);
  }
  *index = i + 1;
  return r;
}

static struct expr_t *parse(int *index) {
  int i, type;
  // uint32_t value;
  struct expr_t *r = NULL, *other;

  i = *index;
  if (i >= nr_token) {
    return NULL;
  }
  type = tokens[i].type;
  if (type == NUMBER) {
    r = new_expr(EXPR_TYPE_NUMBER, NULL, (uint32_t)(atoi(tokens[i].str)));
    *index = i + 1;
  } else if (type == REG) {
    r = new_expr(EXPR_TYPE_REG, NULL, get_reg_value(tokens[i].str));
    *index = i + 1;
  } else if (type == UPLUS || type == UMINUS) {
    i++;
    other = parse(&i);
    if (other) {
      if (type == UPLUS) {
        r = new_expr(EXPR_TYPE_UPLUS_EXPR, other, 0);
      } else {
        r = new_expr(EXPR_TYPE_UMINUS_EXPR, other, 0);
      }
    }
    *index = i + 1;
  } else if (type == LPAREN) {
    i++;
    r = parse(&i);
    if (r && i < nr_token && tokens[i].type == RPAREN) {
      i++;
    } else {
      r = NULL;
    }
    *index = i + 1;
  } else {
    struct or_expr_t *or_expr = parse_or_expr(&i);
    r = (struct expr_t *) ((void *) or_expr);
    *index = i + 1;
  }
  return r;
}

uint32_t eval_primary(const struct primary_t *p) {
  return p->value;
}

uint32_t eval_factor(const struct factor_t *f) {
  switch (f->op) {
    case '!':
      if (eval_factor((struct factor_t *) ((void *) f->e))) {
        return 0u;
      } else {
        return 1u;
      }
    case '*': {
      swaddr_t address = (swaddr_t) eval_factor((struct factor_t *) ((void *) f->e));
      return swaddr_read(address, sizeof(uint32_t));
    }
    default:
      return eval_primary((struct primary_t *) ((void *) f->e));
  }
}

uint32_t eval_term(const struct term_t *t, bool *success) {

  uint32_t lhs, rhs;

  *success = true;
  lhs = eval_factor(t->left);

  switch (t->op) {
    case '*':
    case '/': {
      rhs = eval_factor(t->right);
      if (t->op == '*') {
        return lhs * rhs;
      } else {
        if (rhs == 0) {
          fprintf(stderr, "the divisor cannot be zero");
          *success = false;
        } else {
          return lhs / rhs;
        }
      }
      break;
    }
    default:
      return lhs;
  }
  return 0;
}

uint32_t eval_test_expr(const struct test_expr_t *t, bool *success) {
  uint32_t lhs, rhs;

  *success = true;
  lhs = eval_term(t->left, success);
  if (!*success) {
    return 0;
  }

  switch (t->op) {
    case '=':
    case '!': {
      rhs = eval_term(t->right, success);
      if (!*success) {
        return 0;
      }
      if (t->op == '=') {
        return lhs == rhs;
      } else {
        return lhs != rhs;
      }
    }
    default:
      return lhs;
  }
}

uint32_t eval_and_expr(const struct and_expr_t *e, bool *success) {

  uint32_t lhs, rhs;
  *success = true;

  lhs = eval_test_expr(e->left, success);
  if (!*success) {
    return 0;
  }
  if (e->right) {
    rhs = eval_test_expr(e->right, success);
    if (!*success) {
      return 0;
    }
    return lhs && rhs;
  } else {
    return lhs;
  }
}

uint32_t eval_or_expr(const struct or_expr_t *e, bool *success) {
  uint32_t lhs, rhs;
  *success = true;

  lhs = eval_and_expr(e->left, success);
  if (!*success) {
    return 0;
  }
  if (e->right) {
    rhs = eval_and_expr(e->right, success);
    if (!*success) {
      return 0;
    }
    return lhs || rhs;
  } else {
    return lhs;
  }
}

uint32_t eval(const struct expr_t *e, bool *success) {
  assert(e && success);
  *success = false;

  switch (e->expr_type) {
    case EXPR_TYPE_NUMBER:
    case EXPR_TYPE_REG: {
      struct primary_t *p = (struct primary_t *) ((void*)e);
      *success = true;
      return p->value;
    }
    case EXPR_TYPE_PAREN_EXPR:
      return eval(e->expr, success);
    case EXPR_TYPE_OR_EXPR:
      return eval_or_expr((struct or_expr_t *) ((void *) e), success);
    case EXPR_TYPE_UPLUS_EXPR:
      return eval(e->expr, success);
    case EXPR_TYPE_UMINUS_EXPR:
      return -eval(e->expr, success);
    default:
      ASSERT(false, "never each here");
  }
  return 0;
}

uint32_t expr(char *e, bool *success) {
  if (!make_token(e)) {
    *success = false;
    return 0;
  }

  int i;
  for (i = 0; i < nr_token; i++) {
    if ((tokens[i].type == MUL || tokens[i].type == MINUS || tokens[i].type == PLUS) &&
        (i == 0 ||
         (tokens[i - 1].type != NUMBER && tokens[i - 1].type != REG))) {

      switch (tokens[i].type) {
        case MUL:
          tokens[i].type = DEREF;
          break;
        case MINUS:
          tokens[i].type = UMINUS;
          break;
        case PLUS:
          tokens[i].type = UPLUS;
          break;
        default:
          ASSERT(false, "never reach here");
      }
    }
  }

  int index = 0;
  struct expr_t *tree = parse(&index);
  if (!tree) {
    printf("invalid expression");
    return 0;
  }
  uint32_t value = eval(tree, success);
  if (!*success) {
    printf("invalid expression");
    return 0;
  }

  *success = true;
  return value;
}

