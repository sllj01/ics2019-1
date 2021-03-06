#include "nemu.h"
#include "monitor/expr.h"

/* We use the POSIX regex functions to process regular expressions.
 * Type 'man regex' for more information about POSIX regex functions.
 */
#include <sys/types.h>
#include <regex.h>

enum {
  TK_NOTYPE = 256, TK_EQ, TK_NUMBER, TK_REGISTER, TK_MINUS, TK_DEREFERENCE,
  TK_NOTEQ, TK_AND, TK_OR

  /* TODO: Add more token types */

};

static struct rule {
  char *regex;
  int token_type;
} rules[] = {

  /* TODO: Add more rules.
   * Pay attention to the precedence level of different rules.
   */

  {" +", TK_NOTYPE},    // spaces
  {"\\+", '+'},         // plus
  {"\\-", '-'},
  {"\\*", '*'},
  {"/", '/'},
  {"\\(", '('},
  {"\\)", ')'},
  {"\\$[a-zA-Z0-9]+", TK_REGISTER},
  {"0[xX][0-9a-fA-F]+", TK_NUMBER},  // hex
  {"0|[1-9][0-9]*", TK_NUMBER},
  {"!=", TK_NOTEQ},
  {"&&", TK_AND},
  {"\\|\\|", TK_OR},
  {"==", TK_EQ}         // equal
};

#define NR_REGEX (sizeof(rules) / sizeof(rules[0]) )

static regex_t re[NR_REGEX] = {};

/* Rules are used for many times.
 * Therefore we compile them only once before any usage.
 */
void init_regex() {
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i ++) {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
    if (ret != 0) {
      regerror(ret, &re[i], error_msg, 128);
      panic("regex compilation failed: %s\n%s", error_msg, rules[i].regex);
    }
  }
}

// Operator Precedence of C
// https://en.cppreference.com/w/c/language/operator_precedence
enum {
  OP_LV0 = 0, // number, register
  OP_LV1 = 10, // ()
  OP_LV2_1 = 21, // unary +, -
  OP_LV2_2 = 22, // deference *
  OP_LV3 = 30, // *, /, %
  OP_LV4 = 40, // +, -
  OP_LV7 = 70, // ==, !=
  OP_LV11 = 110, // &&
  OP_LV12 = 120, // ||
};

static Token tokens[32] __attribute__((used)) = {};
static int nr_token __attribute__((used))  = 0;

static bool make_token(char *e) {
  int position = 0;
  int i;
  regmatch_t pmatch;

  nr_token = 0;

  while (e[position] != '\0') {
    /* Try all rules one by one. */
    for (i = 0; i < NR_REGEX; i ++) {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0) {
        char *substr_start = e + position;
        int substr_len = pmatch.rm_eo;

        Log("match rules[%d] = \"%s\" at position %d with len %d: %.*s",
            i, rules[i].regex, position, substr_len, substr_len, substr_start);
        position += substr_len;

        /* TODO: Now a new token is recognized with rules[i]. Add codes
         * to record the token in the array `tokens'. For certain types
         * of tokens, some extra actions should be performed.
         */

        switch (rules[i].token_type) {
          case TK_NOTYPE: break;
          case '+':
          case '-':
            tokens[nr_token].type = rules[i].token_type;
            tokens[nr_token].precedence = OP_LV4;
            ++nr_token;
            break;
          case '*':
          case '/':
            tokens[nr_token].type = rules[i].token_type;
            tokens[nr_token].precedence = OP_LV3;
            ++nr_token;
            break;
          case '(':
          case ')':
            tokens[nr_token].type = rules[i].token_type;
            tokens[nr_token].precedence = OP_LV1;
            ++nr_token;
            break;
          case TK_NUMBER:
            tokens[nr_token].type = rules[i].token_type;
            strncpy(tokens[nr_token].str, substr_start, substr_len);
            tokens[nr_token].precedence = OP_LV0;
            ++nr_token;
            break;
          case TK_REGISTER:
            tokens[nr_token].type = rules[i].token_type;
            strncpy(tokens[nr_token].str, substr_start, substr_len);
            tokens[nr_token].precedence = OP_LV0;
            ++nr_token;
            break;
          case TK_EQ:
          case TK_NOTEQ:
            tokens[nr_token].type = rules[i].token_type;
            tokens[nr_token].precedence = OP_LV7;
            ++nr_token;
            break;
          case TK_AND:
            tokens[nr_token].type = rules[i].token_type;
            tokens[nr_token].precedence = OP_LV11;
            ++nr_token;
            break;
          case TK_OR:
            tokens[nr_token].type = rules[i].token_type;
            tokens[nr_token].precedence = OP_LV12;
            ++nr_token;
            break;
          default:
            Log("Unhandled token found!\n");
            assert(0);
            break;
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

enum {
  BAD_EXPR = 1024, NOT_SURROUND
};

bool check_parentheses(int p, int q, int* error) {
  if (p >= q) {
    *error = BAD_EXPR;
    return false;
  }
  int match_cnt = 0;
  for (int i = p; i <= q; ++i) {
    if (tokens[i].type == '(') {
      ++match_cnt;
    } else if (tokens[i].type == ')') {
      --match_cnt;
    }
    if (match_cnt < 0) {
      *error = BAD_EXPR;
      return false;
    }
  }
  if (match_cnt != 0) {
    *error = BAD_EXPR;
    return false;
  }
  if (tokens[p].type == '(' && tokens[q].type == ')') {
    return true;
  } else {
    *error = NOT_SURROUND;
    return false;
  }
}

uint32_t eval(int p, int q, bool *success) {
  int error;
  if (p > q) {
    *success = false;
    return 0;
  } else if (p == q) {
    uint32_t val;
    if (tokens[p].type == TK_NUMBER) {
      bool is_hex = strlen(tokens[p].str) > 2 && (tokens[p].str[1] == 'x' || tokens[p].str[1] == 'X');
      int ret;
      if (is_hex) {
        ret = sscanf(tokens[p].str, "%x", &val);
      } else {
        ret = sscanf(tokens[p].str, "%d", &val);
      }
      if (ret == 0) {
        *success = false;
      }
    } else if (tokens[p].type == TK_REGISTER) {
      val = isa_reg_str2val(tokens[p].str + 1, success);
    }
    return val;
  } else if (check_parentheses(p, q, &error)) {
    return eval(p + 1, q - 1, success);
  } else {
    if (error == BAD_EXPR) {
      *success = false;
      return 0;
    }
    int op = 0;
    int cur = 0;
    int max_precedence = -1;
    for (int i = p; i <= q; ++i) {
      if (tokens[i].type == '(') {
        ++cur;
      } else if (tokens[i].type == ')') {
        --cur;
      }
      if (cur == 0 && tokens[i].precedence >= max_precedence) {
        max_precedence = tokens[i].precedence;
        op = i;
      }
    }
    uint32_t val1 = 0;
    if (tokens[op].type != TK_DEREFERENCE && tokens[op].type != TK_MINUS) {
      val1 = eval(p, op - 1, success);
    }
    uint32_t val2 = eval(op + 1, q, success);
    switch (tokens[op].type) {
      case '+':
        return val1 + val2;
        break;
      case '-':
        return val1 - val2;
        break;
      case '*':
        return val1 * val2;
        break;
      case '/':
        if (val2 == 0) {
          panic("Division by zero!!");
        }
        return val1 / val2;
        break;
      // TK_DEREFERENCE and TK_MINUS do not support recursive expr evaluation
      case TK_DEREFERENCE:
        return vaddr_read(val2, 4);
        break;
      case TK_MINUS:
        return -val2;
        break;
      case TK_EQ:
        return (val1 == val2);
        break;
      case TK_NOTEQ:
        return (val1 != val2);
        break;
      case TK_AND:
        return (val1 && val2);
        break;
      case TK_OR:
        return (val1 || val2);
        break;

      default:
        Log("Unhandled op %s\n", tokens[op].str);
        assert(0);
    }
  }
}

bool check_unary(int token_type) {
  return (
    token_type == '('
    || token_type == '+'
    || token_type == '-'
    || token_type == '*'
    || token_type == '/'
    || token_type == TK_MINUS
    || token_type == TK_DEREFERENCE
  );
}

uint32_t expr(char *e, bool *success) {
  if (!make_token(e)) {
    *success = false;
    return 0;
  }

  // recognize TK_DEREFERENCE and TK_MINUS
  for (int i = 0; i < nr_token; ++i) {
    if (tokens[i].type == '*' && (i == 0 || check_unary(tokens[i - 1].type))) {
      tokens[i].type = TK_DEREFERENCE;
      tokens[i].precedence = OP_LV2_2;
    }
    if (tokens[i].type == '-' && (i == 0 || check_unary(tokens[i - 1].type))) {
      tokens[i].type = TK_MINUS;
      tokens[i].precedence = OP_LV2_1;
    }
  }
  
  // Log("nr_token: %d", nr_token);
  // for (int i = 0; i < nr_token; ++i) {
  //   if (tokens[i].type < 256) {
  //     Log("%c", tokens[i].type);
  //   } else {
  //     if (tokens[i].type == TK_NUMBER) {
  //       Log("number: %s", tokens[i].str);
  //     } else if (tokens[i].type == TK_REGISTER) {
  //       Log("register: %s", tokens[i].str);
  //     } else if (tokens[i].type == TK_MINUS) {
  //       Log("minus: %s", "-");
  //     } else if (tokens[i].type == TK_DEREFERENCE) {
  //       Log("dereference: %s", "*");
  //     }
  //   }
  // }

  /* TODO: Insert codes to evaluate the expression. */
  return eval(0, nr_token - 1, success);
}
