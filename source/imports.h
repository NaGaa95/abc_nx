/* imports.h -- .so import resolution table */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

extern FILE *stderr_fake;

void update_imports(void);

#endif
