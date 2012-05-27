/* $Id: gregion.h 64181 2012-05-16 00:18:33Z twu $ */
#ifndef GREGION_INCLUDED
#define GREGION_INCLUDED
#include "bool.h"
#include "genomicpos.h"
#include "chrnum.h"
#include "iit-read.h"
#include "match.h"

#define T Gregion_T
typedef struct T *T;

extern void
Gregion_print (T this);

extern void
Gregion_free (T *old);

extern Genomicpos_T
Gregion_genomicstart (T this);

extern Genomicpos_T
Gregion_genomicend (T this);

extern Genomicpos_T
Gregion_genomiclength (T this);

extern bool
Gregion_plusp (T this);

extern bool
Gregion_revcompp (T this);

extern int
Gregion_genestrand (T this);

extern Chrnum_T
Gregion_chrnum (T this);

extern char *
Gregion_chr (T this, IIT_T chromosome_iit);

extern Genomicpos_T
Gregion_chrpos (T this);

extern Genomicpos_T
Gregion_chroffset (T this);

extern Genomicpos_T
Gregion_chrlength (T this);

extern int
Gregion_querystart (T this);

extern int
Gregion_queryend (T this);

extern int
Gregion_matchsize (T this);

extern double
Gregion_weight (T this);

extern int
Gregion_support (T this);

extern bool 
Gregion_extendedp (T this);

extern void
Gregion_set_ncovered (T this, int ncovered, int source);

extern int
Gregion_ncovered (T this);


extern T
Gregion_new (int nexons, Genomicpos_T genomicstart, Genomicpos_T genomicend,
	     bool plusp, int genestrand, IIT_T chromosome_iit, int querystart, int queryend, 
	     int querylength, int matchsize, int trimstart, int trimend);

extern T
Gregion_new_from_matches (Match_T match5, Match_T match3, int genestrand, IIT_T chromosome_iit,
			  int querylength, int matchsize, int trimstart, int trimend);

extern List_T
Gregion_filter_unique (List_T gregionlist);

extern List_T
Gregion_filter_support (List_T gregionlist, int boundary, double pct_max, int diff_max);

extern double
Gregion_best_weight (List_T gregionlist);

extern List_T
Gregion_filter_by_evidence (List_T gregionlist);

extern bool
Gregion_sufficient_support (T this);

extern void
Gregion_extend (T this, Genomicpos_T extension5, Genomicpos_T extension3, int querylength,
		int min_extra_end);

extern int
Gregion_cmp (const void *a, const void *b);

extern List_T
Gregion_filter_clean (List_T gregionlist, int nchrs);

#undef T
#endif


