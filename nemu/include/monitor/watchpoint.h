#ifndef __WATCHPOINT_H__
#define __WATCHPOINT_H__

#include "common.h"

typedef struct watchpoint {
	int NO;
	struct watchpoint *next;

	char *expression;


} WP;

WP **get_watch_points();
WP *new_wp(const char *exp);
void free_wp(WP *wp);

#endif
