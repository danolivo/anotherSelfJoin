#include "postgres.h"

#include "miscadmin.h"

#include "path_walker.h"


bool
walk_path_tree(Path *path, void *context)
{
//	elog(INFO, "--> type: %d, pathtype: %d", path->type, path->pathtype);
	return path_tree_walker(path, walk_path_tree, context);
}

bool
path_tree_walker(Path *path, bool (*walker) (), void *context)
{
	Path *subpath = NULL;
	Path *outerpath = NULL;
	Path *innerpath = NULL;
	List *pathlist = NIL;

	check_stack_depth();

	switch (nodeTag(path))
	{
	case T_AggPath:
		subpath = ((AggPath *) path)->subpath;
		break;
	case T_ProjectionPath:
		subpath = ((ProjectionPath *) path)->subpath;
		break;
	case T_LockRowsPath:
		subpath = ((LockRowsPath *) path)->subpath;
		break;
	case T_MaterialPath:
		subpath = ((MaterialPath *) path)->subpath;
		break;
	case T_BitmapHeapPath:
		subpath = ((BitmapHeapPath *) path)->bitmapqual;
		break;
	case T_LimitPath:
		subpath = ((LimitPath *) path)->subpath;
		break;

	case T_ModifyTablePath:
		pathlist = ((ModifyTablePath *) path)->subpaths;
		break;
	case T_CustomPath:
		pathlist = ((CustomPath *) path)->custom_paths;
		break;

	case T_NestPath:
	case T_HashPath:
	case T_MergePath:
		outerpath = ((JoinPath *) path)->outerjoinpath;
		innerpath = ((JoinPath *) path)->innerjoinpath;
		break;

	case T_Path:
	case T_IndexPath:
	case T_GroupResultPath:
		/* Simple path. No subtree can be found. */
		break;

	default:
		/* If we found unknown path node - exit. */
		elog(WARNING, "Unknown path node: type = %d, pathtype = %d", path->type, path->pathtype);
		return true;
	}

	/* Go down the path tree */

	if (subpath != NULL && walker(subpath, context))
		return true;

	if (outerpath != NULL || innerpath != NULL)
	{
		if (walker(outerpath, context))
			return true;
		if (walker(innerpath, context))
			return true;
	}

	if (pathlist != NIL)
	{
		ListCell *lc;

		foreach (lc, pathlist)
		{
			if (walker((Path *) lfirst(lc), context))
				return true;
		}
	}

	return false;
}
