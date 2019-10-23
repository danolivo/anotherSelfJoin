#ifndef PATH_WALKER_H
#define PATH_WALKER_H

#include "nodes/pathnodes.h"

extern bool walk_path_tree(Path *path, void *context);
extern bool path_tree_walker(Path *path, bool (*walker) (), void *context);

#endif /* PATH_WALKER_H */
