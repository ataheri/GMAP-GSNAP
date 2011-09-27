/* $Id: chrsubset.h,v 1.1 2005/03/04 20:48:09 twu Exp $ */
#ifndef CHRSUBSET_INCLUDED
#define CHRSUBSET_INCLUDED
#include "bool.h"
#include "genomicpos.h"
#include "iit-read.h"

#define T Chrsubset_T
typedef struct T *T;

extern void
Chrsubset_print (T this);
bool
Chrsubset_includep (T this, Genomicpos_T position, IIT_T chromosome_iit);
extern T
Chrsubset_read (char *user_chrsubsetfile, char *genomesubdir, char *fileroot, 
		char *user_chrsubsetname, IIT_T chromosome_iit);

#undef T
#endif
