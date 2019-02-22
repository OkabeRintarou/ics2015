#include <malloc.h>
#include "nemu.h"
#include "monitor/watchpoint.h"
#include "monitor/expr.h"

#define NR_WP 32

static WP wp_pool[NR_WP];
static WP *head, *free_;
static int wp_number = 0;

void init_wp_pool() {
	int i;
	for(i = 0; i < NR_WP; i ++) {
		wp_pool[i].NO = i;
		wp_pool[i].next = &wp_pool[i + 1];
	}
	wp_pool[NR_WP - 1].next = NULL;

	head = NULL;
	free_ = wp_pool;
}

WP *new_wp(const char *exp) {

  Assert(exp, "expression must be valid");

  WP *ret = NULL;
  if (free_) {
    ret = free_;
    free_ = free_->next;
  } else {
    Assert(false, "no enough free watch point");
  }

  size_t len = strlen(exp) + 1;
  char *str = malloc(sizeof(char) * len);
  strcpy(str, exp);
  str[len - 1] = 0;
  ret->expression = str;
  ret->NO = ++wp_number;
  ret->next = NULL;

  ret->next = head;
  head = ret;
  return ret;
}

void free_wp(WP *wp) {
  free(wp->expression);
  memset(wp, 0, sizeof(WP));
  wp->next = free_;
  free_ = wp;
}

WP **get_watch_points() {
  return &head;
}

