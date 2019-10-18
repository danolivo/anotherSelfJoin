/*-------------------------------------------------------------------------
 *
 * nodeSelfjoin.h
 *
 *
 *
 * Copyright (c) 2016-2019, Postgres Professional
 *
 * src/include/executor/nodeSelfjoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESELFJOIN_H
#define NODESELFJOIN_H

#include "nodes/extensible.h"

typedef struct SJPath
{
	CustomPath cp;
} SJPath;


extern void SelfJoin_Init_methods(void);
extern SJPath *create_sj_path(PlannerInfo *root, RelOptInfo *rel, List *childs);

#endif							/* NODESELFJOIN_H */
