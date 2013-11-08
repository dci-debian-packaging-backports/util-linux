#ifndef CANONICALIZE_H
#define CANONICALIZE_H

#include "c.h"	/* for PATH_MAX */

extern char *canonicalize_path(const char *path);
extern char *canonicalize_dm_name(const char *ptname);
extern char *canonicalize_path_restricted(const char *path);

#endif /* CANONICALIZE_H */
