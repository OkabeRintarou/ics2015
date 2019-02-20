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

 <expr> ::= <or-expr>
      ;
 <or-expr> ::=  <and-expr>
             |  <and-expr> "||" <and-expr>
             ;
 <and-expr> ::= <test-expr>
            | <test-expr> "&&" <test-expr>
            ;

 <test-expr> ::=    <comp-expr>
             |      <comp-expr> "==" <comp-expr>
             |      <comp-expr> "!=" <comp-expr>
             ;
 <comp-expr> ::=    <term>
             |      <term> "+" <term>
             |      <term> "-" <term>
             ;
 <term> ::=     <factor>
        |       <factor> "*" <factor>
        |       <factor> "/" <factor>
        ;
 <factor> ::=   <primary>
            ;
 <primary>  ::=  <decimal-number>
             |   <hexadecimal-number>
             |   <reg_name>
             |   "(" <expr> ")"
             |   "+" <expr>
             |   "-" <expr>
             |   "!" <expr>
             |   "*" <expr>
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

struct primary_t {
  uint32_t value;
  struct expr_t *expr;
};

static struct primary_t *new_primary(uint32_t v, struct expr_t *e) {
  struct primary_t *p = malloc(sizeof(struct primary_t));

  memset(p, 0, sizeof(*p));

  p->expr = NULL;
  if (e) {
    p->expr = e;
  } else {
    p->value = v;
  }
  return p;
}

struct factor_t {
  struct primary_t *primary;
};

static struct factor_t *new_factor(struct primary_t *p) {
  struct factor_t *f = malloc(sizeof(struct factor_t));
  f->primary = p;
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

struct comp_expr_t {
  int op;
  struct term_t *left;
  struct term_t *right;
};

static struct comp_expr_t *new_comp_expr(int op, struct term_t *lhs, struct term_t *rhs) {
  struct comp_expr_t *c = malloc(sizeof(struct comp_expr_t));
  c->op = op;
  c->left = lhs;
  c->right = rhs;
  return c;
}

struct test_expr_t {
  int op;
  struct comp_expr_t *left;
  struct comp_expr_t *right;
};

struct test_expr_t *new_test_expr(int op, struct comp_expr_t *lhs, struct comp_expr_t *rhs) {
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

struct expr_t {
  struct or_expr_t *or_expr;
};

static struct expr_t *new_expr(struct or_expr_t *e) {
  struct expr_t *or_expr = malloc(sizeof(struct expr_t));
  or_expr->or_expr = e;
  return or_expr;
}

static uint32_t get_reg_value(const char *reg) {

  int i;
  for (i = R_EAX; i <= R_EDI; i++) {
    if (strcasecmp(reg + 1, regsl[i]) == 0) {
      return cpu.gpr[i]._32;
    }
  }
  return 0;
}

static struct or_expr_t *parse_or_expr(int *index);

static struct expr_t *parse(int *index) {
  int i;

  i = *index;
  if (i >= nr_token) {
    return NULL;
  }

  struct or_expr_t *or_expr = parse_or_expr(&i);
  if (!or_expr) {
    return NULL;
  }
  *index = i;
  return new_expr(or_expr);
}

static struct primary_t *parse_primary(int *index) {
  int i = *index;
  struct primary_t *p = NULL;
  uint32_t value;

  if (i < nr_token) {
    switch (tokens[i].type) {
      case LPAREN:
      case UPLUS:
      case UMINUS:
      case NOT:
      case DEREF: {
        i++;
        struct expr_t *e = parse(&i);
        if (!e) {
          break;
        }
        if (tokens[i].type == LPAREN) {
          if (i < nr_token && tokens[i].type == RPAREN) {
            i++;
          }
        }
        *index = i;
        p = new_primary(0u, e);
        break;
      }
      case NUMBER:
        value = (uint32_t) (atoi(tokens[i].str));
        *index = i + 1;
        p = new_primary(value, NULL);
        break;
      case REG:
        value = get_reg_value(tokens[i].str);
        *index = i + 1;
        p = new_primary(value, NULL);
        break;
      default:
        Assert(false, "never reach here");
    }
  }
  return p;
}

static struct factor_t *parse_factor(int *index) {
  struct primary_t *p = parse_primary(index);
  if (!p) {
    return NULL;
  }
  return new_factor(p);
}

static struct term_t *parse_term(int *index) {
  struct term_t *r = NULL;
  struct factor_t *lhs, *rhs;
  int i, type;

  lhs = parse_factor(index);
  if (!lhs) {
    return NULL;
  }

  i = *index;
  if (i < nr_token) {
    type = tokens[i].type;
    switch (type) {
      case MUL:
      case DIV:
        i++;
        rhs = parse_factor(&i);
        if (rhs) {
          r = new_term(type, lhs, rhs);
          *index = i;
        } else {
          free(lhs);
        }
        break;
      default:
        r = new_term(0, lhs, NULL);
        *index = i;
        break;
    }

  } else {
    r = new_term(0, lhs, NULL);
    *index = i;
  }
  return r;
}

static struct comp_expr_t *parse_comp_expr(int *index) {
  struct comp_expr_t *r = NULL;
  struct term_t *lhs, *rhs;
  int i, type;

  lhs = parse_term(index);
  if (!lhs) {
    return NULL;
  }

  i = *index;

  if (i < nr_token) {
    type = tokens[i].type;
    switch (type) {
      case PLUS:
      case MINUS:
        i++;
        rhs = parse_term(&i);
        if (rhs) {
          r = new_comp_expr(type, lhs, rhs);
          *index = i;
        } else {
          free(lhs);
        }
        break;
      default:
        r = new_comp_expr(0, lhs, NULL);
        *index = i;
        break;
    }
  } else {
    r = new_comp_expr(0, lhs, NULL);
    *index = i;
  }
  return r;
}

static struct test_expr_t *parse_test_expr(int *index) {
  struct test_expr_t *r = NULL;
  struct comp_expr_t *lhs, *rhs;
  int i, type;

  lhs = parse_comp_expr(index);
  if (!lhs) {
    return NULL;
  }

  i = *index;


  if (i < nr_token) {
    type = tokens[i].type;
    switch (type) {
      case EQ:
      case NEQ:
        i++;
        rhs = parse_comp_expr(&i);
        if (rhs) {
          r = new_test_expr(type, lhs, rhs);
          *index = i;
        } else {
          free(lhs);
        }
        break;
      default:
        r = new_test_expr(0, lhs, NULL);
        *index = i;
        break;
    }

  } else {
    r = new_test_expr(0, lhs, NULL);
    *index = i;
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
  *index = i;
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
  *index = i;
  return r;
}

static uint32_t eval(const struct expr_t *e, bool *success);

static uint32_t eval_primary(const struct primary_t *p, bool *success) {
  if (p->expr) {
    return eval(p->expr, success);
  } else {
    *success = true;
    return p->value;
  }
}

static uint32_t eval_factor(const struct factor_t *f, bool *success) {
  return eval_primary(f->primary, success);
}

static uint32_t eval_term(const struct term_t *t, bool *success) {

  uint32_t lhs, rhs;

  lhs = eval_factor(t->left, success);
  if (!*success) {
    return 0u;
  }

  switch (t->op) {
    case MUL:
    case DIV: {
      rhs = eval_factor(t->right, success);
      if (!*success) {
        return 0u;
      }
      if (t->op == MUL) {
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

static uint32_t eval_comp_expr(const struct comp_expr_t *c, bool *success) {
  uint32_t lhs, rhs;

  lhs = eval_term(c->left, success);
  if (!*success) {
    return 0;
  }

  switch (c->op) {
    case PLUS:
    case MINUS: {
      rhs = eval_term(c->right, success);
      if (!*success) {
        return 0;
      }
      if (c->op == PLUS) {
        return lhs + rhs;
      } else {
        return lhs - rhs;
      }
    }
    default:
      return lhs;
  }
}

static uint32_t eval_test_expr(const struct test_expr_t *t, bool *success) {
  uint32_t lhs, rhs;

  lhs = eval_comp_expr(t->left, success);
  if (!*success) {
    return 0;
  }

  switch (t->op) {
    case EQ:
    case NEQ: {
      rhs = eval_comp_expr(t->right, success);
      if (!*success) {
        return 0;
      }
      if (t->op == EQ) {
        return (uint32_t) (lhs == rhs);
      } else {
        return (uint32_t) (lhs != rhs);
      }
    }
    default:
      return lhs;
  }
}

static uint32_t eval_and_expr(const struct and_expr_t *e, bool *success) {

  uint32_t lhs, rhs;

  lhs = eval_test_expr(e->left, success);
  if (!*success) {
    return 0;
  }
  if (e->right) {
    rhs = eval_test_expr(e->right, success);
    if (!*success) {
      return 0;
    }
    return (uint32_t) (lhs && rhs);
  } else {
    return lhs;
  }
}

static uint32_t eval_or_expr(const struct or_expr_t *e, bool *success) {
  uint32_t lhs, rhs;

  lhs = eval_and_expr(e->left, success);
  if (!*success) {
    return 0;
  }
  if (e->right) {
    rhs = eval_and_expr(e->right, success);
    if (!*success) {
      return 0;
    }
    return (uint32_t) (lhs || rhs);
  } else {
    return lhs;
  }
}

static uint32_t eval(const struct expr_t *e, bool *success) {
  assert(e && success);
  *success = false;

  uint32_t value = eval_or_expr(e->or_expr, success);
  return value;
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
          Assert(false, "never reach here");
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

