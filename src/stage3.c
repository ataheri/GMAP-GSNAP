static char rcsid[] = "$Id: stage3.c 105190 2013-08-19 18:47:42Z twu $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "stage3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>		/* For memcpy */
#include <math.h>		/* For pow() */

#include "assert.h"
#include "mem.h"
#include "comp.h"
#include "pair.h"
#include "pairdef.h"
#include "listdef.h"
#include "comp.h"
#include "chrnum.h"
#include "genomicpos.h"
#include "smooth.h"
#include "scores.h"
#include "intron.h"
#include "pbinom.h"
#include "changepoint.h"
#include "translation.h"
#ifdef PMAP
#include "backtranslation.h"
#endif
#include "complement.h"
#include "iit-read.h"
#include "stage2.h"
#include "maxent.h"
#include "maxent_hr.h"


/* The following are the same as dividing by 2048 and 1024 */
#define goodness_intronlen(x) (x >> 11)
#define goodness_nonintronlen(x) (x >> 10)


#define MAXITER_CYCLES 5
#define MAXITER_SMOOTH_BY_SIZE 2
#define MAXITER_INTRONS 2
#define MAXITER_KNOWNSPLICE 1	/* Effectively turns off iteration */

#define INTRON_PENALTY_INCONSISTENT 16
#define NONCANONICAL_PENALTY 12

#define SINGLESLEN 9	/* Should be same as MININTRONLEN */
#define MININTRONLEN 9		/* Determines when Dynprog_genome_gap gets called vs Dynprog_single_gap */
#define MININTRONLEN_FINAL 50	/* Determines when to perform final
				   pass to find canonical introns */
#define MINENDEXON 12

#define SUFF_MATCHES_KEEP 300

/* Old parameters for extrapeel */
/* #define SUFFCONSECUTIVE 5 */
/* #define MAXINCURSION 5 */

/* For Stage3_append */
#define MERGELENGTH 500000
#define LONG_MERGELENGTH 1000000 /* For strong donor and acceptor splice sites */
#define DONOR_THRESHOLD 0.90
#define ACCEPTOR_THRESHOLD 0.90

#define MINCOVERAGE 0.10  /* Not used anymore */

#define DYNPROGINDEX_MAJOR -1
#define DYNPROGINDEX_MINOR +1

#define DUAL_BREAK_PROB_THRESHOLD 0.90
#define MIN_STAGE2_FOR_DUALBREAK 24

#define THETA_SLACK 0.10
#define TRIM_END_PVALUE 1e-4

#define NEARBY_INDEL 6
#define INDEL_SPLICE_ENDLENGTH 12
#define NONCANONICAL_ACCEPT 15
#define NONCANONICAL_PERFECT_MATCHES 12

#define MAXPEELBACK_SCORE 5	/* For determining goodness of intron */
#define MAXPEELBACK_END 1000

#define DUALBREAK_QUERYJUMP_FACTOR 10

#define SCORE_SIGDIFF 5
#define PROB_SIGDIFF 0.5

#define END_SPLICESITE_SEARCH 4
#define END_SPLICESITE_PROB 0.95
#define END_SPLICESITE_EXON_LENGTH 100  /* If shorter than this, then don't look for end splice site */


/* For Stage3_bad_stretch_p */
#define LOG_99 -0.01005033585
#define LOG_01 -4.605170186

#if 0
/* Switches on 5 consecutive mismatches */
#define LOG_99_9999 -0.01015034085
#define LOG_99_0001 -9.220390708
#define LOG_25_0001 -10.59663473
#define LOG_25_9999 -1.386394366
#define LOG_01_9999 -4.605270191
#define LOG_01_0001 -13.81551056
#define LOG_75_0001 -9.498022444
#define LOG_75_9999 -0.2877820775
#endif

#if 1
#define LOG_99_999 -0.01105083619
#define LOG_99_001 -6.917805615
#define LOG_25_001 -8.29404964
#define LOG_25_999 -1.387294861
#define LOG_01_999 -4.606170686
#define LOG_01_001 -11.51292546
#define LOG_75_001 -7.195437351
#define LOG_75_999 -0.2886825728
#endif

#if 0
/* Switches on 4 consecutive mismatches */
#define LOG_99_99 -0.02010067171
#define LOG_99_01 -4.615220522
#define LOG_25_01 -5.991464547
#define LOG_25_99 -1.396344697
#define LOG_01_99 -4.615220522
#define LOG_01_01 -9.210340372
#define LOG_75_01 -4.892852258
#define LOG_75_99 -0.2977324083
#endif


/* #define EXTRACT_GENOMICSEG 1 */


static const Except_T gapcheck_error = {"Gap check failed"};
static const Except_T coordinate_error = {"Coordinate error"};

#define SHORTCUT 1		/* Skips re-solving introns if already canonical */

/* In debug mode, probably want to activate debug in pairpool.c and
   dynprog.c also */
#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif

/* Pair dump */
#ifdef DEBUG1
#define debug1(x) x
#else 
#define debug1(x)
#endif

/* Chimeras */
#ifdef DEBUG2
#define debug2(x) x
#else
#define debug2(x)
#endif

/* path_trim */
#ifdef DEBUG3
#define debug3(x) x
#else
#define debug3(x)
#endif

/* Fix adjacent indels */
#ifdef DEBUG4
#define debug4(x) x
#else 
#define debug4(x)
#endif

/* HMM */
#ifdef DEBUG5
#define debug5(x) x
#else 
#define debug5(x)
#endif

/* assign_gap_types and fill_in_gaps */
#ifdef DEBUG7
#define debug7(x) x
#else 
#define debug7(x)
#endif

/* changepoint */
#ifdef DEBUG8
#define debug8(x) x
#else 
#define debug8(x)
#endif

/* bad_stretch_p */
#ifdef DEBUG9
#define debug9(x) x
#else 
#define debug9(x)
#endif

/* chimera */
#ifdef DEBUG10
#define debug10(x) x
#else 
#define debug10(x)
#endif

/* pick_cdna_direction */
#ifdef DEBUG11
#define debug11(x) x
#else 
#define debug11(x)
#endif

/* splicesitepos */
#ifdef DEBUG12
#define debug12(x) x
#else 
#define debug12(x)
#endif

/* trimming at novel splice sites at ends */
#ifdef DEBUG13
#define debug13(x) x
#else 
#define debug13(x)
#endif

/* build_dual_breaks */
#ifdef DEBUG14
#define debug14(x) x
#else 
#define debug14(x)
#endif



static bool splicingp;
static bool novelsplicingp;
static bool require_splicedir_p;

static IIT_T splicesites_iit;
static int *splicesites_divint_crosstable;

static int donor_typeint;
static int acceptor_typeint;

static Univcoord_T *splicesites;

static int min_intronlength;
static int max_deletionlength;

static bool output_sam_p;

void
Stage3_setup (bool splicingp_in, bool novelsplicingp_in, bool require_splicedir_p_in,
	      IIT_T splicesites_iit_in, int *splicesites_divint_crosstable_in,
	      int donor_typeint_in, int acceptor_typeint_in,
	      Univcoord_T *splicesites_in,
	      int min_intronlength_in, int max_deletionlength_in,
	      bool output_sam_p_in) {
  splicingp = splicingp_in;
  novelsplicingp = novelsplicingp_in;
  require_splicedir_p = require_splicedir_p_in;

  splicesites_iit = splicesites_iit_in;
  splicesites_divint_crosstable = splicesites_divint_crosstable_in;
  donor_typeint = donor_typeint_in;
  acceptor_typeint = acceptor_typeint_in;

  splicesites = splicesites_in;

  min_intronlength = min_intronlength_in;
  max_deletionlength = max_deletionlength_in;

  output_sam_p = output_sam_p_in;

  return;
}


/************************************************************************
 *   Stage 3 merges cDNA-genomic pairs from stage 2 (called "path")
 *   and from dynamic programming into a single list.  In this
 *   process, stage 3 may also have to pop a pair off the path, insert
 *   the dynamic programming results (called "gappairs") and then push
 *   the stored pair onto the list.  The relevant pointers are
 *   leftquerypos and leftgenomepos, which refer to the left (stored) pair;
 *   rightquerypos and rightgenomepos, which refer to the right (top) pair on
 *   the list (which may represent what the list should have for the
 *   purposes of dynamic programming); and querydp5, genomedp5,
 *   querydp3, and genomedp3, which refer to the dynamic programming
 *   indices, inclusive.
 *
 *   path has the end of the query sequence as its car.
 *   pairs has the beginning of the query sequence as its car.
 *
 *   Most procedures take the top of path and put it onto pairs:
 *
 *	 <- <- path  =====>  pairs -> ->
 *	   leftpair	     rightpair
 *
 *   For stage 1 and stage 2, the poly-A/T tails, if any, was stripped
 *   off, but for stage 3, we try to extend these tails if possible.
 *   Therefore, we use the full length and full sequence here, and add
 *   the offset to the path from stage 2.
 ************************************************************************/

#define T Stage3_T
struct T {
  struct Pair_T *pairarray;	/* The array version of pairs_fwd or pairs_rev, with the gaps substituted */
  bool pairarray_freeable_p;
  bool chimera_left_p;		/* Part of a chimera on its querystart end */
  bool chimera_right_p;		/* Part of a chimera on its queryend end */
  int npairs;

  List_T pairs;			/* Winning set of pairs */

  int straintype;
  char *strain;

  Chrnum_T chrnum;
  Univcoord_T chroffset;	/* Start of chromosome chrnum on genome */
  Univcoord_T chrhigh;	        /* End of chromosome chrnum on genome */
  Chrpos_T chrlength;
  int circularpos;

  Chrpos_T chrstart; /* Position on chromosome of start of genomicseg */
  Univcoord_T genomicstart;	/* Start of alignment */
  Univcoord_T genomicend;	/* End of alignment */
  int cdna_direction;
  int sensedir;
  bool watsonp;
  double defect_rate;
  double trimmed_coverage;
  int matches;
  int unknowns;
  int mismatches;
  int qopens;
  int qindels;
  int topens;
  int tindels;
  int noncanonical;
  int goodness;
  int absmq_score;
  int mapq_score;

  int translation_start;
  int translation_end;
  int translation_length;

  int relaastart;
  int relaaend;

  int stage2_source;
  int stage2_indexsize;
#if 0
  double stage2_diag_runtime;
  double stage2_align_runtime;
  double stage2_mapfraction;
  int stage2_maxconsecutive;
#endif

  double stage3_runtime;

  bool joinable_left_p;
  bool joinable_right_p;
};


bool
Stage3_chimera_left_p (T this) {
  return this->chimera_left_p;
}

bool
Stage3_chimera_right_p (T this) {
  return this->chimera_right_p;
}

bool
Stage3_watsonp (T this) {
  return this->watsonp;
}

int
Stage3_cdna_direction (T this) {
  return this->cdna_direction;
}

int
Stage3_sensedir (T this) {
  return this->sensedir;
}


int
Stage3_straintype (T this) {
  return this->straintype;
}

int
Stage3_goodness (T this) {
  debug2(printf("Overall goodness:\n"));
  debug2(printf("  %d matches, %d mismatches, %d qopens, %d qindels, %d topens, %d tindels => %d\n",
		this->matches,this->mismatches,this->qopens,this->qindels,this->topens,this->tindels,this->goodness));

  return this->goodness;
}

int
Stage3_absmq_score (T this) {
  return this->absmq_score;
}

int
Stage3_mapq_score (T this) {
  return this->mapq_score;
}

struct Pair_T *
Stage3_pairarray (T this) {
  return this->pairarray;
}

int
Stage3_npairs (T this) {
  return this->npairs;
}

int
Stage3_matches (T this) {
  return this->matches;
}

int
Stage3_mismatches (T this) {
  return this->mismatches;
}

int
Stage3_indels (T this) {
  /* This should be consistent with the output from Pair_print_pathsummary */
  return this->qindels + this->tindels;
}


int
Stage3_querystart (T this) {
  return Pair_querypos(&(this->pairarray[0]));
}

int
Stage3_queryend (T this) {
  return Pair_querypos(&(this->pairarray[this->npairs-1]));
}

bool
Stage3_joinable_left_p (T this) {
  return this->joinable_left_p;
}

bool
Stage3_joinable_right_p (T this) {
  return this->joinable_right_p;
}

void
Stage3_clear_joinable (T this) {
  this->joinable_left_p = false;
  this->joinable_right_p = false;
  return;
}

void
Stage3_set_joinable_left (T this) {
  this->joinable_left_p = true;
  return;
}

void
Stage3_set_joinable_right (T this) {
  this->joinable_right_p = true;
  return;
}


void
Stage3_print_ends (T this) {
  Pair_print_ends(this->pairs);
  printf(" chimera_left: %d, chimera_right: %d",this->chimera_left_p,this->chimera_right_p);
  printf(" goodness: %d",this->goodness);
  printf("\n");
  return;
}


Chrnum_T
Stage3_chrnum (T this) {
  return this->chrnum;
}

Chrpos_T
Stage3_chrstart (T this) {
  return Pair_genomepos(&(this->pairarray[0]));
}

Chrpos_T
Stage3_chrend (T this) {
  return Pair_genomepos(&(this->pairarray[this->npairs-1]));
}

Univcoord_T
Stage3_genomicstart (T this) {
  /* Should be chroffset + Pair_genomepos(start) */
  return this->genomicstart;
}

Univcoord_T
Stage3_genomicend (T this) {
  /* Should be chroffset + Pair_genomepos(end) */
  return this->genomicend;
}

void
Stage3_set_genomicend (T this, Univcoord_T genomicend) {
  this->genomicend = genomicend;
  return;
}

int
Stage3_circularpos (T this) {
  return this->circularpos;
}



int
Stage3_translation_start (T this) {
  return this->translation_start;
}

int
Stage3_translation_end (T this) {
  return this->translation_end;
}


int
Stage3_domain (T this) {
  int querystart, queryend;

  querystart = Pair_querypos(&(this->pairarray[0]));
  queryend = Pair_querypos(&(this->pairarray[this->npairs-1]));

  return queryend - querystart + 1;
}


int
Stage3_largemargin (int *newstart, int *newend, T this, int queryntlength) {
  int leftmargin, rightmargin;
  int querystart, queryend;

  querystart = Pair_querypos(&(this->pairarray[0]));
  queryend = Pair_querypos(&(this->pairarray[this->npairs-1]));

  if ((leftmargin = querystart) < 0) {
    leftmargin = 0;
  }
  if ((rightmargin = queryntlength - queryend) < 0) {
    rightmargin = 0;
  }

  /* Return larger margin */
  *newstart = querystart;
  *newend = queryend + 1;
  if (leftmargin > rightmargin) {
    /* Trim left */
    return leftmargin;
  } else {
    return rightmargin;
  }
}


double
Stage3_fracidentity (T this) {
  int den;

  if ((den = this->matches + this->mismatches + this->qindels + this->tindels) == 0) {
    return 1.0;
  } else {
    return (double) this->matches/(double) den;
  }
}

Univcoord_T
Stage3_genomicpos (T this, int querypos, bool headp) {
  return this->chroffset + Pair_genomicpos(this->pairarray,this->npairs,querypos,headp);
}


void
Stage3_pathscores (bool *gapp, int *pathscores, T this, int querylength, cDNAEnd_T cdnaend) {
  Pair_pathscores(gapp,pathscores,this->pairarray,this->npairs,this->cdna_direction,querylength,cdnaend);
  return;
}


int
Stage3_chimeric_goodness (int *matches1, int *matches2, T part1, T part2, int breakpoint) {
  int goodness1, goodness2, querystart, queryend;
  int unknowns1, mismatches1, qopens1, qindels1, topens1, tindels1, 
    ncanonical1, nsemicanonical1, nnoncanonical1;
  int unknowns2, mismatches2, qopens2, qindels2, topens2, tindels2, 
    ncanonical2, nsemicanonical2, nnoncanonical2;

  querystart = Pair_querypos(&(part1->pairarray[0]));
  debug2(printf("Chimeric goodness requested for part %d..%d\n",querystart+1,breakpoint));
  Pair_fracidentity_bounded(&(*matches1),&unknowns1,&mismatches1,&qopens1,&qindels1,&topens1,&tindels1,
			    &ncanonical1,&nsemicanonical1,&nnoncanonical1,
			    part1->pairarray,part1->npairs,part1->cdna_direction,
			    querystart,breakpoint);
  goodness1 = (*matches1) + MISMATCH*mismatches1 + QOPEN*qopens1 + QINDEL*qindels1 + TOPEN*topens1 + TINDEL*tindels1;
  debug2(printf("  %d matches, %d mismatches, %d qopens, %d qindels, %d topens, %d tindels => %d\n",
		*matches1,mismatches1,qopens1,qindels1,topens1,tindels1,goodness1));

  queryend = Pair_querypos(&(part2->pairarray[part2->npairs-1]));
  debug2(printf("Chimeric goodness requested for part %d..%d\n",breakpoint+1,queryend+1));
  Pair_fracidentity_bounded(&(*matches2),&unknowns2,&mismatches2,&qopens2,&qindels2,&topens2,&tindels2,
			    &ncanonical2,&nsemicanonical2,&nnoncanonical2,
			    part2->pairarray,part2->npairs,part2->cdna_direction,
			    breakpoint,queryend);
  goodness2 = (*matches2) + MISMATCH*mismatches2 + QOPEN*qopens2 + QINDEL*qindels2 + TOPEN*topens2 + TINDEL*tindels2;
  debug2(printf("  %d matches, %d mismatches, %d qopens, %d qindels, %d topens, %d tindels => %d\n",
		*matches2,mismatches2,qopens2,qindels2,topens2,tindels2,goodness2));

  return goodness1 + goodness2;
}


Chrpos_T
Stage3_genomiclength (T this) {
  if (this->genomicstart < this->genomicend) {
    return this->genomicend - this->genomicstart + 1U;
  } else {
    return this->genomicstart - this->genomicend + 1U;
  }
}


bool
Stage3_passes_filter (T this, double min_trimmed_coverage, double min_identity) {
  int den;

  if (this->chimera_left_p == true) {
    return true;
  } else if (this->chimera_right_p == true) {
    return true;
  } else if (this->trimmed_coverage < min_trimmed_coverage) {
    return false;
  } else if ((den = this->matches + this->mismatches + this->qindels + this->tindels) == 0) {
    /* fracidentity is 1.0 */
    return true;
  } else if ((double) this->matches/(double) den < min_identity) {
    return false;
  } else {
    return true;
  }
}


int
Stage3_cmp (const void *a, const void *b) {
  T x = * (T *) a;
  T y = * (T *) b;
  Chrpos_T x_genomiclength, y_genomiclength;

  if (x->chimera_right_p == true && y->chimera_right_p == false) {
    return -1;
  } else if (y->chimera_right_p == true && x->chimera_right_p == false) {
    return +1;
  } else if (x->chimera_left_p == true && y->chimera_left_p == false) {
    return -1;
  } else if (y->chimera_left_p == true && x->chimera_left_p == false) {
    return +1;
  } else if (x->goodness > y->goodness) {
    return -1;
  } else if (y->goodness > x->goodness) {
    return +1;
  } else if (x->straintype < y->straintype) {
    return -1;
  } else if (y->straintype < x->straintype) {
    return +1;
  } else {
    x_genomiclength = Stage3_genomiclength(x);
    y_genomiclength = Stage3_genomiclength(y);
    if (x_genomiclength < y_genomiclength) {
      return -1;
    } else if (y_genomiclength < x_genomiclength) {
      return +1;
    } else if (x->chrnum < y->chrnum) {
      return -1;
    } else if (y->chrnum < x->chrnum) {
      return +1;
    } else if (x->genomicstart < y->genomicstart) {
      return -1;
    } else if (y->genomicstart < x->genomicstart) {
      return +1;
    } else {
      return 0;
    }
  }
}


int
Stage3_position_cmp (const void *a, const void *b) {
  T x = * (T *) a;
  T y = * (T *) b;
  int querypos1, querypos2;

  if (x->genomicstart < y->genomicstart) {
    return -1;
  } else if (y->genomicstart < x->genomicstart) {
    return +1;
  } else if (x->genomicend < y->genomicend) {
    return -1;
  } else if (y->genomicend < x->genomicend) {
    return +1;
  } else {
    querypos1 = Pair_querypos(&(x->pairarray[0]));
    querypos2 = Pair_querypos(&(y->pairarray[0]));
    if (querypos1 < querypos2) {
      return -1;
    } else if (querypos2 < querypos1) {
      return +1;
    } else {
      querypos1 = Pair_querypos(&(x->pairarray[x->npairs-1]));
      querypos2 = Pair_querypos(&(y->pairarray[y->npairs-1]));
      if (querypos1 < querypos2) {
	return -1;
      } else if (querypos2 < querypos1) {
	return +1;
      } else {
	return 0;
      }
    }
  }
}
					  
int
Stage3_querystart_cmp (const void *a, const void *b) {
  T x = * (T *) a;
  T y = * (T *) b;
  int x_querystart, y_querystart;

  x_querystart = Pair_querypos(&(x->pairarray[0]));
  y_querystart = Pair_querypos(&(y->pairarray[0]));

  if (x_querystart < y_querystart) {
    return -1;
  } else if (y_querystart < x_querystart) {
    return +1;
  } else {
    return 0;
  }
}

int
Stage3_queryend_cmp (const void *a, const void *b) {
  T x = * (T *) a;
  T y = * (T *) b;
  int x_queryend, y_queryend;

  x_queryend = Pair_querypos(&(x->pairarray[x->npairs-1]));
  y_queryend = Pair_querypos(&(y->pairarray[y->npairs-1]));

  if (x_queryend < y_queryend) {
    return -1;
  } else if (y_queryend < x_queryend) {
    return +1;
  } else {
    return 0;
  }
}


int
Stage3_identity_cmp (const void *a, const void *b) {
  T x = * (T *) a;
  T y = * (T *) b;

  if (x < y) {
    return -1;
  } else if (x > y) {
    return +1;
  } else {
    return 0;
  }
}


bool
Stage3_overlap (T x, T y) {

  if (x->straintype != y->straintype) {
    return false;
  } else if (x->watsonp != y->watsonp) {
    return false;
  } else if (x->watsonp) {
    if (x->genomicstart >= y->genomicstart && x->genomicstart <= y->genomicend) {
      return true;
    } else if (y->genomicstart >= x->genomicstart && y->genomicstart <= x->genomicend) {
      return true;
    } else {
      return false;
    }
  } else {
    if (x->genomicstart >= y->genomicend && x->genomicstart <= y->genomicstart) {
      return true;
    } else if (y->genomicstart >= x->genomicend && y->genomicstart <= x->genomicstart) {
      return true;
    } else {
      return false;
    }
  }
}

/************************************************************************
 *   Gaps
 ************************************************************************/

static List_T
check_gaps (List_T pairs, Pairpool_T pairpool) {
  List_T path = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  int queryjump, genomejump;

  debug(printf("\nBeginning check of gaps\n"));
  debug(printf("length = %d\n",List_length(pairs)));
  debug(Pair_dump_list(pairs,true));

  pairptr = pairs;
  pairs = Pairpool_pop(pairs,&pair);
  if (pair->gapp == true) {
    fprintf(stderr,"Gap check error: Unexpected gap at start of pairs\n");
    debug(printf("Gap check error: Unexpected gap at start of pairs\n"));
#ifndef DEBUG
    Except_raise(&gapcheck_error,__FILE__,__LINE__);
#endif
  } else {
#ifdef WASTE
    path = Pairpool_push_existing(NULL,pairpool,pair);
#else
    path = List_push_existing(NULL,pairptr);
#endif
  }

  while (pairs != NULL) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
    if (pair->gapp == true) {
      leftpair = path->first;
      rightpair = pairs->first;
      debug(printf("Observed a gap at %d..%d with queryjump = %d, genomejump = %d\n",
		   leftpair->querypos,rightpair->querypos,pair->queryjump,pair->genomejump));

      queryjump = rightpair->querypos - leftpair->querypos - 1;
      genomejump = rightpair->genomepos - leftpair->genomepos - 1;
      if (leftpair->cdna == ' ') queryjump++;
      if (leftpair->genome == ' ') genomejump++;

      if (pair->queryjump != queryjump) {
	if (rightpair->querypos >= HALFLEN && leftpair->querypos < HALFLEN) {
	  debug(printf("Accept queryjump for gap at %d..%d as probable skiplength.  It's %d, should be %d\n",
		       leftpair->querypos,rightpair->querypos,pair->queryjump,queryjump));
	} else {
	  debug(printf("Gap check error: Wrong queryjump for gap at %d..%d.  It's %d, should be %d\n",
		       leftpair->querypos,rightpair->querypos,pair->queryjump,queryjump));
#ifndef DEBUG
	  Except_raise(&gapcheck_error,__FILE__,__LINE__);
#endif
	}
      }
      if (pair->genomejump != genomejump) {
	debug(printf("Gap check error: Wrong genomejump for gap at %d..%d.  It's %d, should be %d\n",
		     leftpair->querypos,rightpair->querypos,pair->genomejump,genomejump));
#ifndef DEBUG
	Except_raise(&gapcheck_error,__FILE__,__LINE__);
#endif
      }
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif

      /* Process another pair after gap */
      if (pairs == NULL) {
	fprintf(stderr,"Gap check error: Unexpected gap at end of pairs\n");
	debug(printf("Gap check error: Unexpected gap at end of pairs\n"));
#ifndef DEBUG
	Except_raise(&gapcheck_error,__FILE__,__LINE__);
#endif
      }
      pairptr = pairs;
      pairs = Pairpool_pop(pairs,&pair);
      if (pair->gapp == true) {
	fprintf(stderr,"Gap check error: Unexpected gap after gap\n");
#ifndef DEBUG
	Except_raise(&gapcheck_error,__FILE__,__LINE__);
#endif
      }
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif

    } else {
      /* Not a gap */
      leftpair = path->first;
      queryjump = pair->querypos - leftpair->querypos - 1;
      genomejump = pair->genomepos - leftpair->genomepos - 1;
      if (leftpair->cdna == ' ') queryjump++;
      if (leftpair->genome == ' ') genomejump++;

      if (queryjump <= 0 && genomejump <= 0) {
#ifdef WASTE
	path = Pairpool_push_existing(path,pairpool,pair);
#else
	path = List_push_existing(path,pairptr);
#endif
      } else if (queryjump == 0 && genomejump == 0) {
#ifdef WASTE
	path = Pairpool_push_existing(path,pairpool,pair);
#else
	path = List_push_existing(path,pairptr);
#endif
      } else {
	fprintf(stderr,"Gap check error: Unexpected missing gap at %d..%d\n",leftpair->querypos,pair->querypos);
	debug(printf("Gap check error: Unexpected missing gap at %d..%d\n",leftpair->querypos,pair->querypos));
	debug(printf("Gap check error: Pushing a gap at %d..%d because of queryjump = %d, genomejump = %d\n",
		     leftpair->querypos,pair->querypos,queryjump,genomejump));
	/* One place we need accurate queryjump and genomejump */
	path = Pairpool_push_gapholder(path,pairpool,queryjump,genomejump,/*knownp*/false);
#ifdef WASTE
	path = Pairpool_push_existing(path,pairpool,pair);
#else
	path = List_push_existing(path,pairptr);
#endif
#ifndef DEBUG
	Except_raise(&gapcheck_error,__FILE__,__LINE__);
#endif
      }
    }
  }	

  debug(printf("Done with check of gaps\n\n"));

  return path;
}


static char complCode[128] = COMPLEMENT_LC;

static char
get_genomic_nt (char *g_alt, int genomicpos, Univcoord_T chroffset, Univcoord_T chrhigh,
		bool watsonp) {
  char c2, c2_alt;
  Univcoord_T pos;

  if (watsonp) {
    if ((pos = chroffset + genomicpos) < chroffset) { /* Must be <, and not <=, or dynamic programming will fail */
      *g_alt = '*';
      return '*';
      
    } else if (pos >= chrhigh) {
      *g_alt = '*';
      return '*';
      
    } else {
      debug7(printf("At %u, genomicnt is %c\n",
		    genomicpos,Genome_get_char_blocks(&(*g_alt),pos)));
      return Genome_get_char_blocks(&(*g_alt),pos);
    }

  } else {
    if ((pos = chrhigh - genomicpos) < chroffset) { /* Must be <, and not <=, or dynamic programming will fail */
      *g_alt = '*';
      return '*';
      
    } else if (pos >= chrhigh) {
      *g_alt = '*';
      return '*';
      
    } else {
      c2 = Genome_get_char_blocks(&c2_alt,pos);
    }
    debug7(printf("At %u, genomicnt is %c\n",
		  genomicpos,complCode[(int) c2]));
    *g_alt = complCode[(int) c2_alt];
    return complCode[(int) c2];
  }
}

#if 0
static char
get_genomic_nt_genomicseg (char *g_alt, Chrpos_T genomicpos,
			   Univcoord_T chroffset, Univcoord_T chrhigh, bool watsonp,
			   char *genomicseg_ptr, Genome_T genome,
			   bool use_genomicseg_p) {
  char g, c2, c2_alt;
  Univcoord_T pos;

  assert(use_genomicseg_p == false);
  /* Need to allow genomicpos < 0 and genomicpos >= genomiclength for iteration on finding chimeras */
  if (use_genomicseg_p == true && genomicpos < 0) {
    *g_alt = '*';
    return '*';

  } else if (use_genomicseg_p == true && genomicpos >= genomiclength) {
    *g_alt = '*';
    return '*';

  } else if (use_genomicseg_p) {
    debug7(printf("At %u, genomicnt is %c\n",genomicpos,genomicseg_ptr[genomicpos]));
    g = *g_alt = genomicseg_ptr[genomicpos];
    return g;

  } else if (watsonp) {
    if ((pos = chroffset + genomicpos) < chroffset) { /* Must be <, and not <=, or dynamic programming will fail */
      *g_alt = '*';
      return '*';
      
    } else if (pos >= chrhigh) {
      *g_alt = '*';
      return '*';
      
#if 0
    } else if (genome) {
      debug7(printf("At %u, genomicnt is %c\n",
		    genomicpos,Genome_get_char(genome,pos)));
      return Genome_get_char(genome,pos);
#endif

    } else {
      debug7(printf("At %u, genomicnt is %c\n",
		    genomicpos,Genome_get_char_blocks(pos)));
      return Genome_get_char_blocks(&(*g_alt),pos);
    }

  } else {
    if ((pos = chrhigh - genomicpos) < chroffset) { /* Must be <, and not <=, or dynamic programming will fail */
      return '*';
      
    } else if (pos >= chrhigh) {
      return '*';
      
#if 0
    } else if (genome) {
      c2 = Genome_get_char(genome,pos);
#endif

    } else {
      c2 = Genome_get_char_blocks(&c2_alt,pos);
    }
    debug7(printf("At %u, genomicnt is %c\n",
		  genomicpos,complCode[(int) c2]));
    *g_alt = complCode[(int) c2_alt];
    return complCode[(int) c2];
  }
}

static char *
get_genomic_seg (Chrpos_T genomicpos, Univcoord_T chroffset, Univcoord_T chrhigh,
		 int length, bool watsonp,
		 char *genomicseg_ptr, Genome_T genome,
		 bool use_genomicseg_p) {
  char *segment;

  if (use_genomicseg_p) {
    debug7(printf("At %u, genomicseg is %.*s\n",
		  genomicpos,length,genomicseg_ptr[genomicpos]));
    return &(genomicseg_ptr[genomicpos]);

  } else if (watsonp) {
    segment = (char *) CALLOC(length+1,sizeof(char));
    if (genome) {
      Genome_fill_buffer_simple(genome,chroffset + genomicpos,length,segment);
      debug7(printf("At %u, genomicseg is %s\n",genomicpos,segment));
      return segment;
    } else {
      Genome_fill_buffer_blocks(chroffset + genomicpos,length,segment);
      debug7(printf("At %u, genomicseg is %s\n",genomicpos,segment));
      return segment;
    }

  } else {
    if (genome) {
      Genome_fill_buffer_simple(genome,chrhigh - genomicpos,length,segment);
    } else {
      Genome_fill_buffer_blocks(chrhigh - genomicpos,length,segment);
    }
    make_complement_inplace(segment,length);
    debug7(printf("At %u, genomicnt is %s\n",genomicpos,segment));
    return segment;
  }
}
#endif


static List_T
insert_gapholders (List_T pairs, char *queryseq_ptr, char *queryuc_ptr,
		   Univcoord_T chroffset, Univcoord_T chrhigh, bool watsonp,
		   Pairpool_T pairpool) {
  List_T path = NULL, pairptr;
  Pair_T pair, leftpair, gappair = NULL;
  int queryjump, genomejump;
  bool firstp = true;
  char comp, c, g, g_alt;

  /* Remove all existing gaps */
  debug(printf("Beginning deletion/insertion of gaps\n"));

  /* Discard old gap(s) */
  while (pairs != NULL) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
    if (pair->knowngapp == true) {
      /* Keep known introns */
      debug(printf("Keeping a known intron gap with queryjump = %d, genomejump = %d\n",
		   pair->queryjump,pair->genomejump));
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif
    } else if (pair->gapp == true) {
      debug(printf("Removing a gap with queryjump = %d, genomejump = %d\n",
		   pair->queryjump,pair->genomejump));
    } else {
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif
    }
  }

  pairs = List_reverse(path);
  path = (List_T) NULL;

  if (pairs != NULL) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
    path = Pairpool_push_existing(path,pairpool,pair);
#else
    path = List_push_existing(path,pairptr);
#endif
    leftpair = pair;
  }

  while (pairs != NULL) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
    queryjump = pair->querypos - leftpair->querypos - 1;
    genomejump = pair->genomepos - leftpair->genomepos - 1;
    if (leftpair->cdna == ' ') queryjump++;
    if (leftpair->genome == ' ') genomejump++;

    if (pair->knowngapp == true) {
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif

    } else if (leftpair->knowngapp == true) {
      /* Ignore queryjump and genomejump information of gap pair */
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif
    
    } else if (queryjump <= 0 && genomejump <= 0) {
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif

    } else if (queryjump == 1 && genomejump == 1) {
      /* Handle a single mismatch by a simple fill */
      g = get_genomic_nt(&g_alt,leftpair->genomepos+1,chroffset,chrhigh,watsonp);
      /* It is possible for a gap with c == g to occur in the middle of a repetitive oligo, such as poly-A */
      if ((c = queryuc_ptr[leftpair->querypos+1]) == g || c == g_alt) {
	comp = MATCH_COMP;
      } else {
	comp = MISMATCH_COMP;
      }
      debug(printf("Filling a gap at %d..%d because of queryjump = %d, genomejump = %d => query %c, genomic %c\n",
		   leftpair->querypos,pair->querypos,queryjump,genomejump,queryseq_ptr[leftpair->querypos+1],g));
      path = Pairpool_push(path,pairpool,leftpair->querypos+1,leftpair->genomepos+1,queryseq_ptr[leftpair->querypos+1],
			   comp,g,g_alt,/*dynprogindex*/0);
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif

    } else {
      /* Insert new gap.  Need accurate queryjump and genomejump */
      debug(printf("Inserting a gap at %d..%d because of queryjump = %d, genomejump = %d\n",
		   leftpair->querypos,pair->querypos,queryjump,genomejump));
      path = Pairpool_push_gapholder(path,pairpool,queryjump,genomejump,/*knownp*/false);
      gappair = (Pair_T) path->first;
      if (firstp == true) {
	gappair->end_intron_p = true;
	firstp = false;
      }
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif
    }

    leftpair = pair;
  }

  if (gappair != NULL) {
    gappair->end_intron_p = true;
  }
  debug(printf("Ending deletion/insertion of gaps\n"));

  /* debug(Pair_dump_list(path,true)); */
  return path;
}



/* Should call before peel_rightward and peel_leftward, so we don't
   run into gaps that are really indels */
static List_T
assign_gap_types (List_T path, int cdna_direction, bool watsonp, char *queryseq_ptr,
		  Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
		  Pairpool_T pairpool) {
  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  Univcoord_T splicesitepos;
  int queryjump, genomejump, leftquerypos, leftgenomepos, rightquerypos, rightgenomepos, curquerypos,
    introntype, intronlength, genomicpos;
  char left1, left2, right2, right1, left1_alt, left2_alt, right2_alt, right1_alt, c2, c2_alt;

  debug(printf("\n** Starting assign_gap_types\n"));
  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->gapp == false) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pairs == NULL) {
      /* Discard initial gap */

    } else if (path == NULL) {
      /* Discard terminal gap */

    } else {
      queryjump = pair->queryjump;
      genomejump = pair->genomejump;

      if (queryjump == 0 && genomejump == 0) {
	debug7(printf("  Gap is a non-gap\n"));
	/* Discard the gap pair */

      } else if (genomejump == 0) {
	debug7(printf("  Gap is a cDNA insertion\n"));
	/* pair->comp = INDEL_COMP; */

	leftpair = path->first;
	rightpair = pairs->first;
	leftquerypos = leftpair->querypos;
	if (leftpair->cdna == ' ') leftquerypos--;
	rightquerypos = rightpair->querypos;
	rightgenomepos = rightpair->genomepos;

	for (curquerypos = rightquerypos - 1; curquerypos > leftquerypos; --curquerypos) {
	  pairs = Pairpool_push(pairs,pairpool,curquerypos,rightgenomepos,
				queryseq_ptr[curquerypos],INDEL_COMP,/*genome*/' ',/*genomealt*/' ',
				/*dynprogindex*/0);
	}
	/* Discard the gap pair */

      } else if (queryjump > 0) {
	debug7(printf("  Gap is a dual break\n"));
	pair->comp = DUALBREAK_COMP;
#ifdef WASTE
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	pairs = List_push_existing(pairs,pairptr);
#endif

      } else {
	debug7(printf("Gap is an intron\n"));

	leftpair = path->first;
	rightpair = pairs->first;

	leftquerypos = leftpair->querypos;
	leftgenomepos = leftpair->genomepos;
	if (leftpair->cdna == ' ') leftquerypos--;
	if (leftpair->genome == ' ') leftgenomepos--;
	rightquerypos = rightpair->querypos;
	rightgenomepos = rightpair->genomepos;

	left1 = get_genomic_nt(&left1_alt,leftgenomepos+1,chroffset,chrhigh,watsonp);
	left2 = get_genomic_nt(&left2_alt,leftgenomepos+2,chroffset,chrhigh,watsonp);
	right2 = get_genomic_nt(&right2_alt,rightgenomepos-2,chroffset,chrhigh,watsonp);
	right1 = get_genomic_nt(&right1_alt,rightgenomepos-1,chroffset,chrhigh,watsonp);
	debug7(printf("  Dinucleotides are %c%c..%c%c\n",left1,left2,right2,right1));
	introntype = Intron_type(left1,left2,right2,right1,
				 left1_alt,left2_alt,right2_alt,right1_alt,
				 cdna_direction);
	debug7(printf("  Introntype at %u..%u is %s (cdna_direction %d)\n",
		      leftgenomepos,rightgenomepos,Intron_type_string(introntype),cdna_direction));

	intronlength = rightgenomepos - leftgenomepos - 1;
	if (intronlength < min_intronlength) {
	  debug7(printf("  Gap is too short to be an intron (intronlength %d).  Adding pairs from %d downto %d\n",
			intronlength,rightgenomepos-1,leftgenomepos+1));
	  for (genomicpos = rightgenomepos - 1; genomicpos > leftgenomepos; --genomicpos) {
	    c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
	    pairs = Pairpool_push(pairs,pairpool,rightquerypos,genomicpos,' ',/*comp*/SHORTGAP_COMP,c2,c2_alt,
				  /*dynprogindex*/0);
	  }
	  debug7(printf("  Gap is a short gap, so discarding the gap pair\n"));
	  /* Discard the gap pair */

	} else if (cdna_direction > 0) {
	  pair->introntype = introntype;
	  switch (introntype) {
	  case GTAG_FWD: pair->comp = FWD_CANONICAL_INTRON_COMP; break;
	  case GCAG_FWD: pair->comp = FWD_GCAG_INTRON_COMP; break;
	  case ATAC_FWD: pair->comp = FWD_ATAC_INTRON_COMP; break;
	  case NONINTRON: pair->comp = NONINTRON_COMP; break;
	  default: 
	    printf("Unexpected intron type %d\n",introntype);
	    fprintf(stderr,"Unexpected intron type %d\n",introntype);
	    abort();
	  }
	  debug7(printf("  Gap is a fwd intron (intronlength %d), now of type %c\n",intronlength,pair->comp));

	  if (watsonp == true) {
	    splicesitepos = leftgenomepos + 1;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/+1)) {
	      debug12(printf("1. donor at splicesitepos %u is known\n",splicesitepos));
	      pair->donor_prob = 1.0;
	    } else {
	      pair->donor_prob = Maxent_hr_donor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("1. donor at splicesitepos %u has prob %f\n",splicesitepos,pair->donor_prob));
	    }

	    splicesitepos = rightgenomepos;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/+1)) {
	      debug12(printf("2. acceptor at splicesitepos %u is known\n",splicesitepos));
	      pair->acceptor_prob = 1.0;
	    } else {
	      pair->acceptor_prob = Maxent_hr_acceptor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("2. acceptor at splicesitepos %u has prob %f\n",splicesitepos,pair->acceptor_prob));
	    }

	  } else {
	    splicesitepos = (chrhigh - chroffset) - leftgenomepos;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/-1)) {
	      debug12(printf("3. antidonor at splicesitepos %u is known\n",splicesitepos));
	      pair->donor_prob = 1.0;
	    } else {
	      pair->donor_prob = Maxent_hr_antidonor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("3. antidonor at splicesitepos %u has prob %f\n",splicesitepos,pair->donor_prob));
	    }

	    splicesitepos = (chrhigh - chroffset) - rightgenomepos + 1;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/-1)) {
	      debug12(printf("4. antiacceptor at splicesitepos %u is known\n",splicesitepos));
	      pair->acceptor_prob = 1.0;
	    } else {
	      pair->acceptor_prob = Maxent_hr_antiacceptor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("4. antiacceptor at splicesitepos %u has prob %f\n",splicesitepos,pair->acceptor_prob));
	    }
	  }

	  /* Push the gap back on */
#ifdef WASTE
	  pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	  pairs = List_push_existing(pairs,pairptr);
#endif
	  
#ifndef PMAP
	} else if (cdna_direction < 0) {
	  pair->introntype = introntype;
	  switch (introntype) {
	  case ATAC_REV: pair->comp = REV_ATAC_INTRON_COMP; break;
	  case GCAG_REV: pair->comp = REV_GCAG_INTRON_COMP; break;
	  case GTAG_REV: pair->comp = REV_CANONICAL_INTRON_COMP; break;
	  case NONINTRON: pair->comp = NONINTRON_COMP; break;
	  default: 
	    printf("Unexpected intron type %d\n",introntype);
	    fprintf(stderr,"Unexpected intron type %d\n",introntype);
	    abort();
	  }
	  debug7(printf("  Gap is a rev intron (intronlength %d), now of type %c\n",intronlength,pair->comp));

	  if (watsonp == true) {
	    splicesitepos = leftgenomepos + 1;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/-1)) {
	      debug12(printf("5. antiacceptor at splicesitepos %u is known\n",splicesitepos));
	      pair->acceptor_prob = 1.0;
	    } else {
	      pair->acceptor_prob = Maxent_hr_antiacceptor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("5. antiacceptor at splicesitepos %u has prob %f\n",splicesitepos,pair->acceptor_prob));
	    }

	    splicesitepos = rightgenomepos;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/-1)) {
	      debug12(printf("6. antidonor at splicesitepos %u is known\n",splicesitepos));
	      pair->donor_prob = 1.0;
	    } else {
	      pair->donor_prob = Maxent_hr_antidonor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("6. antidonor at splicesitepos %u has prob %f\n",splicesitepos,pair->donor_prob));
	    }

	  } else {
	    splicesitepos = (chrhigh - chroffset) - leftgenomepos;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/+1)) {
	      debug12(printf("7. acceptor at splicesitepos %u is known\n",splicesitepos));
	      pair->acceptor_prob = 1.0;
	    } else {
	      pair->acceptor_prob = Maxent_hr_acceptor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("7. acceptor at splicesitepos %u has prob %f\n",splicesitepos,pair->acceptor_prob));
	    }

	    splicesitepos = (chrhigh - chroffset) - rightgenomepos + 1;
	    if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								      splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/+1)) {
	      debug12(printf("8. donor at splicesitepos %u is known\n",splicesitepos));
	      pair->donor_prob = 1.0;
	    } else {
	      pair->donor_prob = Maxent_hr_donor_prob(chroffset + splicesitepos,chroffset);
	      debug12(printf("8. donor at splicesitepos %u has prob %f\n",splicesitepos,pair->donor_prob));
	    }
	  }

	  /* Push the gap back on */
#ifdef WASTE
	  pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	  pairs = List_push_existing(pairs,pairptr);
#endif

#endif	/* ifndef PMAP */

	} else {
	  /* cdna_direction == 0 */
	  pair->introntype = introntype;
	  pair->comp = NONINTRON_COMP;
	  pair->donor_prob = 0.0;
	  pair->acceptor_prob = 0.0;

	  /* Push the gap back on */
#ifdef WASTE
	  pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	  pairs = List_push_existing(pairs,pairptr);
#endif
	}
      }
    }
  }

  return pairs;
}



/* Modeled after assign_gap_types */
static List_T
remove_indel_gaps (List_T path
#ifdef WASTE
		   , Pairpool_T pairpool
#endif
		   ) {
  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  int queryjump, genomejump, leftgenomepos, rightgenomepos, intronlength;

  debug(printf("\n** Starting assign_gap_types\n"));
  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->gapp == false) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pairs == NULL) {
      /* Discard initial gap */

    } else if (path == NULL) {
      /* Discard terminal gap */

    } else {
      queryjump = pair->queryjump;
      genomejump = pair->genomejump;

      if (queryjump == 0 && genomejump == 0) {
	debug7(printf("  Gap is a non-gap\n"));
	/* Discard the gap pair */

      } else if (genomejump == 0) {
	debug7(printf("  Gap is a cDNA insertion\n"));
	/* pair->comp = INDEL_COMP; */
	/* Discard the gap pair */

      } else if (queryjump > 0) {
	debug7(printf("  Gap is a dual break\n"));
	pair->comp = DUALBREAK_COMP;
#ifdef WASTE
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	pairs = List_push_existing(pairs,pairptr);
#endif

      } else {
	debug7(printf("  Gap is an intron of type %c\n",pair->comp));

	leftpair = path->first;
	rightpair = pairs->first;

	leftgenomepos = leftpair->genomepos;
	if (leftpair->genome == ' ') leftgenomepos--;
	rightgenomepos = rightpair->genomepos;

	intronlength = rightgenomepos - leftgenomepos - 1;
	if (intronlength < min_intronlength) {
	  debug7(printf("  Gap is short (intronlength %d).  Adding pairs from %d downto %d\n",
			intronlength,rightgenomepos-1,leftgenomepos+1));
	  debug7(printf("  Gap is a short gap, so discarding the gap pair\n"));
	  /* Discard the gap pair */

	} else {
	  debug7(printf("  Gap is not short (intronlength %d)\n",intronlength));
	  /* Push the gap back on */
#ifdef WASTE
	  pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	  pairs = List_push_existing(pairs,pairptr);
#endif
	}
      }
    }
  }

  return pairs;
}



#ifdef PMAP
static List_T
undefine_nucleotides (char *queryseq_ptr, int querylength, List_T path, Pairpool_T pairpool, int width) {
  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  int leftquerypos, leftgenomepos, rightquerypos, rightgenomepos, pos;

  debug(printf("\n** Starting undefine_nucleotides\n"));

  if (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
#ifdef WASTE
    pairs = Pairpool_push_existing(NULL,pairpool,pair);
#else
    pairs = List_push_existing(NULL,pairptr);
#endif
    rightquerypos = pair->querypos;
    rightgenomepos = pair->genomepos;
  }

  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->gapp == true) {
      leftpair = path->first;
      rightpair = pairs->first;

      leftquerypos = leftpair->querypos;
      leftgenomepos = leftpair->genomepos;
      if (leftpair->cdna == ' ') leftquerypos--;
      if (leftpair->genome == ' ') leftgenomepos--;

      rightquerypos = rightpair->querypos;
      rightgenomepos = rightpair->genomepos;

      debug(printf("Undefining around rightquerypos = %d and leftquerypos = %d\n",rightquerypos,leftquerypos));
      for (pos = rightquerypos; pos < rightquerypos + width && pos < querylength; pos++) {
	queryseq_ptr[pos] = BACKTRANSLATE_CHAR;
      }
      for (pos = leftquerypos; pos > leftquerypos - width && pos >= 0; --pos) {
	queryseq_ptr[pos] = BACKTRANSLATE_CHAR;
      }
    }
#ifdef WASTE
    pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
    pairs = List_push_existing(pairs,pairptr);
#endif
  }

  return pairs;
}  
#endif


static List_T
add_dualbreak (List_T pairs, char *queryseq_ptr, 
#ifdef PMAP
	       char *queryaaseq_ptr,
#endif
	       Univcoord_T chroffset, Univcoord_T chrhigh, int cdna_direction,
	       bool watsonp, Pair_T leftpair, Pair_T rightpair, Pairpool_T pairpool, int ngap,
	       int diagnosticp) {
  int genomicpos, k;
  int leftquerypos, leftgenomepos, rightquerypos, rightgenomepos, gapgenomepos, midpoint;
  int introntype;
  char left1, left2, right2, right1, left1_alt, left2_alt, right2_alt, right1_alt;
  char c1, c2, c2_alt, comp;

  leftquerypos = leftpair->querypos;
  leftgenomepos = leftpair->genomepos;
  if (leftpair->cdna == ' ') leftquerypos--;
  if (leftpair->genome == ' ') leftgenomepos--;
  rightquerypos = rightpair->querypos;
  rightgenomepos = rightpair->genomepos;

  /* Previously checked for genomicuc_ptr != NULL, but this does not
     work with second round of prepare_for_printing */
  left1 = get_genomic_nt(&left1_alt,leftgenomepos+1,chroffset,chrhigh,watsonp);
  left2 = get_genomic_nt(&left2_alt,leftgenomepos+2,chroffset,chrhigh,watsonp);
  right2 = get_genomic_nt(&right2_alt,rightgenomepos-2,chroffset,chrhigh,watsonp);
  right1 = get_genomic_nt(&right1_alt,rightgenomepos-1,chroffset,chrhigh,watsonp);
  
  debug7(printf("  Dinucleotides are %c%c..%c%c\n",left1,left2,right2,right1));
  introntype = Intron_type(left1,left2,right2,right1,
			   left1_alt,left2_alt,right2_alt,right1_alt,
			   cdna_direction);
  debug7(printf("  Introntype at %u..%u is %s (cdna_direction %d)\n",
		leftgenomepos,rightgenomepos,Intron_type_string(introntype),cdna_direction));
  switch (introntype) {
  case GTAG_FWD: comp = FWD_CANONICAL_INTRON_COMP; break;
  case GCAG_FWD: comp = FWD_GCAG_INTRON_COMP; break;
  case ATAC_FWD: comp = FWD_ATAC_INTRON_COMP; break;
#ifndef PMAP
  case ATAC_REV: comp = REV_ATAC_INTRON_COMP; break;
  case GCAG_REV: comp = REV_GCAG_INTRON_COMP; break;
  case GTAG_REV: comp = REV_CANONICAL_INTRON_COMP; break;
#endif
  case NONINTRON: comp = NONINTRON_COMP; break;
  default: 
    printf("Unexpected intron type %d\n",introntype);
    fprintf(stderr,"Unexpected intron type %d\n",introntype);
    abort();
  }
  /* End of check */


  /* queryjump = rightquerypos - leftquerypos - 1; */
  /* genomejump = rightgenomepos - leftgenomepos - 1; */

  if (diagnosticp == true) {
    for (k = 0; k < ngap; k++) {
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,rightgenomepos,
				     /*cdna*/' ',DUALBREAK_COMP,introntype,/*genome*/' ',/*genomealt*/' ',
				     /*extraexonp*/false);
    }
    for (k = 0; k < 3; k++) {
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,rightgenomepos,
				     /*cdna*/' ',INTRONGAP_COMP,introntype,/*genome*/' ',/*genomealt*/' ',
				     /*extraexonp*/false);
    }
    for (k = 0; k < ngap; k++) {
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,rightgenomepos,
				     /*cdna*/' ',DUALBREAK_COMP,introntype,/*genome*/' ',/*genomealt*/' ',
				     /*extraexonp*/false);
    }

  } else if (rightgenomepos - leftgenomepos - 1 < ngap + ngap) {
    midpoint = (rightgenomepos + leftgenomepos) / 2;

    /* First insertion */
    for (genomicpos = rightgenomepos - 1; genomicpos >= midpoint; --genomicpos) {
      c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,genomicpos,
				     /*cdna*/' ',comp,introntype,c2,c2_alt,/*extraexonp*/true);
    }

    /* cDNA sequence */
    gapgenomepos = genomicpos + 1;
    for (k = rightquerypos - 1; k > leftquerypos; --k) {
#ifdef PMAP
      c1 = Dynprog_codon_char(queryaaseq_ptr[k/3],k%3);
#else
      c1 = queryseq_ptr[k];
#endif
      pairs = Pairpool_push_gapalign(pairs,pairpool,k,gapgenomepos,
				     c1,EXTRAEXON_COMP,/*introntype*/NONINTRON,c1,c1,
				     /*extraexonp*/true); /* Transfer cDNA char to genome */
    }

    /* Second insertion */
    for (genomicpos = midpoint - 1; genomicpos > leftgenomepos; --genomicpos) {
      c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
      pairs = Pairpool_push_gapalign(pairs,pairpool,leftquerypos,genomicpos,
				     /*cdna*/' ',comp,introntype,c2,c2_alt,/*extraexonp*/true);
    }

  } else {

    /* First insertion */
    for (k = 0, genomicpos = rightgenomepos - 1; k < ngap; k++, --genomicpos) {
      c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,genomicpos,
				     /*cdna*/' ',comp,introntype,c2,c2_alt,/*extraexonp*/true);
    }

    /* cDNA sequence */
    gapgenomepos = genomicpos + 1;
    for (k = rightquerypos - 1; k > leftquerypos; --k) {
#ifdef PMAP
      c1 = Dynprog_codon_char(queryaaseq_ptr[k/3],k%3);
#else
      c1 = queryseq_ptr[k];
#endif
      pairs = Pairpool_push_gapalign(pairs,pairpool,k,gapgenomepos,
				     c1,EXTRAEXON_COMP,/*introntype*/NONINTRON,c1,c1,
				     /*extraexonp*/true); /* Transfer cDNA char to genome */
    }

    /* Second insertion */
    genomicpos = leftgenomepos + ngap;
    for (k = 0; k < ngap; k++, --genomicpos) {
      c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
      pairs = Pairpool_push_gapalign(pairs,pairpool,leftquerypos,genomicpos,
				     /*cdna*/' ',comp,introntype,c2,c2_alt,/*extraexonp*/true);
    }
  }

  return pairs;
}


static List_T
add_intron (List_T pairs, Univcoord_T chroffset, Univcoord_T chrhigh,
	    Pair_T leftpair, Pair_T rightpair, char comp, int introntype, int ngap,
	    bool watsonp, Pairpool_T pairpool) {
  char c2, c2_alt;
  int intronlength, genomicpos;
  int leftgenomepos, rightquerypos, rightgenomepos, gapgenomepos;
  int i;

  leftgenomepos = leftpair->genomepos;
  if (leftpair->genome == ' ') leftgenomepos--;
  rightquerypos = rightpair->querypos;
  rightgenomepos = rightpair->genomepos;

  intronlength = rightgenomepos - leftgenomepos - 1;

  debug7(printf("Adding gap of type %c of length %d\n",comp,intronlength));

#if 0
  /* Should not be necessary to fix introns at this point */
  if (cdna_direction >= 0) {
    switch (*comp) {
    case FWD_CANONICAL_INTRON_COMP: case FWD_GCAG_INTRON_COMP: case FWD_ATAC_INTRON_COMP: case NONINTRON: break;
    default: 
      debug7(printf("Unexpected intron comp %c.  Need to fix.\n",*comp));

      left1 = genomicuc_ptr[leftgenomepos+1];
      left2 = genomicuc_ptr[leftgenomepos+2];
      right2 = genomicuc_ptr[rightgenomepos-2];
      right1 = genomicuc_ptr[rightgenomepos-1];

      debug7(printf("  Dinucleotides are %c%c..%c%c\n",left1,left2,right2,right1));
      introntype = Intron_type(left1,left2,right2,right1,
			       left1_alt,left2_alt,right2_alt,right1_alt,
			       cdna_direction);
      debug7(printf("  Introntype at %u..%u is %s (cdna_direction %d)\n",
		    leftgenomepos,rightgenomepos,Intron_type_string(introntype),cdna_direction));
      switch (introntype) {
      case GTAG_FWD: *comp = FWD_CANONICAL_INTRON_COMP; break;
      case GCAG_FWD: *comp = FWD_GCAG_INTRON_COMP; break;
      case ATAC_FWD: *comp = FWD_ATAC_INTRON_COMP; break;
      case NONINTRON:
	intronlength = rightgenomepos - leftgenomepos - 1;
	if (intronlength < min_intronlength) {
	  *comp = SHORTGAP_COMP;	/* Will be printed as INDEL_COMP, but need to score as NONINTRON_COMP */
	} else {
	  *comp = NONINTRON_COMP;
	}
      }
    }
  } else {
    switch (*comp) {
    case REV_CANONICAL_INTRON_COMP: case REV_GCAG_INTRON_COMP: case REV_ATAC_INTRON_COMP: case NONINTRON: break;
    default: 
      debug7(printf("Unexpected intron comp %c.  Need to fix.\n",*comp));

      left1 = genomicuc_ptr[leftgenomepos+1];
      left2 = genomicuc_ptr[leftgenomepos+2];
      right2 = genomicuc_ptr[rightgenomepos-2];
      right1 = genomicuc_ptr[rightgenomepos-1];

      debug7(printf("  Dinucleotides are %c%c..%c%c\n",left1,left2,right2,right1));
      introntype = Intron_type(left1,left2,right2,right1,
			       left1_alt,left2_alt,right2_alt,right1_alt,
			       cdna_direction);
      debug7(printf("  Introntype at %u..%u is %s (cdna_direction %d)\n",
		    leftgenomepos,rightgenomepos,Intron_type_string(introntype),cdna_direction));
      switch (introntype) {
      case ATAC_REV: *comp = REV_ATAC_INTRON_COMP; break;
      case GCAG_REV: *comp = REV_GCAG_INTRON_COMP; break;
      case GTAG_REV: *comp = REV_CANONICAL_INTRON_COMP; break;
      case NONINTRON:
	intronlength = rightgenomepos - leftgenomepos - 1;
	if (intronlength < min_intronlength) {
	  *comp = SHORTGAP_COMP;	/* Will be printed as INDEL_COMP, but need to score as NONINTRON_COMP */
	} else {
	  *comp = NONINTRON_COMP;
	}
      }
    }
  }
#endif

  if (intronlength < ngap + ngap + 3) {
    for (i = 0, genomicpos = rightgenomepos - 1; i < intronlength; i++, --genomicpos) {
      c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,genomicpos,
				     /*cdna*/' ',comp,introntype,c2,c2_alt,/*extraexonp*/false);
    }
  } else {
    for (i = 0, genomicpos = rightgenomepos - 1; i < ngap; i++, --genomicpos) {
      c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,genomicpos,
				     /*cdna*/' ',comp,introntype,c2,c2_alt,/*extraexonp*/false);
      debug7(printf("Pushing %c at genomicpos %d\n",c2,genomicpos));
    }
    
    gapgenomepos = genomicpos + 1;
    pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,gapgenomepos,' ',INTRONGAP_COMP,/*introntype*/NONINTRON,
				   /*genome*/INTRONGAP_CHAR,/*genomealt*/INTRONGAP_CHAR,/*extraexonp*/false);
    pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,gapgenomepos,' ',INTRONGAP_COMP,/*introntype*/NONINTRON,
				   /*genome*/INTRONGAP_CHAR,/*genomealt*/INTRONGAP_CHAR,/*extraexonp*/false);
    pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,gapgenomepos,' ',INTRONGAP_COMP,/*introntype*/NONINTRON,
				   /*genome*/INTRONGAP_CHAR,/*genomealt*/INTRONGAP_CHAR,/*extraexonp*/false);

    genomicpos = leftgenomepos + ngap;
    for (i = ngap-1; i >= 0; --i, --genomicpos) {
      c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
      pairs = Pairpool_push_gapalign(pairs,pairpool,rightquerypos,genomicpos,
				     /*cdna*/' ',comp,introntype,c2,c2_alt,/*extraexonp*/false);
      debug7(printf("Pushing %c at genomicpos %d\n",c2,genomicpos));
    }
  }

  return pairs;
}



/************************************************************************
 *   Fix adjacent indels
 ************************************************************************/

/* Modeled after print_sam_forward in pair.c */
/* Handles indels next to gaps */
static List_T
fix_adjacent_indels (List_T pairs) {
  List_T path = NULL, pairptr;
  Pair_T prev, this = NULL, pair;
  bool in_exon = false;
  int Mlength = 0, Ilength = 0, Dlength = 0;
  char last_token_type = ' ';
  int last_token_length = 0, i;

  debug4(printf("Starting fix_adjacent_indels: "));

  while (pairs != NULL) {
    prev = this;
    this = (Pair_T) List_head(pairs);

    if (this->gapp) {
      if (in_exon == true) {

	if (Mlength > 0) {
	  last_token_type = 'M';
	  last_token_length = Mlength;
	  debug4(printf("%dM",Mlength));
	} else if (Ilength > 0) {
	  debug4(printf("%dI",Ilength));
	  if (last_token_type == 'I' || last_token_type == 'D') {
	    debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Ilength,'I'));
	    for (i = 0; i < last_token_length + Ilength; i++) {
	      path = Pairpool_pop(path,&pair);
	    }
	    last_token_type = 'I';
	    last_token_length = 0; /* Since we have already taken care of this */
	  } else {
	    last_token_type = 'I';
	    last_token_length = Ilength;
	  }
	} else if (Dlength > 0) {
	  debug4(printf("%dD",Dlength));
	  if (last_token_type == 'I' || last_token_type == 'D') {
	    debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Dlength,'D'));
	    for (i = 0; i < last_token_length + Dlength; i++) {
	      path = Pairpool_pop(path,&pair);
	    }
	    last_token_type = 'D';
	    last_token_length = 0; /* Since we have already taken care of this */
	  } else {
	    last_token_type = 'D';
	    last_token_length = Dlength;
	  }
	}

	Mlength = Ilength = Dlength = 0;
	in_exon = false;
      }

    } else if (this->comp == INTRONGAP_COMP) {
      /* Do nothing */

    } else {
      /* Remaining possibilities are MATCH_COMP, DYNPROG_MATCH_COMP, AMBIGUOUS_COMP, INDEL_COMP, 
	 SHORTGAP_COMP, or MISMATCH_COMP */
      if (in_exon == false) {

	if (last_token_type != ' ') {
	  /* Gap */
	  debug4(printf("?N"));
	  last_token_type = 'N';  /* Could potentially also be considered 'D' */
	  last_token_length = 0;

#if 0
	  query_gap = this->querypos - exon_queryend;
	  if (query_gap > 0) {
	    /* Dual gap.  Don't try to piece together.  */
	    debug4(printf("%dI",query_gap));
	    last_token_type = 'I';
	    last_token_length = query_gap;
	  }
#endif
	}

	in_exon = true;
      }

      if (this->comp == INDEL_COMP || this->comp == SHORTGAP_COMP) {
	/* Gap in upper or lower sequence */
	if (this->genome == ' ') {
	  if (Mlength > 0) {
	    debug4(printf("%dM",Mlength));
	    last_token_type = 'M';
	    last_token_length = Mlength;
	    Mlength = 0;

	  } else if (Dlength > 0) {
	    /* unlikely */
	    debug4(printf("%dD",Dlength));
	    if (last_token_type == 'I' || last_token_type == 'D') {
	      debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Dlength,'D'));
	      for (i = 0; i < last_token_length + Dlength; i++) {
		path = Pairpool_pop(path,&pair);
	      }
	      last_token_type = 'D';
	      last_token_length = 0; /* Since we have already taken care of this */
	    } else {
	      last_token_type = 'D';
	      last_token_length = Dlength;
	      Dlength = 0;
	    }
	  }
	  Ilength++;

	} else if (this->cdna == ' ') {
	  if (Mlength > 0) {
	    debug4(printf("%dM",Mlength));
	    last_token_type = 'M';
	    last_token_length = Mlength;
	    Mlength = 0;

	  } else if (Ilength > 0) {
	    debug4(printf("%dI",Ilength));
	    if (last_token_type == 'I' || last_token_type == 'D') {
	      debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Ilength,'I'));
	      for (i = 0; i < last_token_length + Ilength; i++) {
		path = Pairpool_pop(path,&pair);
	      }
	      last_token_type = 'I';
	      last_token_length = 0; /* Since we have already taken care of this */
	    } else {
	      last_token_type = 'I';
	      last_token_length = Ilength;
	    }
	    Ilength = 0;
	  }
	  Dlength++;

	} else {
	  fprintf(stderr,"Error at %c%c%c\n",this->genome,this->comp,this->cdna);
	  exit(9);
	}

      } else {
	/* Count even if unknown base */

	if (Ilength > 0) {
	  debug4(printf("%dI",Ilength));
	  if (last_token_type == 'I' || last_token_type == 'D') {
	    debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Ilength,'I'));
	    for (i = 0; i < last_token_length + Ilength; i++) {
	      path = Pairpool_pop(path,&pair);
	    }
	    last_token_type = 'I';
	    last_token_length = 0; /* Since we have already taken care of this */
	  } else {
	    last_token_type = 'I';
	    last_token_length = Ilength;
	  }
	  Ilength = 0;

	} else if (Dlength > 0) {
	  debug4(printf("%dD",Dlength));
	  if (last_token_type == 'I' || last_token_type == 'D') {
	    debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Dlength,'D'));
	    for (i = 0; i < last_token_length + Dlength; i++) {
	      path = Pairpool_pop(path,&pair);
	    }
	    last_token_type = 'D';
	    last_token_length = 0; /* Since we have already taken care of this */
	  } else {
	    last_token_type = 'D';
	    last_token_length = Dlength;
	  }
	  Dlength = 0;
	}

	Mlength++;
      }
    }

    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
    path = Pairpool_push_existing(path,pairpool,pair);
#else
    path = List_push_existing(path,pairptr);
#endif
  }

  if (Mlength > 0) {
    debug4(printf("%dM",Mlength));
    /* last_token_type = 'M'; */
    /* last_token_length = Mlength; */
  } else if (Ilength > 0) {
    debug4(printf("%dI",Ilength));
    if (last_token_type == 'I' || last_token_type == 'D') {
      debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Ilength,'I'));
      for (i = 0; i < last_token_length + Ilength; i++) {
	path = Pairpool_pop(path,&pair);
      }
      /* last_token_type = 'I'; */
      /* last_token_length = 0; */ /* Since we have already taken care of this */
    } else {
      /* last_token_type = 'I'; */
      /* last_token_length = Ilength; */
    }
  } else if (Dlength > 0) {
    debug4(printf("%dD",Dlength));
    if (last_token_type == 'I' || last_token_type == 'D') {
      debug4(printf("fix_adjacent_indels found %d%c to %d%c\n",last_token_length,last_token_type,Dlength,'D'));
      for (i = 0; i < last_token_length + Dlength; i++) {
	path = Pairpool_pop(path,&pair);
      }
      /* last_token_type = 'D'; */
      /* last_token_length = 0; */ /* Since we have already taken care of this */
    } else {
      /* last_token_type = 'D'; */
      /* last_token_length = Dlength; */
    }
  }

  debug4(printf("\n"));

  return path;
}



#define NORMAL_STATE 0
#define INSERTION_STATE +1
#define DELETION_STATE -1


static List_T
remove_adjacent_ins_del (bool *foundp, List_T pairs) {
  List_T path = NULL, pairptr;
  Pair_T this, pair;
  int state = NORMAL_STATE;

  *foundp = false;
  while (pairs != NULL) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&this);

    if (this->comp == INDEL_COMP || this->comp == SHORTGAP_COMP) {
      if (this->genome == ' ') {
	if (state == NORMAL_STATE) {
	  path = List_push_existing(path,pairptr);
	  state = INSERTION_STATE;

	} else if (state == INSERTION_STATE) {
	  /* Do nothing */
	  path = List_push_existing(path,pairptr);

	} else if (state == DELETION_STATE) {
	  /* Switch from insertion to deletion */
	  /* Remove past insertion */
	  while (path != NULL &&
		 (((Pair_T) path->first)->comp == INDEL_COMP || ((Pair_T) path->first)->comp == SHORTGAP_COMP) &&
		 ((Pair_T) path->first)->cdna == ' ') {
	    path = Pairpool_pop(path,&pair);
	  }
	  /* Remove future deletion */
	  while (pairs != NULL &&
		 (((Pair_T) pairs->first)->comp == INDEL_COMP || ((Pair_T) pairs->first)->comp == SHORTGAP_COMP) &&
		 ((Pair_T) pairs->first)->genome == ' ') {
	    pairs = Pairpool_pop(pairs,&pair);
	  }
	  *foundp = true;
	  state = NORMAL_STATE;

	} else {
	  abort();
	}

      } else if (this->cdna == ' ') {
	if (state == NORMAL_STATE) {
	  path = List_push_existing(path,pairptr);
	  state = DELETION_STATE;

	} else if (state == DELETION_STATE) {
	  /* Do nothing */
	  path = List_push_existing(path,pairptr);

	} else if (state == INSERTION_STATE) {
	  /* Switch from deletion to insertion */
	  /* Remove past deletion */
	  while (path != NULL &&
		 (((Pair_T) path->first)->comp == INDEL_COMP || ((Pair_T) path->first)->comp == SHORTGAP_COMP) &&
		 ((Pair_T) path->first)->genome == ' ') {
	    path = Pairpool_pop(path,&pair);
	  }
	  /* Remove future insertion */
	  while (pairs != NULL &&
		 (((Pair_T) pairs->first)->comp == INDEL_COMP || ((Pair_T) pairs->first)->comp == SHORTGAP_COMP) &&
		 ((Pair_T) pairs->first)->cdna == ' ') {
	    pairs = Pairpool_pop(pairs,&pair);
	  }
	  *foundp = true;
	  state = NORMAL_STATE;

	} else {
	  abort();
	}

      } else {
	abort();
      }

    } else {
      path = List_push_existing(path,pairptr);
      state = NORMAL_STATE;
    }
  }

  return path;
}



/************************************************************************
 *   Chop (trimming within end exons)
 ************************************************************************/

/* Called only by GMAP, because nucleotide matches in PMAP have several ambiguous matches. */
static List_T
clean_path_end3 (List_T path) {
  Pair_T lastpair;

  debug(printf("clean_path_end3\n"));
  /* Remove any remaining nonmatches at 3' end, which can happen rarely */
  if (path != NULL) {
    lastpair = path->first;
    while (lastpair->comp != MATCH_COMP && lastpair->comp != DYNPROG_MATCH_COMP && lastpair->comp != AMBIGUOUS_COMP) {
      debug(printf("Removing nonmatch at 3' end: "));
      debug(Pair_dump_one(lastpair,/*zerobasedp*/true));
      debug(printf("\n"));
      path = Pairpool_pop(path,&lastpair);
      if (path == NULL) {
	return NULL;
      } else {
	lastpair = path->first;
      }
    }
  }

#ifdef PMAP
  while (path != NULL) {
    lastpair = path->first;
    if (lastpair->querypos % 3 == 2) {
      return path;
    } else {
      debug(printf("PMAP popping querypos %d to get to codon boundary\n",lastpair->querypos));
      path = Pairpool_pop(path,&lastpair);
    }
  }
#endif

  return path;
}


static List_T
clean_pairs_end5 (List_T pairs) {
  Pair_T firstpair;

  debug(printf("clean_pairs_end5\n"));
  /* Remove any remaining nonmatches at 5' end, which can happen rarely */
  if (pairs != NULL) {
    firstpair = pairs->first;
    while (firstpair->comp != MATCH_COMP && firstpair->comp != DYNPROG_MATCH_COMP && firstpair->comp != AMBIGUOUS_COMP) {
      debug(printf("Removing nonmatch at 5' end: "));
      debug(Pair_dump_one(firstpair,/*zerobasedp*/true));
      debug(printf("\n"));
      pairs = Pairpool_pop(pairs,&firstpair);
      if (pairs == NULL) {
	return NULL;
      } else {
	firstpair = pairs->first;
      }
    }
  }

#ifdef PMAP
  while (pairs != NULL) {
    firstpair = pairs->first;
    if (firstpair->querypos % 3 == 0) {
      return pairs;
    } else {
      debug(printf("PMAP popping querypos %d to get to codon boundary\n",firstpair->querypos));
      pairs = Pairpool_pop(pairs,&firstpair);
    }
  }
#endif

  return pairs;
}


/* Called only by GMAP, because nucleotide matches in PMAP have several ambiguous matches. */
static List_T
clean_path_end3_gap_indels (List_T path) {
  Pair_T lastpair;

  debug(printf("clean_path_end3_gap_indels\n"));
  /* Remove any remaining gap/indels at 3' end, which can happen rarely */
  if (path != NULL) {
    lastpair = path->first;
    while (lastpair->gapp == true || lastpair->comp == INDEL_COMP) {
      debug(printf("Removing gap/indel at 3' end: "));
      debug(Pair_dump_one(lastpair,/*zerobasedp*/true));
      debug(printf("\n"));
      path = Pairpool_pop(path,&lastpair);
      if (path == NULL) {
	return NULL;
      } else {
	lastpair = path->first;
      }
    }
  }

#ifdef PMAP
  while (path != NULL) {
    lastpair = path->first;
    if (lastpair->querypos % 3 == 2) {
      return path;
    } else {
      debug(printf("PMAP popping querypos %d to get to codon boundary\n",lastpair->querypos));
      path = Pairpool_pop(path,&lastpair);
    }
  }
#endif

  return path;
}


static List_T
clean_pairs_end5_gap_indels (List_T pairs) {
  Pair_T firstpair;

  debug(printf("clean_pairs_end5_gap_indels\n"));
  /* Remove any remaining gap/indels at 5' end, which can happen rarely */
  if (pairs != NULL) {
    firstpair = pairs->first;
    while (firstpair->gapp == true || firstpair->comp == INDEL_COMP) {
      debug(printf("Removing gap/indel at 5' end: "));
      debug(Pair_dump_one(firstpair,/*zerobasedp*/true));
      debug(printf("\n"));
      pairs = Pairpool_pop(pairs,&firstpair);
      if (pairs == NULL) {
	return NULL;
      } else {
	firstpair = pairs->first;
      }
    }
  }

#ifdef PMAP
  while (pairs != NULL) {
    firstpair = pairs->first;
    if (firstpair->querypos % 3 == 0) {
      return pairs;
    } else {
      debug(printf("PMAP popping querypos %d to get to codon boundary\n",firstpair->querypos));
      pairs = Pairpool_pop(pairs,&firstpair);
    }
  }
#endif

  return pairs;
}


static List_T
chop_ends_by_changepoint (List_T pairs
#ifdef WASTE
			  , Pairpool_T pairpool
#endif
			  ) {
  List_T path;
  Pair_T pair;
  int *matchscores;
  int nmatches, ntotal, nmatches_left, ntotal_left, nmatches_right, ntotal_right;
  int left_edge, right_edge, length, i;
  int side;
  double theta;
  bool chop_left_p = false, chop_right_p = false;

  if (pairs == NULL) {
    return (List_T) NULL;
  } else {
    matchscores = Pair_matchscores_list(&nmatches,&ntotal,&length,pairs);
    debug8(printf("Overall, %d matches/%d total\n",nmatches,ntotal));
  }

  left_edge = Changepoint_left(&nmatches_left,&ntotal_left,matchscores,length);
  right_edge = Changepoint_right(&nmatches_right,&ntotal_right,matchscores,length);

  debug8(printf("At left edge %d (in 0..%d), %d matches/%d total\n",left_edge,List_length(pairs),nmatches_left,ntotal_left));
  debug8(printf("At right edge %d (in 0..%d), %d matches/%d total\n",right_edge,List_length(pairs),nmatches_right,ntotal_right));

  if (right_edge <= left_edge) {
    debug8(printf("Edges cross.  Need to select one.\n"));
    /* Need to select one side to chop. */
    if (ntotal_left == 0 || ntotal - ntotal_left <= 0) {
      side = +1;		/* chop right side */
    } else if (ntotal_right == 0 || ntotal - ntotal_right <= 0) {
      side = -1;		/* chop left side */
    } else {

#if 0
      theta = (double) (nmatches - nmatches_left)/(double) (ntotal - ntotal_left);
      /* Don't have artificially high expectations for theta, e.g., 1.00 */
      theta = theta - THETA_SLACK;
      if (theta < 0.10) {
	/* Protect against negative values */
	theta = 0.10;
      }
      debug8(printf("Testing on left: Pbinom(%d,%d,%f) = %g\n",
		    nmatches_left,ntotal_left,theta,Pbinom(nmatches_left,ntotal_left,theta)));
      pbinom_left = Pbinom(nmatches_left,ntotal_left,theta);

      theta = (double) (nmatches - nmatches_right)/(double) (ntotal - ntotal_right);
      /* Don't have artificially high expectations for theta, e.g., 1.00 */
      theta = theta - THETA_SLACK;
      if (theta < 0.10) {
	/* Protect against negative values */
	theta = 0.10;
      }
      
      debug8(printf("Testing on right: Pbinom(%d,%d,%f) = %g\n",
		    nmatches_right,ntotal_right,theta,Pbinom(nmatches_right,ntotal_right,theta)));
      pbinom_right = Pbinom(nmatches_right,ntotal_right,theta);
      
      if (pbinom_left < pbinom_right) {
	if (pbinom_left < TRIM_END_PVALUE) {
	  side = -1;		/* chop left side */
	} else {
	  side = 0;
	}
      } else if (pbinom_right < pbinom_left) {
	if (pbinom_right < TRIM_END_PVALUE) {
	  side = +1;		/* chop right side */
	} else {
	  side = 0;
	}
      } else {
	side = 0;
      }
#else
      /* Pick shortest side */
      if (ntotal_left < ntotal_right) {
	debug8(printf("left side is shorter\n"));
	side = -1;		/* chop left side */
      } else {
	debug8(printf("right side is shorter\n"));
	side = +1;		/* chop right side */
      }
#endif

    }

    if (side == -1) {
      debug8(printf("Chopping %d on left.\n",left_edge));
      for (i = 0; i < left_edge; i++) {
	pairs = Pairpool_pop(pairs,&pair);
      }
      chop_left_p = true;
    } else if (side == +1) {
      debug8(printf("Chopping %d - %d on right.\n",length,right_edge));
      path = List_reverse(pairs);
      for (i = 0; i < length - right_edge; i++) {
	path = Pairpool_pop(path,&pair);
      }

      pairs = (List_T) NULL;
#ifdef WASTE
      while (path) {
	path = Pairpool_pop(path,&pair);
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
      }
#else
      pairs = Pairpool_transfer(pairs,path);
#endif
      chop_right_p = true;
    }

  } else {
    if (ntotal_left == 0) {
      path = List_reverse(pairs);
    } else if (ntotal - ntotal_left <= 0) {
      path = List_reverse(pairs);
    } else {
      theta = (double) (nmatches - nmatches_left)/(double) (ntotal - ntotal_left);
      /* Don't have artificially high expectations for theta, e.g., 1.00 */
      theta = theta - THETA_SLACK;
      if (theta < 0.10) {
	/* Protect against negative values */
	theta = 0.10;
      }

      debug8(printf("Testing on left: Pbinom(%d,%d,%f) = %g\n",
		    nmatches_left,ntotal_left,theta,Pbinom(nmatches_left,ntotal_left,theta)));
      if (Pbinom(nmatches_left,ntotal_left,theta) > TRIM_END_PVALUE) {
	path = List_reverse(pairs);
      } else {
	debug8(printf("Chopping %d on left.\n",left_edge));
	for (i = 0; i < left_edge; i++) {
	  pairs = Pairpool_pop(pairs,&pair);
	}
	path = (List_T) NULL;
#ifdef WASTE
	while (pairs) {
	  pairs = Pairpool_pop(pairs,&pair);
	  path = Pairpool_push_existing(path,pairpool,pair);
	}
#else
	path = Pairpool_transfer(path,pairs);
#endif
	debug8(printf("path is now length %d\n",List_length(path)));
	chop_left_p = true;
      }
    }

    if (ntotal_right == 0) {
      pairs = List_reverse(path);
    } else if (ntotal - ntotal_right <= 0) {
      pairs = List_reverse(path);
    } else {
      theta = (double) (nmatches - nmatches_right)/(double) (ntotal - ntotal_right);
      /* Don't have artificially high expectations for theta, e.g., 1.00 */
      theta = theta - THETA_SLACK;
      if (theta < 0.10) {
	/* Protect against negative values */
	theta = 0.10;
      }

      debug8(printf("Testing on right: Pbinom(%d,%d,%f) = %g\n",
		    nmatches_right,ntotal_right,theta,Pbinom(nmatches_right,ntotal_right,theta)));
      if (Pbinom(nmatches_right,ntotal_right,theta) > TRIM_END_PVALUE) {
	pairs = List_reverse(path);
      } else {
	debug8(printf("Chopping %d - %d on right.\n",length,right_edge));
	for (i = 0; i < length - right_edge; i++) {
	  path = Pairpool_pop(path,&pair);
	}
	pairs = (List_T) NULL;
#ifdef WASTE
	while (path) {
	  path = Pairpool_pop(path,&pair);
	  pairs = Pairpool_push_existing(pairs,pairpool,pair);
	}
#else
	pairs = Pairpool_transfer(pairs,path);
#endif
	chop_right_p = true;
      }
    }
  }
  
  FREE(matchscores);

  debug8(printf("Returning alignment of length %d\n",List_length(pairs)));

  return pairs;
}


#if 0
/* pairs -> pairs */
static List_T
trim_short_end_exons (bool *trim5p, bool *trim3p, List_T pairs, Pairpool_T pairpool, int minendexon) {
  List_T path, exon, pairptr;
  Pair_T pair;
  int exon_nmatches;

  debug8(printf("Starting trim_short_end5_exons\n"));
  debug8(Pair_dump_list(pairs,true));

  /* Handle first exon */
  if (pairs == NULL) {
    *trim5p = *trim3p = false;
    return (List_T) NULL;
  } else {
    pair = pairs->first;
  }

  exon = (List_T) NULL;
  exon_nmatches = 0;
  while (pairs != NULL && !pair->gapp) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
    exon = Pairpool_push_existing(exon,pairpool,pair);
#else
    exon = List_push_existing(exon,pairptr);
#endif
    if (pair->gapp == false && pair->comp != MISMATCH_COMP && pair->comp != INDEL_COMP) {
      exon_nmatches++;
    }
  }

  if (exon_nmatches >= minendexon) {
    debug8(printf("Keeping first exon of length %d\n",exon_nmatches));
    path = exon;		/* exon already has the gap */
    *trim5p = false;
  } else if (exon_nmatches == 0) {
    debug8(printf("Trimming first exon of length %d.  firstpair must be a gap.\n",exon_nmatches));
    pairs = Pairpool_pop(pairs,&pair); /* discard gap */
    path = (List_T) NULL;
    *trim5p = false;
  } else {
    debug8(printf("Trimming first exon of length %d\n",exon_nmatches));
    path = (List_T) NULL;
    *trim5p = true;
  }

#ifdef WASTE
  while (pairs != NULL) {
    pairs = Pairpool_pop(pairs,&pair);
    path = Pairpool_push_existing(path,pairpool,pair);

  }
#else
  path = Pairpool_transfer(path,pairs);
#endif

  /* Handle last exon */
  if (path == NULL) {
    *trim5p = *trim3p = false;
    return (List_T) NULL;
  } else {
    pair = path->first;
  }

  exon = (List_T) NULL;
  exon_nmatches = 0;
  while (path != NULL && !pair->gapp) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
#ifdef WASTE
    exon = Pairpool_push_existing(exon,pairpool,pair);
#else
    exon = List_push_existing(exon,pairptr);
#endif
    if (pair->gapp == false && pair->comp != MISMATCH_COMP && pair->comp != INDEL_COMP) {
      exon_nmatches++;
    }
  }

  if (exon_nmatches >= minendexon) {
    debug8(printf("Keeping last exon of length %d\n",exon_nmatches));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;
  } else if (exon_nmatches == 0) {
    debug8(printf("Trimming last exon of length %d.  firstpair must be a gap.\n",exon_nmatches));
    path = Pairpool_pop(path,&pair); /* discard gap */
    pairs = (List_T) NULL;
    *trim3p = false;
  } else {
    debug8(printf("Trimming last exon of length %d\n",exon_nmatches));
    pairs = (List_T) NULL;
    *trim3p = true;
  }

#ifdef WASTE
  while (path != NULL) {
    path = Pairpool_pop(path,&pair);
    pairs = Pairpool_push_existing(pairs,pairpool,pair);
  }
#else
  pairs = Pairpool_transfer(pairs,path);
#endif

  debug8(printf("End of trim_short_end_exons: length = %d\n",List_length(pairs)));
  debug8(Pair_dump_list(pairs,true));
  return pairs;
}
#endif


#if 0
/* pairs -> path */
static List_T
trim_short_end5_exons (bool *trim5p, List_T pairs,
#ifdef WASTE
		       Pairpool_T pairpool,
#endif
		       int minendexon) {
  List_T path, exon, pairptr;
  Pair_T pair;
  int exon_nmatches, exon_nmismatches;

  debug8(printf("Starting trim_short_end5_exons\n"));
  debug8(Pair_dump_list(pairs,true));

  /* Handle first exon */
  if (pairs == NULL) {
    *trim5p = false;
    return (List_T) NULL;
  } else {
    pair = pairs->first;
  }

  exon = (List_T) NULL;
  exon_nmatches = exon_nmismatches = 0;
  while (pairs != NULL && !pair->gapp) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
    exon = Pairpool_push_existing(exon,pairpool,pair);
#else
    exon = List_push_existing(exon,pairptr);
#endif
    if (pair->gapp == true) {
      /* Skip */
    } else if (pair->comp == MISMATCH_COMP || pair->comp == INDEL_COMP) {
      exon_nmismatches++;
    } else {
      exon_nmatches++;
    }
  }

  if (exon_nmatches - exon_nmismatches >= minendexon) {
    debug8(printf("Keeping first exon of length %d\n",exon_nmatches));
    path = exon;		/* exon already has the gap */
    *trim5p = false;
  } else if (exon_nmatches == 0) {
    debug8(printf("Trimming first exon of length %d.  firstpair must be a gap.\n",exon_nmatches));
    pairs = Pairpool_pop(pairs,&pair); /* discard gap */
    path = (List_T) NULL;
    *trim5p = false;
  } else {
    debug8(printf("Trimming first exon of length %d\n",exon_nmatches));
    path = (List_T) NULL;
    *trim5p = true;
  }

#ifdef WASTE
  while (pairs != NULL) {
    pairs = Pairpool_pop(pairs,&pair);
    path = Pairpool_push_existing(path,pairpool,pair);

  }
#else
  path = Pairpool_transfer(path,pairs);
#endif

  debug8(printf("End of trim_short_end_exons: length = %d\n",List_length(pairs)));
  debug8(Pair_dump_list(pairs,true));
  return path;
}
#endif


#if 0
/* path -> pairs */
static List_T
trim_short_end3_exons (bool *trim3p, List_T path,
#ifdef WASTE
		       Pairpool_T pairpool,
#endif
		       int minendexon) {
  List_T pairs, exon, pairptr;
  Pair_T pair;
  int exon_nmatches, exon_nmismatches;

  debug8(printf("Starting trim_short_end3_exons\n"));
  debug8(Pair_dump_list(path,true));

  /* Handle last exon */
  if (path == NULL) {
    *trim3p = false;
    return (List_T) NULL;
  } else {
    pair = path->first;
  }

  exon = (List_T) NULL;
  exon_nmatches = exon_nmismatches = 0;
  while (path != NULL && !pair->gapp) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
#ifdef WASTE
    exon = Pairpool_push_existing(exon,pairpool,pair);
#else
    exon = List_push_existing(exon,pairptr);
#endif
    if (pair->gapp == true) {
      /* Skip */
    } else if (pair->comp == MISMATCH_COMP || pair->comp == INDEL_COMP) {
      exon_nmismatches++;
    } else {
      exon_nmatches++;
    }
  }

  if (exon_nmatches - exon_nmismatches >= minendexon) {
    debug8(printf("Keeping last exon of length %d\n",exon_nmatches));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;
  } else if (exon_nmatches == 0) {
    debug8(printf("Trimming last exon of length %d.  firstpair must be a gap.\n",exon_nmatches));
    path = Pairpool_pop(path,&pair); /* discard gap */
    pairs = (List_T) NULL;
    *trim3p = false;
  } else {
    debug8(printf("Trimming last exon of length %d\n",exon_nmatches));
    pairs = (List_T) NULL;
    *trim3p = true;
  }

#ifdef WASTE
  while (path != NULL) {
    path = Pairpool_pop(path,&pair);
    pairs = Pairpool_push_existing(pairs,pairpool,pair);
  }
#else
  pairs = Pairpool_transfer(pairs,path);
#endif

  debug8(printf("End of trim_short_end_exons: length = %d\n",List_length(pairs)));
  debug8(Pair_dump_list(pairs,true));
  return pairs;
}
#endif



static bool
dualbreak_p (List_T pairs) {
  Pair_T pair;

  while (pairs != NULL) {
    pair = (Pair_T) pairs->first;
    if (pair->gapp ==true && pair->queryjump > 0 && pair->genomejump > 0) {
      return true;
    }
    pairs = pairs->rest;
  }

  return false;
}


static int
dualbreak_distance_from_end (int *npairs, int *totaljump, List_T pairs) {
  Pair_T pair;
  int nmatches, nmismatches;

  /* Handle first pair */
  if (pairs == NULL) {
    *totaljump = 0;
    return 0;
  } else {
    pair = pairs->first;
  }

  nmatches = nmismatches = 0;
  *npairs = 0;
  while (pairs != NULL && (pair->gapp == false || pair->queryjump == 0 || pair->genomejump == 0)) {
    if (pair->gapp == true) {
      /* Skip */
    } else if (pair->comp == MISMATCH_COMP || pair->comp == INDEL_COMP) {
      nmismatches++;
    } else {
      nmatches++;
    }

    pairs = pairs->rest;
    if (pairs != NULL) {
      pair = (Pair_T) pairs->first;
    }
    *npairs += 1;
  }

  if (pair->gapp == true && pair->queryjump > 0 && pair->genomejump > 0) {
    *npairs += 1;		/* trim gap */
    *totaljump = DUALBREAK_QUERYJUMP_FACTOR * pair->queryjump;
  } else {
    *totaljump = 0;
  }

  return nmatches - nmismatches;
}


static List_T
trim_npairs (List_T pairs, int npairs) {
  int i;
  Pair_T pair;

  for (i = 0; i < npairs; i++) {
    pairs = Pairpool_pop(pairs,&pair);
  }
  return pairs;
}


static bool
enough_matches (int matches, int genomejump
#if 0
		, double donor_prob, double acceptor_prob
#endif
		) {
#if 1
  if (genomejump > 100000) {
    return (matches >= 10) ? true : false;
  } else if (genomejump > 32000) {
    return (matches >= 9) ? true : false;
  } else if (genomejump > 8000) {
    return (matches >= 8) ? true : false;
  } else if (genomejump > 2000) {
    return (matches >= 7) ? true : false;
  } else {
    return (matches >= 6) ? true : false;
  }
#else
  double prob, prob_threshold;

  prob = 1 - pow(1.0-pow(4.0,(double) -matches),(double) genomejump);
  debug3(printf("Probability of exon of length %d with intron of length %d is %g\n",
		 matches,genomejump,prob));

#if 0
  prob_threshold = 1.0 - (1.0 - donor_prob)*(1.0 - acceptor_prob);
  debug3(printf("  Comparing with probability of splice %f and %f => %f\n",
		 donor_prob,acceptor_prob,prob_threshold));
#endif

  if (prob < 0.10) {
    return true;
  } else {
    return false;
  }
#endif
}


static bool
canonicalp (bool knowngapp, char comp, int cdna_direction) {
  if (knowngapp) {
    return true;

  } else if (cdna_direction > 0) {
    if (comp == FWD_CANONICAL_INTRON_COMP || comp == FWD_GCAG_INTRON_COMP || comp == FWD_ATAC_INTRON_COMP) {
      return true;
    } else {
      return false;
    }
  } else if (cdna_direction < 0) {
    if (comp == REV_CANONICAL_INTRON_COMP || comp == REV_GCAG_INTRON_COMP || comp == REV_ATAC_INTRON_COMP) {
      return true;
    } else {
      return false;
    }
  } else {
#if 0
    /* Too much freedom.  Also, ambig_splicetypes depend on cdna_direction to be known. */
    if (comp == FWD_CANONICAL_INTRON_COMP || comp == FWD_GCAG_INTRON_COMP || comp == FWD_ATAC_INTRON_COMP ||
	comp == REV_CANONICAL_INTRON_COMP || comp == REV_GCAG_INTRON_COMP || comp == REV_ATAC_INTRON_COMP) {
      return true;
    } else {
      return false;
    }
#else
    return false;
#endif
  }
}


/* Copied from stage1hr.c */
static int
sufficient_splice_prob_local (int support, int nmismatches, double distal_spliceprob
#if 0
			      , double medial_spliceprob
#endif
			      ) {
  support -= 2*nmismatches;
  if (support < 0) {
    return 0;
  } else if (support < 7) {
    return (distal_spliceprob > 0.95);
  } else if (support < 11) {
    return (distal_spliceprob > 0.90);
  } else if (support < 15) {
    return (distal_spliceprob > 0.85);
  } else if (support < 19) {
    return (distal_spliceprob > 0.50);
  } else {
    return true;
  }
}



static int
exon_length_5 (List_T pairs) {
  int exon_length = 0;
  List_T p;

  p = pairs;
  while (p != NULL && ((Pair_T) p->first)->gapp == false) {
    exon_length++;
    p = p->rest;
  }

  if (p == NULL) {
    debug13(printf("no intron found, so exon_length_5 = %d\n",END_SPLICESITE_EXON_LENGTH));
    return END_SPLICESITE_EXON_LENGTH;
  } else {
    debug13(printf("intron found, with exon_length_5 = %d\n",exon_length));
    return exon_length;
  }
}


static int
exon_length_3 (List_T path) {
  int exon_length = 0;
  List_T p;

  p = path;
  while (p != NULL && ((Pair_T) p->first)->gapp == false) {
    exon_length++;
    p = p->rest;
  }

  if (p == NULL) {
    debug13(printf("no intron found, so exon_length_3 = %d\n",END_SPLICESITE_EXON_LENGTH));
    return END_SPLICESITE_EXON_LENGTH;
  } else {
    debug13(printf("intron found, with exon_length_3 = %d\n",exon_length));
    return exon_length;
  }
}


/* Also handles case where novelsplicingp == false */
/* pairs -> path */
static List_T
trim_noncanonical_end5_exons (bool *trim5p, int *ambig_end_length_5, Splicetype_T *ambig_splicetype_5,
			      List_T pairs, int paired_favor_mode, int zero_offset,
			      int querylength, int watsonp, int cdna_direction, int maxintronlen
#ifdef WASTE
			      , Pairpool_T pairpool
#endif
			      ) {
  List_T path, exon, pairptr, p;
  Pair_T pair, medial, distal;
  int nmatches = 0, nmismatches = -1 /* because of the gap */, i;
  bool nearindelp = false, nearmismatchp = false, is_canonical;
  double medial_prob;
#ifdef USE_BINGO
  int insertlength;
  bool bingop = false;
#endif


  debug3(printf("Starting trim_noncanonical_end5_exons\n"));

  /* Handle first exon */
  if (pairs == NULL) {
    *trim5p = false;
    return (List_T) NULL;
  } else {
    pair = pairs->first;
#ifdef USE_BINGO
    if (paired_favor_mode < 0) {
      insertlength = pair->genomepos + querylength - zero_offset;
      debug3(printf("*** 5' insertlength %d\n",insertlength));
      if (insertlength > expected_pairlength - pairlength_deviation &&
	  insertlength < expected_pairlength + pairlength_deviation) {
	bingop = true;
      }
    }
#endif
  }

  exon = (List_T) NULL;
  while (pairs != NULL && !pair->gapp) {
    pairptr = pairs;
    pairs = Pairpool_pop(pairs,&pair);
    if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
      nmatches++;
    } else {
      nmismatches++;
    }
#ifdef WASTE
    exon = Pairpool_push_existing(exon,pairpool,pair);
#else
    exon = List_push_existing(exon,pairptr);
#endif
  }
  debug3(Pair_dump_list(exon,true));

  /* Do not alter the variable pair, which holds the gap */
  for (p = pairs, i = 0; p != NULL && i < NEARBY_INDEL; p = List_next(p), i++) {
    medial = (Pair_T) p->first;
    if (medial->comp == MATCH_COMP || medial->comp == DYNPROG_MATCH_COMP || medial->comp == AMBIGUOUS_COMP) {
      /* Skip */
    } else if (medial->comp == INDEL_COMP) {
      debug3(printf("Saw indel medial to 5' end intron\n"));
      nearindelp = true;
    } else {
      debug3(printf("Saw mismatch %c medial to 5' end intron\n",medial->comp));
      nearmismatchp = true;
    }
  }

  /* Do not alter the variable pair, which holds the gap */
  if (exon != NULL) {
    /* Skip first pair of exon, which holds the gap */
    for (p = List_next(exon), i = 0; p != NULL && i < NEARBY_INDEL; p = List_next(p), i++) {
      distal = (Pair_T) p->first;
      if (distal->comp == MATCH_COMP || distal->comp == DYNPROG_MATCH_COMP || distal->comp == AMBIGUOUS_COMP) {
	/* Skip */
      } else if (distal->comp == INDEL_COMP) {
	debug3(printf("Saw indel distal to 5' end intron\n"));
	nearindelp = true;
      } else {
	debug3(printf("Saw mismatch %c distal to 5' end intron\n",distal->comp));
	nearmismatchp = true;
      }
    }
  }


  if (nearindelp == true) {
    if (pair->donor_prob >= 0.90) {
      /* nmismatches += 1; */
    } else if (pair->donor_prob >= 0.80) {
      nmismatches += 1;
    } else {
      nmismatches += 3;
    }
    if (pair->acceptor_prob >= 0.90) {
      /* nmismatches += 1; */
    } else if (pair->acceptor_prob >= 0.80) {
      nmismatches += 1;
    } else {
      nmismatches += 3;
    }
  }

  if ((is_canonical = canonicalp(pair->knowngapp,pair->comp,cdna_direction)) == false) {
    nmismatches += 2;
  }

  debug3(printf("nmatches %d, nmismatches %d, genomejump %d\n",nmatches,nmismatches,pair->genomejump));
  if (pairs == NULL) {
    debug3(printf("No 5' exon\n"));
    path = exon;
    *trim5p = false;

  } else if (exon == NULL) {
    debug3(printf("No 5' exon\n"));
    path = exon;
    *trim5p = false;

  } else if (pair->knowngapp == true && nmismatches == 0) {
    debug3(printf("Intron is known and no mismatches, so keeping it\n"));
    path = exon;		/* exon already has the gap */
    *trim5p = false;

  } else if (exon->rest != NULL && ((Pair_T) exon->rest->first)->disallowedp == true) {
    debug3(printf("Intron is disallowed, so trimming it\n"));
    path = (List_T) NULL;
    *trim5p = true;

  } else if (List_length(exon) - 1 > List_length(pairs)) {
    /* Subtract 1 because gap is included in exon */
    debug3(printf("Exon is more than halfway across %d - 1 > %d, so keeping it\n",List_length(exon),List_length(pairs)));
    path = exon;		/* exon already has the gap */
    *trim5p = false;

  } else if (pair->genomejump > maxintronlen) {
    debug3(printf("Intron length %d is too long, so trimming it\n",pair->genomejump));
    path = (List_T) NULL;
    *trim5p = true;

#ifdef USE_BINGO
  } else if (bingop == true && is_canonical == true && nmismatches <= 1) {
    debug3(printf("bingo is true and canonical and nmismatches %d <= 1, so keeping it\n",nmismatches));
    path = exon;
    *trim5p = false;
#endif

  } else if (nearindelp == true && nmatches < INDEL_SPLICE_ENDLENGTH) {
    debug3(printf("near indel with nmatches %d too low, so trimming it\n",nmatches));
    path = (List_T) NULL;
    *trim5p = true;

  } else if (enough_matches(nmatches-nmismatches,pair->genomejump/*,pair->donor_prob,pair->acceptor_prob*/) == false) {
    debug3(printf("nmatches %d - nmismatches %d not enough for genomejump %d, so trimming it\n",
		 nmatches,nmismatches,pair->genomejump));
    path = (List_T) NULL;
    *trim5p = true;

#if 0
  } else if (nmatches - nmismatches >= NONCANONICAL_ACCEPT && (pair->donor_prob >= 0.90 || pair->acceptor_prob >= 0.90)) {
    debug3(printf("Exon has nmatches %d - nmismatches %d >= %d and probs %f and %f, so keeping it\n",
		   nmatches,nmismatches,NONCANONICAL_ACCEPT,pair->donor_prob,pair->acceptor_prob));
    path = exon;		/* exon already has the gap */
    *trim5p = false;

  } else if (nmatches >= NONCANONICAL_PERFECT_MATCHES && nmismatches == 0) {
    debug3(printf("Exon has perfect nmatches %d > %d, so keeping it\n",
		 nmatches,NONCANONICAL_PERFECT_MATCHES));
    path = exon;		/* exon already has the gap */
    *trim5p = false;
#endif

#if 0
  } else if (splicesites_iit != NULL && List_length(exon) < 20) {
    debug3(printf("Known splicesites and exon length %d < 20, so trimming 5' exon\n",
		 List_length(exon)));
#if 0
    pairs = Pairpool_pop(pairs,&pair); /* discard gap */
#endif
    path = (List_T) NULL;
    *trim5p = true;
#endif

  } else if (sufficient_splice_prob_local(List_length(exon),nmismatches,
					  /*distal_spliceprob*/cdna_direction >= 0 ? pair->donor_prob : pair->acceptor_prob
#if 0
					  ,/*medial_spliceprob*/cdna_direction >= 0 ? pair->acceptor_prob : pair->donor_prob
#endif
					  )) {
#ifdef GSNAP
    /* Want to keep for comparison of fwd and rev, even if probabilities are poor */
    debug3(printf("Keeping first 5' exon with %d matches and %d mismatches\n",nmatches,nmismatches));
    path = exon;		/* exon already has the gap */
    *trim5p = false;
#else
    if (pair->donor_prob >= 0.9 || pair->acceptor_prob >= 0.9) {
      debug3(printf("Keeping first 5' exon with probs %f and %f\n",pair->donor_prob,pair->acceptor_prob));
      path = exon;		/* exon already has the gap */
      *trim5p = false;
    } else {
      debug3(printf("Trimming bad canonical 5' exon: prob %f and %f\n",pair->donor_prob,pair->acceptor_prob));
      path = (List_T) NULL;
      *trim5p = true;
    }
#endif

#if 0
  } else if (canonicalp(pair->knowngapp,pair->comp,cdna_direction) == true) {
    debug3(printf("Keeping first 5' exon with %d matches and %d mismatches\n",nmatches,nmismatches));
    path = exon;		/* exon already has the gap */
    *trim5p = false;
#endif

  } else {
    debug3(printf("Trimming noncanonical 5' exon\n"));
#if 0
    pairs = Pairpool_pop(pairs,&pair); /* discard gap */
#endif

    medial_prob = (cdna_direction >= 0) ? pair->acceptor_prob : pair->donor_prob;
    if (canonicalp(pair->knowngapp,pair->comp,cdna_direction) == true &&
	medial_prob > 0.95) {

#if 0
      /* Let trim_novel_spliceends take care of this */
      /* *ambig_end_length_5 += List_length(exon); */
      *ambig_end_length_5 = ((Pair_T) pairs->first)->querypos;
      debug3(printf("Setting ambig_end_length_5 to be %d\n",*ambig_end_length_5));
      if (watsonp == true) {
	if (cdna_direction > 0) {
	  *ambig_splicetype_5 = ACCEPTOR;
	} else {
	  *ambig_splicetype_5 = ANTIDONOR;
	}
      } else {
	if (cdna_direction > 0) {
	  *ambig_splicetype_5 = ANTIACCEPTOR;
	} else {
	  *ambig_splicetype_5 = DONOR;
	}
      }
#endif

      *trim5p = false;		/* Not really, since we are trimming, but this stops further work */
    } else {
      *trim5p = true;
    }
    path = (List_T) NULL;
  }

#ifdef WASTE
  while (pairs != NULL) {
    pairs = Pairpool_pop(pairs,&pair);
    path = Pairpool_push_existing(path,pairpool,pair);

  }
#else
  path = Pairpool_transfer(path,pairs);
#endif

  debug3(printf("End of trim_noncanonical_end5_exons: length = %d\n",List_length(path)));
  debug3(Pair_dump_list(path,true));
  return path;
}



/* Also handles case where novelsplicingp == false */
/* path -> pairs */
static List_T
trim_noncanonical_end3_exons (bool *trim3p, int *ambig_end_length_3, Splicetype_T *ambig_splicetype_3,
			      List_T path, int paired_favor_mode, int zero_offset,
			      int querylength, bool watsonp, int cdna_direction,
			      int maxintronlen
#ifdef WASTE
			      , Pairpool_T pairpool
#endif
			      ) {
  List_T pairs, exon, pairptr, p;
  Pair_T pair, medial, distal;
  int nmatches = 0, nmismatches = -1 /* because of the gap */, i;
  bool nearindelp = false, nearmismatchp = false, is_canonical;
  double medial_prob;
#ifdef USE_BINGO
  int insertlength;
  bool bingop = false;
#endif


  debug3(printf("Starting trim_noncanonical_end3_exons\n"));

  /* Handle last exon */
  if (path == NULL) {
    *trim3p = false;
    return (List_T) NULL;
  } else {
    pair = path->first;
#ifdef USE_BINGO
    if (paired_favor_mode > 0) {
      insertlength = (genomiclength - pair->genomepos) + querylength - zero_offset;
      debug3(printf("*** 3' insertlength %d\n",insertlength));
      if (insertlength > expected_pairlength - pairlength_deviation &&
	  insertlength < expected_pairlength + pairlength_deviation) {
	bingop = true;
      }
    }
#endif
  }

  exon = (List_T) NULL;
  while (path != NULL && !pair->gapp) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
      nmatches++;
    } else {
      nmismatches++;
    }
#ifdef WASTE
    exon = Pairpool_push_existing(exon,pairpool,pair);
#else
    exon = List_push_existing(exon,pairptr);
#endif
  }
  debug3(Pair_dump_list(exon,true));


  /* Do not alter the variable pair, which holds the gap */
  for (p = path, i = 0; p != NULL && i < NEARBY_INDEL; p = List_next(p), i++) {
    medial = (Pair_T) p->first;
    if (medial->comp == MATCH_COMP || medial->comp == DYNPROG_MATCH_COMP || medial->comp == AMBIGUOUS_COMP) {
      /* Skip */
    } else if (medial->comp == INDEL_COMP) {
      debug3(printf("Saw indel medial to 3' end intron\n"));
      nearindelp = true;
    } else {
      debug3(printf("Saw mismatch medial %c to 3' end intron\n",medial->comp));
      nearmismatchp = true;
    }
  }

  /* Do not alter the variable pair, which holds the gap */
  if (exon != NULL) {
    /* Skip first pair of exon, which holds the gap */
    for (p = List_next(exon), i = 0; p != NULL && i < NEARBY_INDEL; p = List_next(p), i++) {
      distal = (Pair_T) p->first;
      if (distal->comp == MATCH_COMP || distal->comp == DYNPROG_MATCH_COMP || distal->comp == AMBIGUOUS_COMP) {
	/* Skip */
      } else if (distal->comp == INDEL_COMP) {
	debug3(printf("Saw indel distal to 3' end intron\n"));
	nearindelp = true;
      } else {
	debug3(printf("Saw mismatch %c distal to 3' end intron\n",distal->comp));
	nearmismatchp = true;
      }
    }
  }

  if (nearindelp == true) {
    if (pair->donor_prob >= 0.90) {
      /* nmismatches += 1; */
    } else if (pair->donor_prob >= 0.80) {
      nmismatches += 1;
    } else {
      nmismatches += 3;
    }
    if (pair->acceptor_prob >= 0.90) {
      /* nmismatches += 1; */
    } else if (pair->acceptor_prob >= 0.80) {
      nmismatches += 1;
    } else {
      nmismatches += 3;
    }
  }

  if ((is_canonical = canonicalp(pair->knowngapp,pair->comp,cdna_direction)) == false) {
    nmismatches += 2;
  }


  debug3(printf("nmatches %d, nmismatches %d, genomejump %d\n",nmatches,nmismatches,pair->genomejump));
  if (path == NULL) {
    debug3(printf("No 3' exon\n"));
    pairs = exon;
    *trim3p = false;

  } else if (exon == NULL) {
    debug3(printf("No 3' exon\n"));
    pairs = exon;
    *trim3p = false;

  } else if (pair->knowngapp == true && nmismatches == 0) {
    debug3(printf("Intron is known and no mismatches, so keeping it\n"));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;

  } else if (exon->rest != NULL && ((Pair_T) exon->rest->first)->disallowedp == true) {
    debug3(printf("Intron is disallowed, so trimming it\n"));
    pairs = (List_T) NULL;
    *trim3p = true;

  } else if (List_length(exon) - 1 > List_length(path)) {
    /* Subtract 1 because gap is included in exon */
    debug3(printf("Exon is more than halfway across %d - 1 > %d, so keeping it\n",List_length(exon),List_length(path)));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;

  } else if (pair->genomejump > maxintronlen) {
    debug3(printf("Intron length %d is too long, so trimming it\n",pair->genomejump));
    pairs = (List_T) NULL;
    *trim3p = true;

#ifdef USE_BINGO
  } else if (bingop == true && is_canonical == true && nmismatches <= 1) {
    debug3(printf("bingo is true and canonical and nmismatches %d <= 1, so keeping it\n",nmismatches));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;
#endif

  } else if (nearindelp == true && nmatches < INDEL_SPLICE_ENDLENGTH) {
    debug3(printf("near indel with nmatches %d too low, so trimming it\n",nmatches));
    pairs = (List_T) NULL;
    *trim3p = true;

  } else if (enough_matches(nmatches-nmismatches,pair->genomejump/*,pair->donor_prob,pair->acceptor_prob*/) == false) {
    debug3(printf("nmatches %d - nmismatches %d not enough for genomejump %d, so trimming it\n",
		 nmatches,nmismatches,pair->genomejump));
    pairs = (List_T) NULL;
    *trim3p = true;

#if 0
  } else if (nmatches - nmismatches >= NONCANONICAL_ACCEPT) {
    debug3(printf("Exon has nmatches %d - nmismatches %d > %d and probs %f and %f, so keeping it\n",
		   nmatches,nmismatches,NONCANONICAL_ACCEPT,pair->donor_prob,pair->acceptor_prob));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;

  } else if (nmatches >= NONCANONICAL_PERFECT_MATCHES && nmismatches == 0) {
    debug3(printf("Exon has perfect nmatches %d > %d, so keeping it\n",
		 nmatches,NONCANONICAL_PERFECT_MATCHES));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;
#endif

#if 0
  } else if (splicesites_iit != NULL && List_length(exon) < 20) {
    debug3(printf("Known splicesites and exon length %d < 20, so trimming 3' exon\n",
		 List_length(exon)));
#if 0
    path = Pairpool_pop(path,&pair); /* discard gap */
#endif
    pairs = (List_T) NULL;
    *trim3p = true;
#endif

  } else if (sufficient_splice_prob_local(List_length(exon),nmismatches,
					  /*distal_spliceprob*/cdna_direction >= 0 ? pair->acceptor_prob : pair->donor_prob
#if 0
					  ,/*medial_spliceprob*/cdna_direction >= 0 ? pair->donor_prob : pair->acceptor_prob
#endif
					  )) {
#ifdef GSNAP
    /* Want to keep for comparison of fwd and rev, even if probabilities are poor */
    debug3(printf("Keeping last 3' exon with %d matches and %d mismatches\n",nmatches,nmismatches));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;
#else
    if (pair->donor_prob >= 0.9 || pair->acceptor_prob >= 0.9) {
      debug3(printf("Keeping last 3' exon with probs %f and %f\n",pair->donor_prob,pair->acceptor_prob));
      pairs = exon;		/* exon already has the gap */
      *trim3p = false;
    } else {
      debug3(printf("Trimming bad canonical 3' exon: prob %f and %f\n",pair->donor_prob,pair->acceptor_prob));
      pairs = (List_T) NULL;
      *trim3p = true;
    }
#endif

#if 0
  } else if (canonicalp(pair->knowngapp,pair->comp,cdna_direction) == true) {
    debug3(printf("Keeping last 3' exon with %d matches and %d mismatches\n",nmatches,nmismatches));
    pairs = exon;		/* exon already has the gap */
    *trim3p = false;
#endif

  } else {
    debug3(printf("Trimming noncanonical 3' exon\n"));
#if 0
    path = Pairpool_pop(path,&pair); /* discard gap */
#endif

    medial_prob = (cdna_direction >= 0) ? pair->donor_prob : pair->acceptor_prob;
    if (canonicalp(pair->knowngapp,pair->comp,cdna_direction) == true &&
	medial_prob > 0.95) {

#if 0
      /* Let trim_novel_spliceends take care of this */
      /* *ambig_end_length_3 += List_length(exon); */
      *ambig_end_length_3 = (querylength - 1) - ((Pair_T) path->first)->querypos;
      debug3(printf("Setting ambig_end_length_3 to be %d\n",*ambig_end_length_3));
      if (watsonp == true) {
	if (cdna_direction > 0) {
	  *ambig_splicetype_3 = DONOR;
	} else {
	  *ambig_splicetype_3 = ANTIACCEPTOR;
	}
      } else {
	if (cdna_direction > 0) {
	  *ambig_splicetype_3 = ANTIDONOR;
	} else {
	  *ambig_splicetype_3 = ACCEPTOR;
	}
      }
#endif

      *trim3p = false;		/* Not really, since we are trimming, but this stops further work */
    } else {
      *trim3p = true;
    }
    pairs = (List_T) NULL;
  }

#ifdef WASTE
  while (path != NULL) {
    path = Pairpool_pop(path,&pair);
    pairs = Pairpool_push_existing(pairs,pairpool,pair);
  }
#else
  pairs = Pairpool_transfer(pairs,path);
#endif

  debug3(printf("End of trim_noncanonical_end3_exons: length = %d\n",List_length(pairs)));
  debug3(Pair_dump_list(pairs,true));
  return pairs;
}




/* This procedure fills in introns and replaces non-canonical introns
   with deletions, so it should be called after all dynamic
   programming procedures */
static List_T 
fill_in_gaps (List_T path, Pairpool_T pairpool, char *queryseq_ptr, 
#ifdef PMAP
	      char *queryaaseq_ptr,
#endif
	      Univcoord_T chroffset, Univcoord_T chrhigh,
	      int cdna_direction, int sensedir, bool watsonp, int ngap, bool diagnosticp) {
  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;

  int leftquerypos, leftgenomepos, rightquerypos, rightgenomepos,
    introntype, intronlength, genomicpos, k;
  char comp;
  char left1, left2, right2, right1, left1_alt, left2_alt, right2_alt, right1_alt, c1, c2, c2_alt;
  bool intronp, introntype_found_p;


  if (path == NULL) {
    return (List_T) NULL;
  } else {
    pair = path->first;
  }

  while (path != NULL && ((Pair_T) path->first)->gapp == true) {
    /* Gap at beginning of alignment.  Can occur after smoothing. */
    debug7(printf("Gap %p at beginning of alignment\n",pair));
    path = Pairpool_pop(path,&pair);
  }

  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);

#ifdef PMAP
    if (pair->cdna == BACKTRANSLATE_CHAR) {
      pair->cdna = 'N';
    }
#endif
    if (pair->comp == INDEL_COMP || pair->comp == SHORTGAP_COMP) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->gapp == false) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (path == NULL) {
      /* Gap at end of alignment.  Can occur after smoothing. */
      debug7(printf("Gap at end of alignment\n"));

    } else if (pairs == NULL) {
      /* Gap at beginning of alignment.  Skip. */
      debug7(printf("Gap at beginning of alignment\n"));

    } else {

      /* Discard gap; do not push */
      leftpair = path->first;
      rightpair = pairs->first;

      if (output_sam_p == false && pair->comp == DUALBREAK_COMP) {
#ifdef PMAP
	pairs = add_dualbreak(pairs,queryseq_ptr,queryaaseq_ptr,
			      chroffset,chrhigh,cdna_direction,watsonp,
			      leftpair,rightpair,pairpool,ngap,diagnosticp);
#else
	pairs = add_dualbreak(pairs,queryseq_ptr,
			      chroffset,chrhigh,cdna_direction,watsonp,
			      leftpair,rightpair,pairpool,ngap,diagnosticp);
#endif
      } else {

	leftquerypos = leftpair->querypos;
	leftgenomepos = leftpair->genomepos;
	if (leftpair->cdna == ' ') leftquerypos--;
	if (leftpair->genome == ' ') leftgenomepos--;
	rightquerypos = rightpair->querypos;
	rightgenomepos = rightpair->genomepos;
	intronlength = rightgenomepos - leftgenomepos - 1;

	introntype_found_p = false;
	if (pair->knowngapp == true) {
	  debug7(printf("known intron is true, so an intron\n"));
	  intronp = true;
	} else if (splicingp == false) {
	  debug7(printf("splicingp is false, so not an intron\n"));
	  intronp = false;
#if 0
	} else if (sensedir == SENSE_NULL) {
	  /* Can lead to very large deletions */
	  debug7(printf("sensedir == SENSE_NULL, so not an intron\n"));
	  intronp = false;
#endif
	} else if (intronlength < min_intronlength) {
	  debug7(printf("intronlength %d < min_intronlength %d, so not an intron\n",
			intronlength,min_intronlength));
	  intronp = false;
	} else if (intronlength >= max_deletionlength) {
	  debug7(printf("intronlength %d >= max_deletionlength %d, so an intron\n",
			intronlength,max_deletionlength));
	  intronp = true;
	} else {
	  /* Possible short intron */
	  left1 = get_genomic_nt(&left1_alt,leftgenomepos+1,chroffset,chrhigh,watsonp);
	  left2 = get_genomic_nt(&left2_alt,leftgenomepos+2,chroffset,chrhigh,watsonp);
	  right2 = get_genomic_nt(&right2_alt,rightgenomepos-2,chroffset,chrhigh,watsonp);
	  right1 = get_genomic_nt(&right1_alt,rightgenomepos-1,chroffset,chrhigh,watsonp);
	  debug7(printf("  Dinucleotides are %c%c..%c%c\n",left1,left2,right2,right1));
	  introntype = Intron_type(left1,left2,right2,right1,
				   left1_alt,left2_alt,right2_alt,right1_alt,
				   cdna_direction);
	  introntype_found_p = true;
	  debug7(printf("  Introntype at %u..%u is %s (cdna_direction %d)\n",
			leftgenomepos,rightgenomepos,Intron_type_string(introntype),cdna_direction));

	  if ((cdna_direction >= 0 && introntype == GTAG_FWD)
#ifndef PMAP
	      || (cdna_direction <= 0 && introntype == GTAG_REV)
#endif
	      ) {
	    intronp = true;
	  } else {
	    intronp = false;
	  }
	}

	if (intronp == false) {
	  debug7(printf("  Gap is not an intron (intronlength %d).  Adding pairs from %d downto %d\n",
			intronlength,rightgenomepos-1,leftgenomepos+1));
	  for (k = rightquerypos - 1; k > leftquerypos; --k) {
#ifdef PMAP
	    c1 = Dynprog_codon_char(queryaaseq_ptr[k/3],k%3);
#else
	    c1 = queryseq_ptr[k];
#endif
	    pairs = Pairpool_push(pairs,pairpool,k,rightgenomepos,c1,INDEL_COMP,' ',' ',/*dynprogindex*/0);
	  }

	  for (genomicpos = rightgenomepos - 1; genomicpos > leftgenomepos; --genomicpos) {
	    c2 = get_genomic_nt(&c2_alt,genomicpos,chroffset,chrhigh,watsonp);
	    pairs = Pairpool_push(pairs,pairpool,rightquerypos,genomicpos,' ',/*comp*/SHORTGAP_COMP,c2,c2_alt,
				  /*dynprogindex*/0);
	  }
	} else {
	  if (introntype_found_p == false) {
	    left1 = get_genomic_nt(&left1_alt,leftgenomepos+1,chroffset,chrhigh,watsonp);
	    left2 = get_genomic_nt(&left2_alt,leftgenomepos+2,chroffset,chrhigh,watsonp);
	    right2 = get_genomic_nt(&right2_alt,rightgenomepos-2,chroffset,chrhigh,watsonp);
	    right1 = get_genomic_nt(&right1_alt,rightgenomepos-1,chroffset,chrhigh,watsonp);
	    debug7(printf("  Dinucleotides are %c%c..%c%c\n",left1,left2,right2,right1));
	    introntype = Intron_type(left1,left2,right2,right1,
				     left1_alt,left2_alt,right2_alt,right1_alt,
				     cdna_direction);
	    debug7(printf("  Introntype at %u..%u is %s (cdna_direction %d)\n",
			  leftgenomepos,rightgenomepos,Intron_type_string(introntype),cdna_direction));
	  }

	  if (cdna_direction == 0) {
	    /* cdna_direction of 0 should happen only from Stage3_merge_local_splice */
	    comp = NONINTRON_COMP;
	  } else {
	    comp = pair->comp;
	  }

	  debug7(printf("Adding an intron at %d..%d, currently of type %c, with introntype %d\n",
			leftpair->querypos,rightpair->querypos,comp,introntype));
	  pairs = add_intron(pairs,chroffset,chrhigh,leftpair,rightpair,comp,introntype,ngap,
			     watsonp,pairpool);
	}
      }
    }
  }

  debug7(printf("Final length: %d\n",List_length(pairs)));

  return pairs;
}

static List_T 
add_queryseq_offset (List_T path, int queryseq_offset
#ifdef WASTE
		     , Pairpool_T pairpool
#endif
		     ) {
  List_T pairs = NULL, pairptr;
  Pair_T pair;

  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    /* Previously excluded cases where pair->gapp was true, but this failed on chimeric paths */
    /* Now excluding again, because we are running this before chimera detection and merging */
    if (pair->gapp == false) {
      pair->querypos += queryseq_offset;
    }
#ifdef WASTE
    pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
    pairs = List_push_existing(pairs,pairptr);
#endif
  }

  return pairs;
}


static void
add_skiplength (List_T pairs, int skiplength) {
  List_T p;
  Pair_T pair;

  for (p = pairs; p != NULL; p = p->rest) {
    pair = (Pair_T) p->first;
    if (pair->gapp == true) {
      /* Skip */
    } else if (pair->querypos >= HALFLEN) {
      pair->querypos += skiplength;
    }
  }
  return;
}


static void
Stage3_free_pairarray (T *old) {
  if ((*old)->pairarray_freeable_p == true) {
    FREE_OUT((*old)->pairarray);
    (*old)->pairarray_freeable_p = false;
  }
  return;
}


/* Does not alter pairs, except for adding subseq_offset to querypos,
   in case we need to re-compute alignment for chimera */
static struct Pair_T *
make_pairarray (int *npairs, List_T *pairs, int cdna_direction, int sensedir, bool watsonp,
		Pairpool_T pairpool, char *queryseq_ptr,
#ifdef PMAP
		char *queryaaseq_ptr,
#endif
		Univcoord_T chroffset, Univcoord_T chrhigh,
		int ngap, int subseq_offset, int skiplength, bool diagnosticp) {
  struct Pair_T *pairarray;
  List_T printpairs, printpath, path, p;
  Pair_T oldpair, newpair;

  
  printpairs = Pairpool_copy(*pairs,pairpool);

  printpath = List_reverse(printpairs);
#ifdef PMAP
  printpairs = fill_in_gaps(printpath,pairpool,queryseq_ptr,queryaaseq_ptr,
			    chroffset,chrhigh,
			    cdna_direction,sensedir,watsonp,ngap,diagnosticp);
#else
  printpairs = fill_in_gaps(printpath,pairpool,queryseq_ptr,
			    chroffset,chrhigh,
			    cdna_direction,sensedir,watsonp,ngap,diagnosticp);
#endif

  if (subseq_offset != 0) {
    path = List_reverse(*pairs);
#ifdef WASTE
    *pairs = add_queryseq_offset(path,subseq_offset,pairpool);
#else
    *pairs = add_queryseq_offset(path,subseq_offset);
#endif
    
    printpath = List_reverse(printpairs);
#ifdef WASTE
    printpairs = add_queryseq_offset(printpath,subseq_offset,pairpool);
#else
    printpairs = add_queryseq_offset(printpath,subseq_offset);
#endif
  }

  if (skiplength != 0) {
    add_skiplength(*pairs,skiplength);
    add_skiplength(printpairs,skiplength);
  }

  if ((*npairs = List_length(printpairs)) == 0) {
    return (struct Pair_T *) NULL;
  } else {
    /* Used to be Pair_block_copy */
    newpair = pairarray = (struct Pair_T *) MALLOC_OUT(*npairs*sizeof(struct Pair_T));
    for (p = printpairs; p != NULL; p = p->rest) {
      oldpair = (Pair_T) p->first;
      memcpy(newpair++,oldpair,sizeof(struct Pair_T));
    }

    /* No need to free newpairs, since they belong to pairpool */
    Pair_set_genomepos(pairarray,*npairs,chroffset,chrhigh,watsonp);

    return pairarray;
  }
}


/* Does not alter pairs, except for adding subseq_offset to querypos,
   in case we need to re-compute alignment for chimera */
static void
make_pairarray_merge (T this_left, T this_right, int cdna_direction, int sensedir, bool watsonp,
		      Pairpool_T pairpool, char *queryseq_ptr,
#ifdef PMAP
		      char *queryaaseq_ptr,
#endif
		      Univcoord_T chroffset, Univcoord_T chrhigh,
		      int ngap, int subseq_offset, int skiplength, bool diagnosticp,
		      bool new_gap_p) {
  struct Pair_T *pairarray;
  List_T printpairs, printpath, path, p;
  Pair_T oldpair, newpair;
  int ncanonical, nsemicanonical, nnoncanonical;
  double min_splice_prob;

  this_left->pairs = List_append(this_left->pairs,this_right->pairs);
  this_right->pairs = (List_T) NULL;

  if (new_gap_p == true) {
    path = List_reverse(this_left->pairs);
    this_left->pairs = assign_gap_types(path,cdna_direction,this_left->watsonp,queryseq_ptr,
					this_left->chrnum,this_left->chroffset,this_left->chrhigh,
					pairpool);
  }

  debug10(Pair_dump_list(this_left->pairs,true));
      
  this_left->cdna_direction = cdna_direction;
  Pair_fracidentity(&this_left->matches,&this_left->unknowns,&this_left->mismatches,
		    &this_left->qopens,&this_left->qindels,&this_left->topens,&this_left->tindels,
		    &ncanonical,&nsemicanonical,&nnoncanonical,
		    &min_splice_prob,this_left->pairs,this_left->cdna_direction);
  
  printpairs = Pairpool_copy(this_left->pairs,pairpool);

  printpath = List_reverse(printpairs);
#ifdef PMAP
  printpairs = fill_in_gaps(printpath,pairpool,queryseq_ptr,queryaaseq_ptr,
			    chroffset,chrhigh,
			    cdna_direction,sensedir,watsonp,ngap,diagnosticp);
#else
  printpairs = fill_in_gaps(printpath,pairpool,queryseq_ptr,
			    chroffset,chrhigh,
			    cdna_direction,sensedir,watsonp,ngap,diagnosticp);
#endif

  if (subseq_offset != 0) {
    path = List_reverse(this_left->pairs);
#ifdef WASTE
    this_left->pairs = add_queryseq_offset(path,subseq_offset,pairpool);
#else
    this_left->pairs = add_queryseq_offset(path,subseq_offset);
#endif
    
    printpath = List_reverse(printpairs);
#ifdef WASTE
    printpairs = add_queryseq_offset(printpath,subseq_offset,pairpool);
#else
    printpairs = add_queryseq_offset(printpath,subseq_offset);
#endif
  }

  if (skiplength != 0) {
    add_skiplength(this_left->pairs,skiplength);
    add_skiplength(printpairs,skiplength);
  }

  Stage3_free_pairarray(&this_left);
  if ((this_left->npairs = List_length(printpairs)) == 0) {
    this_left->pairarray = (struct Pair_T *) NULL;
    this_left->pairarray_freeable_p = false;
  } else {
    /* Used to be Pair_block_copy */
    newpair = pairarray = (struct Pair_T *) MALLOC_OUT(this_left->npairs*sizeof(struct Pair_T));
    for (p = printpairs; p != NULL; p = p->rest) {
      oldpair = (Pair_T) p->first;
      memcpy(newpair++,oldpair,sizeof(struct Pair_T));
    }

    /* No need to free newpairs, since they belong to pairpool */
    Pair_set_genomepos(pairarray,this_left->npairs,chroffset,chrhigh,watsonp);
    this_left->pairarray = pairarray;
    this_left->pairarray_freeable_p = true;
  }

  return;
}


static void
make_pairarrays_chimera (T this_left, T this_right,
#ifdef PMAP
			 char *queryaaseq_ptr,
#endif
			 Sequence_T queryseq, Pairpool_T pairpool, int gaplength, int ngap) {
  List_T printpairs_left, printpath_left, printpairs_right, printpath_right, p;
  Pair_T oldpair, newpair;
  char *queryseq_ptr;
  int newnpairs;
  int ncanonical, nsemicanonical, nnoncanonical;
  double min_splice_prob;

  queryseq_ptr = Sequence_fullpointer(queryseq);

  
  /* Revise statistics */
  Pair_fracidentity(&this_left->matches,&this_left->unknowns,&this_left->mismatches,
		    &this_left->qopens,&this_left->qindels,&this_left->topens,&this_left->tindels,
		    &ncanonical,&nsemicanonical,&nnoncanonical,
		    &min_splice_prob,this_left->pairs,this_left->cdna_direction);

  Pair_fracidentity(&this_right->matches,&this_right->unknowns,&this_right->mismatches,
		    &this_right->qopens,&this_right->qindels,&this_right->topens,&this_right->tindels,
		    &ncanonical,&nsemicanonical,&nnoncanonical,
		    &min_splice_prob,this_right->pairs,this_right->cdna_direction);


  printpairs_left = Pairpool_copy(this_left->pairs,pairpool);
  printpath_left = List_reverse(printpairs_left);
#ifdef PMAP
  printpairs_left = fill_in_gaps(printpath_left,pairpool,queryseq_ptr,queryaaseq_ptr,
				 this_left->chroffset,this_left->chrhigh,
				 this_left->cdna_direction,this_left->sensedir,this_left->watsonp,
				 ngap,/*diagnosticp*/false);
#else
  printpairs_left = fill_in_gaps(printpath_left,pairpool,queryseq_ptr,
				 this_left->chroffset,this_left->chrhigh,
				 this_left->cdna_direction,this_left->sensedir,this_left->watsonp,
				 ngap,/*diagnosticp*/false);
#endif

  printpairs_right = Pairpool_copy(this_right->pairs,pairpool);
  printpath_right = List_reverse(printpairs_right);
#ifdef PMAP
  printpairs_right = fill_in_gaps(printpath_right,pairpool,queryseq_ptr,queryaaseq_ptr,
				  this_right->chroffset,this_right->chrhigh,
				  this_right->cdna_direction,this_right->sensedir,this_right->watsonp,
				  ngap,/*diagnosticp*/false);
#else
  printpairs_right = fill_in_gaps(printpath_right,pairpool,queryseq_ptr,
				  this_right->chroffset,this_right->chrhigh,
				  this_right->cdna_direction,this_right->sensedir,this_right->watsonp,
				  ngap,/*diagnosticp*/false);
#endif


  /* Do not use subseq_offset or skiplength for chimeras, since we are
     working on the original queryseq, not querysubseq */

  this_left->npairs = List_length(printpairs_left);
  this_right->npairs = List_length(printpairs_right);

  Stage3_free_pairarray(&this_left);
  Stage3_free_pairarray(&this_right);
  if ((newnpairs = this_left->npairs + this_right->npairs) == 0) {
    this_left->pairarray = (struct Pair_T *) NULL;
    this_right->pairarray = (struct Pair_T *) NULL;
    this_left->pairarray_freeable_p = false;
    this_right->pairarray_freeable_p = false;

  } else {
    /* Need to have a single pairarray for this_left, so we can translate protein correctly */
    newpair = this_left->pairarray = (struct Pair_T *) MALLOC_OUT((newnpairs + gaplength)*sizeof(struct Pair_T));
    this_left->pairarray_freeable_p = true;
    for (p = printpairs_left; p != NULL; p = p->rest) {
      oldpair = (Pair_T) p->first;
      memcpy(newpair++,oldpair,sizeof(struct Pair_T));
    }
    Pair_set_genomepos(this_left->pairarray,this_left->npairs,this_left->chroffset,this_left->chrhigh,
		       this_left->watsonp);

    newpair = this_right->pairarray = &(this_left->pairarray[this_left->npairs + gaplength]);
    this_right->pairarray_freeable_p = false;
    for (p = printpairs_right; p != NULL; p = p->rest) {
      oldpair = (Pair_T) p->first;
      memcpy(newpair++,oldpair,sizeof(struct Pair_T));
    }
    Pair_set_genomepos(this_right->pairarray,this_right->npairs,this_right->chroffset,this_right->chrhigh,
		       this_right->watsonp);
  }

  return;
}


	    
#define MAPQ_MAXIMUM_SCORE 40

void
Stage3_recompute_goodness (List_T stage3list) {
  T this;
  List_T p;
  int best_absmq_score;
  double total = 0.0, q;

#ifdef DEPEND_ON_QUALITY
  for (p = stage3list; p != NULL && high_quality_p == false; p = List_next(p)) {
    this = (T) List_head(p);
    if (this->trimmed_coverage > 0.80 && this->defect_rate < 0.10) {
      high_quality_p = true;
    }
  }

  /* Subtracts points for non-canonical introns */
  if (high_quality_p == true) {
    for (p = stage3list; p != NULL; p = List_next(p)) {
      this = (T) List_head(p);
      this->goodness = this->matches + MISMATCH*this->mismatches
	+ QOPEN*this->qopens + QINDEL*this->qindels + TOPEN*this->topens + TINDEL*this->tindels
	- CANONICAL_POINTS*this->noncanonical;
    }
  } else {
    for (p = stage3list; p != NULL; p = List_next(p)) {
      this = (T) List_head(p);
      this->goodness = this->matches;
    }
  }

#else
  if (stage3list != NULL) {
    /* Use the first entry to initialize best_absmq_score */
    p = stage3list;
    this = (T) List_head(p);
    best_absmq_score = this->absmq_score = this->matches - 10*this->mismatches;
    p = List_next(p);

    while (p != NULL) {
      this = (T) List_head(p);
      if ((this->absmq_score = this->matches - 10*this->mismatches) > best_absmq_score) {
	best_absmq_score = this->absmq_score;
      }
      p = List_next(p);
    }
  }

  for (p = stage3list; p != NULL; p = List_next(p)) {
    this = (T) List_head(p);
    this->absmq_score -= best_absmq_score;
    total += exp(this->absmq_score);
  }

  for (p = stage3list; p != NULL; p = List_next(p)) {
    this = (T) List_head(p);
    if ((q = 1.0 - exp(this->absmq_score) / total) < 1.0e-4 /* 10^-4.0 */) {
      this->mapq_score = 40;
    } else {
      this->mapq_score = rint(-10.0 * log10(q));
    }

    this->absmq_score += MAPQ_MAXIMUM_SCORE;
    if (this->absmq_score < 0) {
      this->absmq_score = 0;
    }

    this->goodness = this->matches + MISMATCH*this->mismatches
      + QOPEN*this->qopens + QINDEL*this->qindels + TOPEN*this->topens + TINDEL*this->tindels
      - CANONICAL_POINTS*this->noncanonical;
  }

#endif
}


void
Stage3_recompute_coverage (List_T stage3list, Sequence_T queryseq) {
  List_T p;
  T stage3;
  Pair_T start, end;
  int querypos1, querypos2;
  int trim_start, trim_end, effective_trim_start, effective_trim_end, skiplength;

  trim_start = Sequence_trim_start(queryseq);
  trim_end = Sequence_trim_end(queryseq);
  skiplength = Sequence_skiplength(queryseq);

  for (p = stage3list; p != NULL; p = List_next(p)) {
    stage3 = (T) List_head(p);
    start = &(stage3->pairarray[0]);
    end = &(stage3->pairarray[stage3->npairs - 1]);

    querypos1 = start->querypos;
    querypos2 = end->querypos;

    if (querypos2 + 1 > trim_end) {
      effective_trim_end = querypos2 + 1;
    } else {
      effective_trim_end = trim_end;
    }
    if (querypos1 < trim_start) {
      effective_trim_start = querypos1;
    } else {
      effective_trim_start = trim_start;
    }

    stage3->trimmed_coverage = (double) (querypos2 - querypos1 + 1)/(double) (trim_end - trim_start + skiplength);
  }

  return;
}


static List_T
pick_cdna_direction (int *winning_cdna_direction, int *sensedir,
		     List_T pairs_fwd, List_T pairs_rev,
		     int ncanonical_fwd, int nsemicanonical_fwd,
		     int nnoncanonical_fwd, int nbadintrons_fwd,
		     int ncanonical_rev, int nsemicanonical_rev,
		     int nnoncanonical_rev, int nbadintrons_rev,
		     double max_intron_score_fwd, double avg_donor_score_fwd, double avg_acceptor_score_fwd,
		     double max_intron_score_rev, double avg_donor_score_rev, double avg_acceptor_score_rev,
#ifdef COMPLEX_DIRECTION
		     int nmatches_fwd, int nmismatches_fwd, int nmatches_rev, int nmismatches_rev, int nindels_fwd, int nindels_rev,
		     int indel_alignment_score_fwd, int indel_alignment_score_rev,
#endif
		     int alignment_score_fwd, int alignment_score_rev, int sense_filter) {
  int canonical_score_fwd, canonical_score_rev;

  if (pairs_fwd) {
    canonical_score_fwd = ncanonical_fwd - nbadintrons_fwd + nsemicanonical_fwd - nnoncanonical_fwd;
    debug11(printf("ncanonical_fwd %d, nbadintrons_fwd %d, nsemicanonical_fwd %d, nnoncanonical_fwd %d\n",
		   ncanonical_fwd,nbadintrons_fwd,nsemicanonical_fwd,nnoncanonical_fwd));
  }
  if (pairs_rev) {
    canonical_score_rev = ncanonical_rev - nbadintrons_rev + nsemicanonical_rev - nnoncanonical_rev;
    debug11(printf("ncanonical_rev %d, nbadintrons_rev %d, nsemicanonical_rev %d, nnoncanonical_rev %d\n",
		   ncanonical_rev,nbadintrons_rev,nsemicanonical_rev,nnoncanonical_rev));
  }

  if (pairs_fwd == NULL && pairs_rev == NULL) {
    debug11(printf("pairs_fwd is NULL and pairs_rev is NULL\n"));
    *winning_cdna_direction = 0;
    *sensedir = SENSE_NULL;
    return (List_T) NULL;

  } else if (pairs_rev == NULL) {
    debug11(printf("pairs_rev is NULL, so fwd wins\n"));
    *winning_cdna_direction = +1;
    *sensedir = SENSE_FORWARD;
    return pairs_fwd;

  } else if (pairs_fwd == NULL) {
    debug11(printf("pairs_fwd is NULL, so rev wins\n"));
    *winning_cdna_direction = -1;
    *sensedir = SENSE_ANTI;
    return pairs_rev;

#if 0
  } else if (nnoncanonical_fwd == 0 && nnoncanonical_rev > 0) {
    debug11(printf("nnoncanonical_fwd 0 && nnoncanonical_rev %d, so fwd wins\n",
		   nnoncanonical_rev));
    *winning_cdna_direction = +1;

  } else if (nnoncanonical_fwd > 0 && nnoncanonical_rev == 0) {
    debug11(printf("nnoncanonical_fwd %d && nnoncanonical_rev 0, so rev wins\n",
		   nnoncanonical_fwd));
    *winning_cdna_direction = -1;
#endif

#if 0
  } else if (canonical_score_fwd > canonical_score_rev + 1) {
    debug11(printf("canonical_score_fwd %d > canonical_score_rev %d + 1, so fwd wins\n",
		   canonical_score_fwd,canonical_score_rev));
    *winning_cdna_direction = +1;

  } else if (canonical_score_rev > canonical_score_fwd + 1) {
    debug11(printf("canonical_score_rev %d > canonical_score_fwd %d + 1, so rev wins\n",
		   canonical_score_rev,canonical_score_fwd));
    *winning_cdna_direction = -1;
#endif

#if 0
  } else if (indel_alignment_score_fwd >= 0 && indel_alignment_score_rev < 0 && nbadintrons_rev > 0) {
    debug11(printf("indel_alignment_score_fwd %d positive and indel_alignment_score_rev %d negative for a bad intron, so fwd wins\n",
		   indel_alignment_score_fwd,indel_alignment_score_rev));
    *winning_cdna_direction = +1;

  } else if (indel_alignment_score_rev >= 0 && indel_alignment_score_fwd < 0 && nbadintrons_fwd > 0) {
    debug11(printf("indel_alignment_score_fwd %d negative for a bad intron and indel_alignment_score_rev %d positive, so rev wins\n",
		   indel_alignment_score_fwd,indel_alignment_score_rev));
    *winning_cdna_direction = -1;
#endif

#if 0
    /* Cannot use, because favors a terminal over a splice */
  } else if (nmismatches_fwd < nmismatches_rev) {
    debug11(printf("nmismatches fwd %d < nmismatches rev %d, so fwd wins\n",
		   nmismatches_fwd,nmismatches_rev));
    *winning_cdna_direction = +1;

  } else if (nmismatches_fwd > nmismatches_rev) {
    debug11(printf("nmismatches fwd %d > nmismatches rev %d, so rev wins\n",
		   nmismatches_fwd,nmismatches_rev));
    *winning_cdna_direction = -1;
#endif

  } else if (avg_donor_score_fwd > 0.9 && avg_acceptor_score_fwd > 0.9 &&
	     (avg_donor_score_rev < 0.5 || avg_acceptor_score_rev < 0.5)) {
    debug11(printf("intronscores fwd %f,%f > intronscores rev %f,%f, so fwd wins\n",
		   avg_donor_score_fwd,avg_acceptor_score_fwd,avg_donor_score_rev,avg_acceptor_score_rev));
    /* intronscores reveal a clear sensedir */
    *winning_cdna_direction = +1;

  } else if (avg_donor_score_rev > 0.9 && avg_acceptor_score_rev > 0.9 &&
	     (avg_donor_score_fwd < 0.5 || avg_acceptor_score_fwd < 0.5)) {
    debug11(printf("intronscores rev %f,%f > intronscores fwd %f,%f, so fwd wins\n",
		   avg_donor_score_rev,avg_acceptor_score_rev,avg_donor_score_fwd,avg_acceptor_score_fwd));
    /* intronscores reveal a clear sensedir */
    *winning_cdna_direction = -1;

  } else if (alignment_score_fwd > alignment_score_rev + SCORE_SIGDIFF) {
    debug11(printf("alignment_score_fwd %d >> alignment_score_rev %d, so fwd wins\n",
		   alignment_score_fwd,alignment_score_rev));
    *winning_cdna_direction = +1;

  } else if (alignment_score_rev > alignment_score_fwd + SCORE_SIGDIFF) {
    debug11(printf("alignment_score_rev %d << alignment_score_fwd %d, so rev wins\n",
		   alignment_score_rev,alignment_score_fwd));
    *winning_cdna_direction = -1;

  } else if (nnoncanonical_fwd < nnoncanonical_rev) {
    debug11(printf("nnoncanonical_fwd %d < nnoncanonical_rev %d, so fwd wins\n",
		   nnoncanonical_fwd,nnoncanonical_rev));
    *winning_cdna_direction = +1;

  } else if (nnoncanonical_rev < nnoncanonical_fwd) {
    debug11(printf("nnoncanonical_rev %d < nnoncanonical_fwd %d, so rev wins\n",
		   nnoncanonical_rev,nnoncanonical_fwd));
    *winning_cdna_direction = -1;

  } else if (avg_donor_score_fwd + avg_acceptor_score_fwd > avg_donor_score_rev + avg_acceptor_score_rev + PROB_SIGDIFF) {
    debug11(printf("intronscores fwd %f+%f > intronscores rev %f+%f, so fwd wins\n",
		   avg_donor_score_fwd,avg_acceptor_score_fwd,avg_donor_score_rev,avg_acceptor_score_rev));
    /* intronscores reveal a preferred sensedir */
    *winning_cdna_direction = +1;

  } else if (avg_donor_score_rev + avg_acceptor_score_rev > avg_donor_score_fwd + avg_acceptor_score_fwd + PROB_SIGDIFF) {
    debug11(printf("intronscores rev %f+%f > intronscores fwd %f+%f, so fwd wins\n",
		   avg_donor_score_rev,avg_acceptor_score_rev,avg_donor_score_fwd,avg_acceptor_score_fwd));
    /* intronscores reveal a preferred sensedir */
    *winning_cdna_direction = -1;

  } else if (alignment_score_fwd > alignment_score_rev) {
    debug11(printf("alignment_score_fwd %d > alignment_score_rev %d, so fwd wins\n",
		   alignment_score_fwd,alignment_score_rev));
    *winning_cdna_direction = +1;

  } else if (alignment_score_rev > alignment_score_fwd) {
    debug11(printf("alignment_score_rev %d < alignment_score_fwd %d, so rev wins\n",
		   alignment_score_rev,alignment_score_fwd));
    *winning_cdna_direction = -1;

  } else {
    debug11(printf("scores all equal, so fwd wins\n"));
    /* No clear intron direction, so allow under all sense_filters */
    *winning_cdna_direction = +1;
    *sensedir = SENSE_NULL;
    return pairs_fwd;
  }

  if (*winning_cdna_direction == +1) {
    if (ncanonical_fwd == 0 && nsemicanonical_fwd == 0 && nnoncanonical_fwd == 0) {
      *sensedir = SENSE_NULL;
    } else if (max_intron_score_fwd < 1.8) {
      *sensedir = SENSE_NULL;
    } else {
      *sensedir = SENSE_FORWARD;
    }
#ifndef PMAP
    if (sense_filter < 0) {
      return (List_T) NULL;
    }
#endif
    debug11(printf("winning_cdna_direction = %d, sensedir = %d\n",
		   *winning_cdna_direction,*sensedir));
    return pairs_fwd;

  } else if (*winning_cdna_direction == -1) {
    if (ncanonical_rev == 0 && nsemicanonical_rev == 0 && nnoncanonical_rev == 0) {
      *sensedir = SENSE_NULL;
    } else if (max_intron_score_rev < 1.8) {
      *sensedir = SENSE_NULL;
    } else {
      *sensedir = SENSE_ANTI;
    }
#ifndef PMAP
    if (sense_filter > 0) {
      return (List_T) NULL;
    }
#endif
    debug11(printf("winning_cdna_direction = %d, sensedir = %d\n",
		   *winning_cdna_direction,*sensedir));
    return pairs_rev;

  } else {
    fprintf(stderr,"Unexpected value %d for winning_cdna_direction\n",*winning_cdna_direction);
    abort();
  }
}


T
Stage3_new (struct Pair_T *pairarray, List_T pairs, int npairs, int cdna_direction, int sensedir,
	    int stage2_source, int stage2_indexsize,
	    int matches, int unknowns, int mismatches, int qopens, int qindels,
	    int topens, int tindels, int ncanonical, int nsemicanonical,
	    int nnoncanonical, double defect_rate,
	    Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh, Chrpos_T chrlength,
	    bool watsonp, int querylength, int skiplength, int trimlength, double stage3_runtime,
	    int straintype, char *strain, IIT_T altstrain_iit) {
  T new = (T) MALLOC(sizeof(*new));
  Pair_T start, end;
  int *typematches, nmatches;

  new->pairarray = pairarray;
  new->pairarray_freeable_p = true;
  new->chimera_left_p = false;
  new->chimera_right_p = false;

  new->pairs = pairs;
  new->npairs = npairs;

  new->matches = matches;
  new->unknowns = unknowns;
  new->mismatches = mismatches;
  new->qopens = qopens;
  new->qindels = qindels;
  new->topens = topens;
  new->tindels = tindels;

  new->noncanonical = nsemicanonical + nnoncanonical;
  new->goodness = 0;

#ifdef PMAP
  /* Should be +1 */
  new->cdna_direction = cdna_direction;
  new->sensedir = sensedir;
#else
  if (cdna_direction == 0 && require_splicedir_p == true) {
    new->cdna_direction = Pair_guess_cdna_direction_array(&new->sensedir,pairarray,npairs,/*invertedp*/false,
							  chroffset,watsonp);
  } else if (ncanonical == 0 && nsemicanonical == 0 && nnoncanonical == 0) {
    new->cdna_direction = 0;
    new->sensedir = sensedir;
  } else {
    new->cdna_direction = cdna_direction;
    new->sensedir = sensedir;
  }
#endif


#if 0
  nexons = Pair_nexons_approx(pairs);
  if (nexons > 2) {
    /* Favor spliced transcripts, but only if we're sure they're
       spliced (i.e., 3 or more exons).  A random intron shouldn't
       get credit. */
    new->goodness += nexons;
  }
#endif
    
  new->translation_start = 0;
  new->translation_end = 0;
  new->translation_length = 0;

  new->stage2_source = stage2_source;
  new->stage2_indexsize = stage2_indexsize;

  start = &(pairarray[0]);
  end = &(pairarray[npairs-1]);

  new->straintype = straintype;
  new->strain = strain;

  new->chrnum = chrnum;
  new->chroffset = chroffset;
  new->chrhigh = chrhigh;
  new->chrlength = chrlength;
  new->circularpos = Pair_circularpos(pairarray,npairs,chrlength,watsonp,querylength);

  new->watsonp = watsonp;
  new->defect_rate = defect_rate;

  new->genomicstart = chroffset + Pair_genomepos(start);
  new->genomicend = chroffset + Pair_genomepos(end);

  new->stage3_runtime = stage3_runtime;

  new->trimmed_coverage = (double) (end->querypos - start->querypos + 1)/(double) (trimlength + skiplength);

  if (straintype == 0) {
    return new;
  } else {
    if (watsonp) {
      typematches = IIT_get_typed(&nmatches,altstrain_iit,/*divstring*/NULL,
				  new->genomicstart,new->genomicend,straintype,/*sortp*/false);
    } else {
      typematches = IIT_get_typed(&nmatches,altstrain_iit,/*divstring*/NULL,
				  new->genomicend,new->genomicstart,straintype,/*sortp*/false);
    }
    if (typematches == NULL) {
      Stage3_free(&new);
      return NULL;
    } else {
      FREE(typematches);
      return new;
    }
  }

  return new;
}


void
Stage3_free (T *old) {

  if (*old) {
    /* Don't free strain.  Belongs to altstrain_iit. */
    if ((*old)->pairarray_freeable_p == true) {
      FREE_OUT((*old)->pairarray);
    }
    FREE_OUT(*old);
  }
  return;
}

#if 0
/* Needed for mutation analysis in align_relative */
void
Stage3_genomicbounds (Univcoord_T *genomicstart, Univcoord_T *genomiclength, T this) {
  *genomicstart = this->chroffset;
  *genomiclength = this->genomiclength;
  return;
}
#endif


bool
Stage3_test_bounds (T this, int minpos, int maxpos) {
  int nstart;

  if (Pairpool_count_bounded(&nstart,this->pairs,minpos,maxpos) > 25) {
    return true;
  } else {
    return false;
  }
}

T
Stage3_apply_bounds (T this, int minpos, int maxpos, bool revertp) {
  int newnpairs;
  List_T p;
  Pair_T newpair, oldpair;

#if 0
  this->pairs = Pairpool_transfer_bounded(NULL,this->pairs,minpos,maxpos);
  this->pairs = List_reverse(this->pairs);
#else
  this->pairs = Pairpool_clip_bounded(this->pairs,minpos,maxpos);
#endif

  if ((newnpairs = List_length(this->pairs)) == 0) {
    if (revertp == false) {
      return NULL;
    }
  } else {
    Stage3_free_pairarray(&this);
    this->npairs = newnpairs;
      
    newpair = this->pairarray = (struct Pair_T *) MALLOC_OUT(newnpairs*sizeof(struct Pair_T));
    this->pairarray_freeable_p = true;
    for (p = this->pairs; p != NULL; p = p->rest) {
      oldpair = (Pair_T) p->first;
      memcpy(newpair++,oldpair,sizeof(struct Pair_T));
    }
  }

  return this;
}

#ifdef PMAP
void
Stage3_translate_cdna (T this, Sequence_T queryaaseq, bool strictp) {
  Translation_via_cdna(&this->translation_start,&this->translation_end,&this->translation_length,
		       &this->relaastart,&this->relaaend,
		       this->pairarray,this->npairs,Sequence_fullpointer(queryaaseq),strictp);
  return;
}

void
Stage3_backtranslate_cdna (T this, bool diagnosticp) {
  Backtranslation_cdna(this->pairarray,this->npairs,this->translation_start,this->translation_end,
		       diagnosticp);
  return;
}

#else

static void
truncate_fulllength (Stage3_T this, bool translatep, int cds_startpos, int querylength, bool strictp) {

  if (translatep == true) {
    if (this->cdna_direction < 0) {
      Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			      &this->relaastart,&this->relaaend,
			      this->pairarray,this->npairs,/*backwardsp*/true,/*revcompp*/true,/*fulllengthp*/true,
			      cds_startpos,querylength,strictp);
    } else {
      Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			      &this->relaastart,&this->relaaend,
			      this->pairarray,this->npairs,/*backwardsp*/false,/*revcompp*/false,/*fulllengthp*/true,
			      cds_startpos,querylength,strictp);
    }
  }

  Stage3_apply_bounds(this,Stage3_translation_start(this),Stage3_translation_end(this),/*revertp*/true);
  return;
}


void
Stage3_translate_genomic (T this, int npairs, bool fulllengthp, int cds_startpos, int querylength, bool truncatep, bool strictp) {

  if (this->cdna_direction < 0) {
    Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			    &this->relaastart,&this->relaaend,
			    this->pairarray,npairs,/*backwardsp*/true,/*revcompp*/true,fulllengthp,
			    cds_startpos,querylength,strictp);
  } else {
    Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			    &this->relaastart,&this->relaaend,
			    this->pairarray,npairs,/*backwardsp*/false,/*revcompp*/false,fulllengthp,
			    cds_startpos,querylength,strictp);
  }
  if (truncatep == true) {
    truncate_fulllength(this,/*translatep*/false,cds_startpos,querylength,strictp);
    if (this->cdna_direction < 0) {
      Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			      &this->relaastart,&this->relaaend,
			      this->pairarray,npairs,/*backwardsp*/true,/*revcompp*/true,fulllengthp,
			      cds_startpos,querylength,strictp);
    } else {
      Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			      &this->relaastart,&this->relaaend,
			      this->pairarray,npairs,/*backwardsp*/false,/*revcompp*/false,fulllengthp,
			      cds_startpos,querylength,strictp);
    }
  }

  return;
}
#endif

void
Stage3_translate_cdna_via_reference (T this, T reference, bool literalrefp) {
  bool fixshiftp = !literalrefp;

  if (this->watsonp == reference->watsonp) {
    if (reference->cdna_direction < 0) {
      Translation_via_reference(&this->relaastart,&this->relaaend,
				this->pairarray,this->npairs,this->watsonp,/*backwardsp*/true,/*revcompp*/true,
				reference->pairarray,reference->npairs,reference->watsonp,fixshiftp);
    } else {
      Translation_via_reference(&this->relaastart,&this->relaaend,
				this->pairarray,this->npairs,this->watsonp,/*backwardsp*/false,/*revcompp*/false,
				reference->pairarray,reference->npairs,reference->watsonp,fixshiftp);
    }
  } else {
    if (reference->cdna_direction < 0) {
      Translation_via_reference(&this->relaastart,&this->relaaend,
				this->pairarray,this->npairs,this->watsonp,/*backwardsp*/false,/*revcompp*/false,
				reference->pairarray,reference->npairs,reference->watsonp,fixshiftp);
    } else {
      Translation_via_reference(&this->relaastart,&this->relaaend,
				this->pairarray,this->npairs,this->watsonp,/*backwardsp*/true,/*revcompp*/true,
				reference->pairarray,reference->npairs,reference->watsonp,fixshiftp);
    }
  }

  return;
}

void
Stage3_fix_cdna_direction (T this, T reference) {
  if (this->cdna_direction == 0) {
    if (reference->cdna_direction > 0) {
      if (this->watsonp == reference->watsonp) {
	this->cdna_direction = +1;
      } else {
	this->cdna_direction = -1;
      }
    } else if (reference->cdna_direction < 0) {
      if (this->watsonp == reference->watsonp) {
	this->cdna_direction = -1;
      } else {
	this->cdna_direction = +1;
      }
    }
  }
  return;
}



void
Stage3_translate (T this,
#ifdef PMAP
		  Sequence_T queryseq, bool diagnosticp,
#endif
		  int querylength, bool fulllengthp,
		  int cds_startpos, bool truncatep, bool strictp,
		  bool maponlyp) {

  if (maponlyp == true) {
    this->translation_start = 0;
    this->translation_end = 0;
    this->translation_length = 0;
  } else {
#ifdef PMAP
    Translation_via_cdna(&this->translation_start,&this->translation_end,&this->translation_length,
			 &this->relaastart,&this->relaaend,
			 this->pairarray,this->npairs,Sequence_fullpointer(queryseq),strictp);
    Backtranslation_cdna(this->pairarray,this->npairs,this->translation_start,this->translation_end,
			 diagnosticp);
#else
    if (this->cdna_direction < 0) {
      Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			      &this->relaastart,&this->relaaend,
			      this->pairarray,this->npairs,/*backwardsp*/true,/*revcompp*/true,fulllengthp,
			      cds_startpos,querylength,strictp);
    } else {
      Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
			      &this->relaastart,&this->relaaend,
			      this->pairarray,this->npairs,/*backwardsp*/false,/*revcompp*/false,fulllengthp,
			      cds_startpos,querylength,strictp);
    }
    if (truncatep == true) {
      truncate_fulllength(this,/*translatep*/false,cds_startpos,querylength,strictp);
      if (this->cdna_direction < 0) {
	Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
				&this->relaastart,&this->relaaend,
				this->pairarray,this->npairs,/*backwardsp*/true,/*revcompp*/true,fulllengthp,
				cds_startpos,querylength,strictp);
      } else {
	Translation_via_genomic(&this->translation_start,&this->translation_end,&this->translation_length,
				&this->relaastart,&this->relaaend,
				this->pairarray,this->npairs,/*backwardsp*/false,/*revcompp*/false,fulllengthp,
				cds_startpos,querylength,strictp);
      }
      return;
    }
#endif
  }
  return;
}


void
Stage3_translate_chimera (T this, T mate,
#ifdef PMAP
			  Sequence_T queryseq, bool diagnosticp,
#endif
			  int querylength, bool fulllengthp,
			  int cds_startpos, bool truncatep, bool strictp,
			  bool maponlyp) {
  int npairs1, npairs2;
  int translation_start, translation_end, translation_length, relaastart, relaaend;

  if (maponlyp == true) {
    this->translation_start = 0;
    this->translation_end = 0;
    this->translation_length = 0;

    mate->translation_start = 0;
    mate->translation_end = 0;
    mate->translation_length = 0;

  } else {
    npairs1 = this->npairs;
    npairs2 = mate->npairs;

#ifdef PMAP
    Translation_via_cdna(&translation_start,&translation_end,&translation_length,
			 &relaastart,&relaaend,
			 this->pairarray,npairs1 + npairs2,Sequence_fullpointer(queryseq),strictp);
    Backtranslation_cdna(this->pairarray,npairs1 + npairs2,translation_start,translation_end,
			 diagnosticp);
#else
    if (this->cdna_direction < 0) {
      Translation_via_genomic(&translation_start,&translation_end,&translation_length,
			      &relaastart,&relaaend,
			      this->pairarray,npairs1 + npairs2,/*backwardsp*/true,/*revcompp*/true,fulllengthp,
			      cds_startpos,querylength,strictp);
    } else {
      Translation_via_genomic(&translation_start,&translation_end,&translation_length,
			      &relaastart,&relaaend,
			      this->pairarray,npairs1 + npairs2,/*backwardsp*/false,/*revcompp*/false,fulllengthp,
			      cds_startpos,querylength,strictp);
    }

    if (truncatep == true) {
      truncate_fulllength(this,/*translatep*/false,cds_startpos,querylength,strictp);
      if (this->cdna_direction < 0) {
	Translation_via_genomic(&translation_start,&translation_end,&translation_length,
				&relaastart,&relaaend,
				this->pairarray,npairs1 + npairs2,/*backwardsp*/true,/*revcompp*/true,fulllengthp,
				cds_startpos,querylength,strictp);
      } else {
	Translation_via_genomic(&translation_start,&translation_end,&translation_length,
				&relaastart,&relaaend,
				this->pairarray,npairs1 + npairs2,/*backwardsp*/false,/*revcompp*/false,fulllengthp,
				cds_startpos,querylength,strictp);
      }
    }

#endif

    if (translation_start < npairs1) {
      this->translation_start = translation_start;
      mate->translation_start = 0;
    } else {
      this->translation_start = npairs1 - 1;
      mate->translation_start = translation_start - npairs1;
    }
    if (translation_end < npairs1) {
      this->translation_end = translation_end;
      mate->translation_end = 0;
    } else {
      this->translation_end = npairs1 - 1;
      mate->translation_end = translation_end - npairs1;
    }

    /* Additional checks to stay within array bounds */
    if (this->translation_end >= this->npairs) {
      this->translation_end = this->npairs - 1;
    }
    if (this->translation_start > this->translation_end) {
      this->translation_start = this->translation_end;
    }

    if (mate->translation_end >= mate->npairs) {
      mate->translation_end = mate->npairs - 1;
    }
    if (mate->translation_start > mate->translation_end) {
      mate->translation_start = mate->translation_end;
    }

    debug(printf("Converted translation %d..%d in %d+%d pairs to %d..%d and %d..%d\n",
		 translation_start,translation_end,this->npairs,mate->npairs,
		 this->translation_start,this->translation_end,mate->translation_start,mate->translation_end));

    this->translation_length = Pair_translation_length(this->pairarray,this->npairs);
    mate->translation_length = Pair_translation_length(mate->pairarray,mate->npairs);
    debug(printf("Original translation length %d => %d plus %d\n",
		 translation_length,this->translation_length,mate->translation_length));

    this->relaastart = this->pairarray[this->translation_start].aapos;
    this->relaaend = this->pairarray[this->translation_end].aapos;

    mate->relaastart = mate->pairarray[mate->translation_start].aapos;
    mate->relaaend = mate->pairarray[mate->translation_end].aapos;

  }

  return;
}



void
Stage3_print_pathsummary (FILE *fp, T this, int pathnum, Univ_IIT_T chromosome_iit, Univ_IIT_T contig_iit, 
			  IIT_T altstrain_iit, Sequence_T queryseq,
			  char *dbversion, int maxmutations, bool diagnosticp, bool maponlyp) {
  Pair_T start, end;
  bool referencealignp;

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);
  referencealignp = this->straintype == 0 ? true : false;
  Pair_print_pathsummary(fp,pathnum,start,end,this->chrnum,this->chroffset,
			 chromosome_iit,referencealignp,altstrain_iit,this->strain,contig_iit,
			 dbversion,Sequence_fulllength_given(queryseq),Sequence_skiplength(queryseq),
			 Sequence_trim_start(queryseq),Sequence_trim_end(queryseq),
			 Pair_nexons(this->pairarray,this->npairs),this->matches,this->unknowns,this->mismatches,
			 this->qopens,this->qindels,this->topens,this->tindels,this->goodness,
			 this->watsonp,this->cdna_direction,
			 this->translation_start,this->translation_end,this->translation_length,
			 0,0,maponlyp,diagnosticp,this->stage2_source,this->stage2_indexsize,
			 this->defect_rate);
  if (maponlyp == false) {
    Translation_print_comparison(fp,this->pairarray,this->npairs,NULL,0,this->cdna_direction,
				 this->relaastart,this->relaaend,maxmutations);
  }
  fprintf(fp,"\n");

  return;
}


void
Stage3_print_pslformat_nt (FILE *fp, T this, Univ_IIT_T chromosome_iit, Sequence_T usersegment, Sequence_T queryaaseq) {
  Pair_T start, end;

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);

  Pair_print_pslformat_nt(fp,this->pairarray,this->npairs,start,end,queryaaseq,this->chrnum,
			  chromosome_iit,usersegment,
			  /* Pair_nexons(this->pairarray,this->npairs), */
			  this->matches,this->unknowns,this->mismatches, 
			  this->watsonp);
  return;
}

#ifdef PMAP
void
Stage3_print_pslformat_pro (FILE *fp, T this, Univ_IIT_T chromosome_iit, Sequence_T usersegment, Sequence_T queryaaseq, bool strictp) {
  Pair_T start, end;

#if 0
  Stage3_translate_cdna(this,queryaaseq,strictp);
  Stage3_backtranslate_cdna(this,/*diagnosticp*/false);
#endif

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);

  Pair_print_pslformat_pro(fp,this->pairarray,this->npairs,start,end,queryaaseq,this->chrnum,
			   chromosome_iit,usersegment,
			   /* Pair_nexons(this->pairarray,this->npairs), */
			   this->watsonp,this->cdna_direction);
  return;
}
#endif


void
Stage3_print_gff3 (FILE *fp, T this, int pathnum, Univ_IIT_T chromosome_iit, Sequence_T usersegment,
		   Sequence_T queryseq, int querylength, Printtype_T printtype, char *sourcename) {
  Pair_T start, end;
  bool gff_gene_format_p, gff_estmatch_format_p;

  if (printtype == GFF3_GENE) {
    gff_gene_format_p = true;
    gff_estmatch_format_p = false;
  } else if (printtype == GFF3_MATCH_CDNA) {
    gff_gene_format_p = false;
    gff_estmatch_format_p = false;
  } else if (printtype == GFF3_MATCH_EST) {
    gff_gene_format_p = false;
    gff_estmatch_format_p = true;
  } else {
    fprintf(stderr,"Unexpected printtype %d\n",printtype);
    abort();
  }

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);

  Pair_print_gff3(fp,this->pairarray,this->npairs,pathnum,
		  Sequence_accession(queryseq),start,end,
		  this->chrnum,chromosome_iit,usersegment,
		  this->translation_end,querylength,Sequence_skiplength(queryseq),
		  this->matches,this->mismatches,this->qindels,this->tindels,
		  this->watsonp,this->cdna_direction,gff_gene_format_p,gff_estmatch_format_p,
		  sourcename);
  return;
}


#ifndef GSNAP
#ifndef PMAP
/* Only for GMAP program */
void
Stage3_print_sam (FILE *fp, T this, int pathnum, int npaths,
		  int absmq_score, int first_absmq, int second_absmq, int mapq_score,
		  Univ_IIT_T chromosome_iit, Sequence_T usersegment,
		  Sequence_T queryseq, int chimera_part, Chimera_T chimera,
		  int quality_shift, bool sam_paired_p, char *sam_read_group_id) {
  int querylength;
  Chrpos_T chrpos;
  Pair_T pair;

  querylength = Sequence_fulllength_given(queryseq);
  if (this->watsonp == true) {
    pair = &(this->pairarray[0]);
    chrpos = pair->genomepos + 1U;
  } else {
    pair = &(this->pairarray[this->npairs-1]);
    chrpos = pair->genomepos + 1U;
  }

  if (this->circularpos > 0) {
    Pair_print_sam(fp,this->pairarray,this->npairs,
		   Sequence_accession(queryseq),/*acc2*/NULL,this->chrnum,chromosome_iit,usersegment,
		   Sequence_fullpointer(queryseq),Sequence_quality_string(queryseq),
		   /*hardclip5*/0,/*hardclip3*/querylength-this->circularpos,querylength,
		   this->watsonp,this->cdna_direction,chimera_part,chimera,
		   quality_shift,Sequence_firstp(queryseq),
		   pathnum,npaths,absmq_score,first_absmq,second_absmq,chrpos,
		   mapq_score,sam_paired_p,sam_read_group_id,/*invertp*/false,
		   /*circularp*/true);
    Pair_print_sam(fp,this->pairarray,this->npairs,
		   Sequence_accession(queryseq),/*acc2*/NULL,this->chrnum,chromosome_iit,usersegment,
		   Sequence_fullpointer(queryseq),Sequence_quality_string(queryseq),
		   /*hardclip5*/this->circularpos,/*hardclip3*/0,querylength,
		   this->watsonp,this->cdna_direction,chimera_part,chimera,
		   quality_shift,Sequence_firstp(queryseq),
		   pathnum,npaths,absmq_score,first_absmq,second_absmq,/*chrpos*/1,
		   mapq_score,sam_paired_p,sam_read_group_id,/*invertp*/false,
		   /*circularp*/true);
  } else {
    Pair_print_sam(fp,this->pairarray,this->npairs,
		   Sequence_accession(queryseq),/*acc2*/NULL,this->chrnum,chromosome_iit,usersegment,
		   Sequence_fullpointer(queryseq),Sequence_quality_string(queryseq),
		   /*hardclip5*/0,/*hardclip3*/0,querylength,
		   this->watsonp,this->cdna_direction,chimera_part,chimera,
		   quality_shift,Sequence_firstp(queryseq),
		   pathnum,npaths,absmq_score,first_absmq,second_absmq,chrpos,
		   mapq_score,sam_paired_p,sam_read_group_id,/*invertp*/false,
		   /*circularp*/false);
  }

  return;
}
#endif
#endif


void
Stage3_print_iit_map (FILE *fp, T this, Univ_IIT_T chromosome_iit, Sequence_T queryseq) {
  Pair_T start, end;

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);

  Pair_print_iit_map(fp,queryseq,Sequence_accession(queryseq),start,end,
		     this->chrnum,chromosome_iit);
  return;
}

void
Stage3_print_iit_exon_map (FILE *fp, T this, Univ_IIT_T chromosome_iit, Sequence_T queryseq) {
  Pair_T start, end;

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);

  Pair_print_iit_exon_map(fp,this->pairarray,this->npairs,queryseq,Sequence_accession(queryseq),
			  start,end,this->chrnum,chromosome_iit);
  return;
}

void
Stage3_print_splicesites (FILE *fp, T this, Univ_IIT_T chromosome_iit, Sequence_T queryseq) {
  Pair_print_splicesites(fp,this->pairarray,this->npairs,Sequence_accession(queryseq),
			 Pair_nexons(this->pairarray,this->npairs),this->chrnum,
			 chromosome_iit,this->watsonp);
  return;
}

void
Stage3_print_introns (FILE *fp, T this, Univ_IIT_T chromosome_iit, Sequence_T queryseq) {
  Pair_print_introns(fp,this->pairarray,this->npairs,Sequence_accession(queryseq),
		     Pair_nexons(this->pairarray,this->npairs),this->chrnum,
		     chromosome_iit);
  return;
}



void
Stage3_print_mutations (FILE *fp, T this, T reference, Univ_IIT_T chromosome_iit, Sequence_T queryseq,
			char *dbversion, bool showalignp, bool diagnosticp,
			int invertmode, bool nointronlenp, int wraplength,
			int maxmutations) {
  Pair_T start, end;
  bool referencealignp;

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);

  /*  Pair_dump_array(this->pairarray,this->npairs,false); */

  referencealignp = this->straintype == 0 ? true : false;
  Pair_print_pathsummary(fp,/*pathnum*/1,start,end,reference->chrnum,reference->chroffset,
			 chromosome_iit,referencealignp,/*altstrain_iit*/NULL,this->strain,/*contig_iit*/NULL,
			 dbversion,Sequence_fulllength_given(queryseq),Sequence_skiplength(queryseq),
			 Sequence_trim_start(queryseq),Sequence_trim_end(queryseq),
			 Pair_nexons(this->pairarray,this->npairs),this->matches,this->unknowns,this->mismatches,
			 this->qopens,this->qindels,this->topens,this->tindels,this->goodness,
			 this->watsonp,this->cdna_direction,
			 0,0,0,this->relaastart,this->relaaend,/*maponlyp*/false,
			 diagnosticp,this->stage2_source,this->stage2_indexsize,
			 this->defect_rate);
  Translation_print_comparison(fp,this->pairarray,this->npairs,reference->pairarray,reference->npairs,
			       this->cdna_direction,this->relaastart,this->relaaend,maxmutations);
  fprintf(fp,"\n");

  if (showalignp == true) {
    Pair_print_alignment(fp,this->pairarray,this->npairs,reference->chrnum,reference->chroffset,
			 chromosome_iit,this->watsonp,diagnosticp,invertmode,nointronlenp,wraplength);
  }
  debug1(Pair_dump_array(this->pairarray,this->npairs,/*zerobasedp*/true));
  debug1(Pair_check_array(this->pairarray,this->npairs));

  return;
}



static void
print_map (FILE *fp, T this, IIT_T map_iit, int *map_divint_crosstable,
	   Univ_IIT_T chromosome_iit, int pathnum, bool map_bothstrands_p,
	   int nflanking, bool print_comment_p) {
  int chrlow, chrhigh;
  Pair_T start, end;
  int chrpos1, chrpos2;
  int *iit_matches = NULL, nmatches, *leftflanks, nleftflanks, *rightflanks, nrightflanks;
  int divno, sign;
  char *chr;

  if ((divno = map_divint_crosstable[this->chrnum]) <= 0) {
    fprintf(fp,"  *Map hits for path %d (0):\n\n",pathnum);
    return;
  } else {
    chr = Chrnum_to_string(this->chrnum,chromosome_iit);
  }

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);

  if (this->watsonp) {
    chrlow = chrpos1 = Pair_genomepos(start);
    chrhigh = chrpos2 = Pair_genomepos(end);
    sign = +1;

  } else {
    chrhigh = chrpos1 = Pair_genomepos(start);
    chrlow = chrpos2 = Pair_genomepos(end);
    sign = -1;
  }

  if (map_bothstrands_p == true) {
    iit_matches = IIT_get_with_divno(&nmatches,map_iit,divno,chrlow,chrhigh,/*sortp*/false);
    if (nflanking > 0) {
      IIT_get_flanking_with_divno(&leftflanks,&nleftflanks,&rightflanks,&nrightflanks,map_iit,
				  divno,chrlow,chrhigh,nflanking,/*sign*/0);
    }
    if (nflanking > 0) {
      fprintf(fp,"  Map hits for path %d (%d|%d|%d):\n",pathnum,nleftflanks,nmatches,nrightflanks);
    } else {
      fprintf(fp,"  Map hits for path %d (%d):\n",pathnum,nmatches);
    }
    if (nflanking > 0) {
      IIT_print_header(fp,map_iit,leftflanks,nleftflanks,/*bothstrandsp*/true,chr,
		       /*reversep*/true,/*relativep*/false,/*left*/0U,print_comment_p);
      fprintf(fp,"    ====================\n");
    }
    IIT_print_header(fp,map_iit,iit_matches,nmatches,/*bothstrandsp*/true,chr,
		     /*reversep*/false,/*relativep*/false,/*left*/0U,print_comment_p);
    if (nflanking > 0) {
      fprintf(fp,"    ====================\n");
      IIT_print_header(fp,map_iit,rightflanks,nrightflanks,/*bothstrandsp*/true,chr,
		       /*reversep*/false,/*relativep*/false,/*left*/0U,print_comment_p);
    }

  } else {
    iit_matches = IIT_get_signed_with_divno(&nmatches,map_iit,divno,chrlow,chrhigh,/*sortp*/true,sign);
    if (nflanking > 0) {
      IIT_get_flanking_with_divno(&leftflanks,&nleftflanks,&rightflanks,&nrightflanks,map_iit,
				  divno,chrlow,chrhigh,nflanking,sign);
    }
    if (nflanking > 0) {
      fprintf(fp,"  Map hits for path %d (%d|%d|%d):\n",pathnum,nleftflanks,nmatches,nrightflanks);
    } else {
      fprintf(fp,"  Map hits for path %d (%d):\n",pathnum,nmatches);
    }
    if (nflanking > 0) {
      IIT_print_header(fp,map_iit,leftflanks,nleftflanks,/*bothstrandsp*/false,chr,
		       /*reversep*/true,/*relativep*/false,/*left*/0U,print_comment_p);
      fprintf(fp,"    ====================\n");
    }
    IIT_print_header(fp,map_iit,iit_matches,nmatches,/*bothstrandsp*/false,chr,
		     /*reversep*/false,/*relativep*/false,/*left*/0U,print_comment_p);
    if (nflanking > 0) {
      fprintf(fp,"    ====================\n");
      IIT_print_header(fp,map_iit,rightflanks,nrightflanks,/*bothstrandsp*/false,chr,
		       /*reversep*/false,/*relativep*/false,/*left*/0U,print_comment_p);
    }
  }
  fprintf(fp,"\n");

  if (nflanking > 0) {
    FREE(rightflanks);
    FREE(leftflanks);
  }

  FREE(iit_matches);
  FREE(chr);
  return;
}


/* Doesn't handle nflanking */
static void
print_exon_map (FILE *fp, T this, IIT_T map_iit, int *map_divint_crosstable, 
		Univ_IIT_T chromosome_iit, int pathnum, bool map_bothstrands_p, bool print_comment_p) {
  Uintlist_T exonbounds;
  Chrpos_T position1, position2;
  int *iit_matches = NULL, nmatches;
  int divno, exonno = 0;
  char *chr;

  if ((divno = map_divint_crosstable[this->chrnum]) <= 0) {
    fprintf(fp,"  *Map hits for path %d (0):\n\n",pathnum);
    return;
  } else {
    chr = Chrnum_to_string(this->chrnum,chromosome_iit);
  }

  exonbounds = Pair_exonbounds(this->pairarray,this->npairs,/*chroffset*/0U);

  while (exonbounds != NULL) {
    exonbounds = Uintlist_pop(exonbounds,&position1);
    exonbounds = Uintlist_pop(exonbounds,&position2);

    if (map_bothstrands_p == true) {
      if (position1 < position2) {
	iit_matches = IIT_get(&nmatches,map_iit,chr,position1,position2,/*sortp*/true);
      } else {
	iit_matches = IIT_get(&nmatches,map_iit,chr,position2,position1,/*sortp*/true);
      }
      fprintf(fp,"  Map hits for path %d, exon %d (%d):\n",pathnum,++exonno,nmatches);
      IIT_print_header(fp,map_iit,iit_matches,nmatches,/*bothstrandsp*/true,chr,
		       /*reversep*/false,/*relativep*/false,/*left*/0U,print_comment_p);

    } else {
      if (position1 < position2) {
	iit_matches = IIT_get_signed_with_divno(&nmatches,map_iit,divno,position1,position2,
						/*sortp*/true,/*sign*/+1);
      } else {
	iit_matches = IIT_get_signed_with_divno(&nmatches,map_iit,divno,position2,position1,
						/*sortp*/true,/*sign*/-1);
      }
      fprintf(fp,"  Map hits for path %d, exon %d (%d):\n",pathnum,++exonno,nmatches);
      IIT_print_header(fp,map_iit,iit_matches,nmatches,/*bothstrandsp*/false,chr,
		       /*reversep*/false,/*relativep*/false,/*left*/0U,print_comment_p);
    }
    fprintf(fp,"\n");
    FREE(iit_matches);
  }

  return;
}

void
Stage3_print_map (FILE *fp, T this, IIT_T map_iit, int *map_divint_crosstable, Univ_IIT_T chromosome_iit,
		  int pathnum, bool map_exons_p, bool map_bothstrands_p, int nflanking,
		  bool print_comment_p) {
  if (map_exons_p == true) {
    print_exon_map(fp,this,map_iit,map_divint_crosstable,
		   chromosome_iit,pathnum,map_bothstrands_p,print_comment_p);
  } else {
    print_map(fp,this,map_iit,map_divint_crosstable,
	      chromosome_iit,pathnum,map_bothstrands_p,nflanking,print_comment_p);
  }
  return;
}



/* queryaaseq is used only by PMAP */
void
Stage3_print_alignment (FILE *fp, T this, Genome_T genome,
			Univ_IIT_T chromosome_iit, Printtype_T printtype,
			bool continuousp, bool continuous_by_exon_p, bool diagnosticp, bool genomefirstp,
			int invertmode, bool nointronlenp, int wraplength) {
  if (continuous_by_exon_p == true) {
    Pair_print_exonsummary(fp,this->pairarray,this->npairs,this->chrnum,this->chroffset,
			   genome,chromosome_iit,this->watsonp,this->cdna_direction,genomefirstp,invertmode);
    Pair_print_continuous_byexon(fp,this->pairarray,this->npairs,this->watsonp,diagnosticp,invertmode);

  } else if (continuousp == true) {

#if 0
    if (maponlyp == false) {
#ifdef PMAP
      Stage3_translate_cdna(this,queryaaseq,strictp);
      Stage3_backtranslate_cdna(this,diagnosticp);
#endif
    }
#endif

    Pair_print_continuous(fp,this->pairarray,this->npairs,this->watsonp,
			  diagnosticp,genomefirstp,invertmode,nointronlenp);
  } else {
    /* Assumes Stage3_print_pathsummary already called */
    Pair_print_exonsummary(fp,this->pairarray,this->npairs,this->chrnum,this->chroffset,
			   genome,chromosome_iit,this->watsonp,this->cdna_direction,genomefirstp,invertmode);
    if (printtype == ALIGNMENT) {
      Pair_print_alignment(fp,this->pairarray,this->npairs,this->chrnum,this->chroffset,
			   chromosome_iit,this->watsonp,diagnosticp,invertmode,nointronlenp,wraplength);
    }
  }
  debug1(Pair_dump_array(this->pairarray,this->npairs,/*zerobasedp*/true));
  debug1(Pair_check_array(this->pairarray,this->npairs));
  return;
}


void
Stage3_print_coordinates (FILE *fp, T this, Univ_IIT_T chromosome_iit, int invertmode) {
  Pair_print_coordinates(fp,this->pairarray,this->npairs,this->chrnum,this->chroffset,
			 chromosome_iit,this->watsonp,invertmode);
  return;
}


void
Stage3_print_cdna (FILE *fp, T this, int wraplength) {
#ifdef PMAP
  Pair_print_nucleotide_cdna(fp,this->pairarray,this->npairs,wraplength);
#else
  if (this->cdna_direction >= 0) {
    Pair_print_protein_cdna(fp,this->pairarray,this->npairs,wraplength,/*forwardp*/true);
  } else {
    Pair_print_protein_cdna(fp,&(this->pairarray[this->npairs-1]),this->npairs,wraplength,/*forwardp*/false);
  }
#endif
  return;
}

void
Stage3_print_protein_genomic (FILE *fp, T this, int wraplength) {
  if (this->cdna_direction >= 0) {
    Pair_print_protein_genomic(fp,this->pairarray,this->npairs,wraplength,/*forwardp*/true);
  } else {
    Pair_print_protein_genomic(fp,&(this->pairarray[this->npairs-1]),this->npairs,wraplength,/*forwardp*/false);
  }
  return;
}


void
Stage3_print_compressed (FILE *fp, T this, Sequence_T queryseq, Univ_IIT_T chromosome_iit,
			 char *dbversion, Sequence_T usersegment, int pathnum, int npaths,
			 bool checksump, int chimerapos, int chimeraequivpos,
			 double donor_prob, double acceptor_prob, int chimera_cdna_direction) {
  Pair_T start, end;

#if 0
#ifdef PMAP
  Stage3_translate_cdna(this,queryseq,strictp);
  Stage3_backtranslate_cdna(this,/*diagnosticp*/false);
#else
  if (truncatep == true) {
    truncate_fulllength(this,/*translatep*/true,/*cds_startpos*/-1,
			Sequence_fulllength_given(queryseq),strictp);
  }
#endif
#endif

  start = &(this->pairarray[0]);
  end = &(this->pairarray[this->npairs-1]);
  Pair_print_compressed(fp,pathnum,npaths,start,end,queryseq,dbversion,usersegment,
			Pair_nexons(this->pairarray,this->npairs),
			Stage3_fracidentity(this),this->pairarray,this->npairs,
			this->chrnum,this->chroffset,chromosome_iit,
			Sequence_fulllength_given(queryseq),Sequence_skiplength(queryseq),
			Sequence_trim_start(queryseq),Sequence_trim_end(queryseq),
			checksump,chimerapos,chimeraequivpos,donor_prob,acceptor_prob,
			chimera_cdna_direction,this->strain,this->watsonp,
			this->cdna_direction);
  return;
}



#if 0
static int
compute_introntype (char left1, char left2, char right2, char right1) {
  int leftdi, rightdi;

  if (left1 == 'G' && left2 == 'T') {
    leftdi = LEFT_GT;
  } else if (left1 == 'G' && left2 == 'C') {
    leftdi = LEFT_GC;
  } else if (left1 == 'A' && left2 == 'T') {
    leftdi = LEFT_AT;
#ifndef PMAP
  } else if (left1 == 'C' && left2 == 'T') {
    leftdi = LEFT_CT;
#endif
  } else {
    leftdi = 0x00;
  }

  if (right2 == 'A' && right1 == 'G') {
    rightdi = RIGHT_AG;
  } else if (right2 == 'A' && right1 == 'C') {
    rightdi = RIGHT_AC;
#ifndef PMAP
  } else if (right2 == 'G' && right1 == 'C') {
    rightdi = RIGHT_GC;
  } else if (right2 == 'A' && right1 == 'T') {
    rightdi = RIGHT_AT;
#endif
  } else {
    rightdi = 0x00;
  }

  return leftdi & rightdi;
}
#endif

static char uppercaseCode[128] = UPPERCASE_U2T;

static List_T
peel_leftward (bool *mismatchp, List_T *peeled_path, List_T path, int *querydp5, int *genomedp5, 
#ifdef WASTE
	       Pairpool_T pairpool,
#endif
	       int maxpeelback, bool throughmismatchp, bool quit_on_gap_p,
	       List_T *endgappairs, Pair_T *gappair, int *querydp5_medialgap, int *genomedp5_medialgap) {
  List_T peeled = NULL, rest = NULL, pairptr;
  Pair_T pair, nextpair, rightpair;
  int npeelback = 0, nconsecutive = 0, init_dynprogindex = DYNPROGINDEX_MINOR;
  bool stopp;
  int nmatches;
#if 0
  int nincursion = 0;
#endif

  *mismatchp = false;
  debug(printf("Peeling leftward:"));
  if (path == NULL) {
    debug(printf(" path is empty\n"));
  } else {
    pair = path->first;
    if (pair->gapp == true) {
      /* Throw away known gap */
      debug(printf(" Known_gap"));
      pairptr = path;
      path = Pairpool_pop(path,&pair);
#ifdef WASTE
      peeled = Pairpool_push_existing(peeled,pairpool,pair);
#else
      peeled = List_push_existing(peeled,pairptr);
#endif
    }
    rest = path->rest;
    
    stopp = false;
    while (rest != NULL && stopp == false) {
      nextpair = rest->first;
      if (nextpair->gapp == true || nextpair->cdna == ' ' || nextpair->genome == ' ' || nextpair->protectedp == true) {
	stopp = true;
      } else {
	pairptr = path;
	path = Pairpool_pop(path,&pair);
#ifdef WASTE
	peeled = Pairpool_push_existing(peeled,pairpool,pair);
#else
	peeled = List_push_existing(peeled,pairptr);
#endif
	debug(printf(" Peel [");
	      Pair_dump_one(pair,/*zerobasedp*/true);
	      printf("]"));

	if (uppercaseCode[(int) pair->cdna] != uppercaseCode[(int) pair->genome]) {
	  *mismatchp = true;
	}

	if (++npeelback >= maxpeelback) {
	  stopp = true;
	}

	if (init_dynprogindex > 0 && pair->dynprogindex <= 0) {
	  init_dynprogindex = pair->dynprogindex;
	}

	rest = path->rest;
      }
    }

    /* Continue to peelback through little skips and mismatches */
    debug(printf("\n||"));

    stopp = false;
    while (rest != NULL && stopp == false) {
      nextpair = rest->first;
      if (nextpair->gapp == true) {
	/* Peel this one, but then stop at end of loop */
      } else if (nextpair->protectedp == true) {
	/* Stop because it's protected */
	stopp = true;
      } else if (nextpair->cdna != ' ' && nextpair->genome != ' ') {
	/* Stop because it looks okay */
	stopp = true;
      }

      pairptr = path;
      path = Pairpool_pop(path,&pair);
#ifdef WASTE
      peeled = Pairpool_push_existing(peeled,pairpool,pair);
#else
      peeled = List_push_existing(peeled,pairptr);
#endif
      debug(printf(" Extrapeel [");
	    Pair_dump_one(pair,/*zerobasedp*/true);
	    printf("]"));
	
      if (uppercaseCode[(int) pair->cdna] != uppercaseCode[(int) pair->genome]) {
	*mismatchp = true;
      }

#if 0
      if (pair->comp == INDEL_COMP || pair->comp == MISMATCH_COMP) {
	nconsecutive = 0;
      } else if (++nconsecutive >= SUFFCONSECUTIVE) {
	stopp = true;
      }
#endif
      
#if 0
      if (pair->dynprogindex != init_dynprogindex) {
	if (++nincursion >= MAXINCURSION) {
	  stopp = true;
	}
      }
#endif

      if (pair->gapp == true) {
	stopp = true;
      }

      rest = path->rest;
    }
  }

#ifdef PMAP
  /* Reverse process to codon boundary.  Cases:

     X X X | X -
     0 1 2   3 4

     X X X | - X
     0 1 2   3 3

     X X - X | X
     0 1 2 2   3

     Rule: nextpair->querypos % 3 == 0 */

  debug(printf("\n<<"));
  if (peeled != NULL) {
    rest = peeled->rest;
    stopp = false;
    while (rest != NULL && stopp == false) {
      pairptr = peeled;
      peeled = Pairpool_pop(peeled,&pair);
#ifdef WASTE
      path = Pairpool_push_existing(path,pairpool,pair);
#else
      path = List_push_existing(path,pairptr);
#endif
      debug(printf(" Mod3putback [");
	    Pair_dump_one(pair,/*zerobasedp*/true);
	    printf("]"));
      nextpair = rest->first;
      if (nextpair->querypos % 3 == 0) {
	stopp = true;
      }
      rest = peeled->rest;
    }
  }
#endif

  if (peeled == NULL) {
    /* Do not alter querydp5 or genomedp5 */
  } else {
    rightpair = peeled->first;
    while (peeled != NULL && (rightpair->gapp == true || rightpair->comp == INDEL_COMP)) {
      debug(printf("Ran into gap; undoing peel, case 1, rightpair gapp %d, comp %c\n",
		   rightpair->gapp,rightpair->comp));
      if (endgappairs != NULL) {
	path = Pairpool_transfer(path,*endgappairs);
	*endgappairs = (List_T) NULL;
      }

      if (quit_on_gap_p == true) {
	path = Pairpool_transfer(path,peeled);
	*peeled_path = (List_T) NULL;
	return path;

      } else {
	/* Put back 1 */
	/* if ((pairptr = peeled) != NULL) { */
	pairptr = peeled;
	peeled = Pairpool_pop(peeled,&pair);
	path = List_push_existing(path,pairptr);
	debug(printf(" Putback [");
	      Pair_dump_one(pair,/*zerobasedp*/true);
	      printf("]"));
	  /* } */

#if 0
	/* Put back 2 */
	if ((pairptr = peeled) != NULL) {
	  peeled = Pairpool_pop(peeled,&pair);
	  path = List_push_existing(path,pairptr);
	  debug(printf(" Putback [");
		Pair_dump_one(pair,/*zerobasedp*/true);
		printf("]"));
	}
#endif
      }

      rightpair = path->first;
    }

    if (path != NULL) {
      rightpair = path->first;
      *querydp5 = rightpair->querypos + 1;
      *genomedp5 = rightpair->genomepos + 1;
    } else if (peeled != NULL) {
      rightpair = peeled->first;
      *querydp5 = rightpair->querypos;
      *genomedp5 = rightpair->genomepos;
    } else {
      fprintf(stderr,"In peel_rightward, path and peeled are both NULL\n");
      abort();
    }
  }

  if (endgappairs != NULL) {
    if (path == NULL || (pair = path->first) == NULL || (pair->gapp == false && pair->comp != INDEL_COMP)) {
      *endgappairs = NULL;
      *querydp5_medialgap = *querydp5;
      *genomedp5_medialgap = *genomedp5;
    } else {
      pairptr = path;
      path = Pairpool_pop(path,&pair);
#ifdef WASTE
      *endgappairs = Pairpool_push_existing(NULL,pairpool,pair);
#else
      *endgappairs = List_push_existing(NULL,pairptr);
#endif
      debug(printf(" Peeling gap [");
	    Pair_dump_one(pair,/*zerobasedp*/true);
	    printf("]"));
      *gappair = pair;
      debug(printf(" gapcomp: '%c'",pair->comp));

      nmatches = 0;
      while (path != NULL && nmatches < 3) {
	pairptr = path;
	path = Pairpool_pop(path,&pair);
#ifdef WASTE
	*endgappairs = Pairpool_push_existing(*endgappairs,pairpool,pair);
#else
	*endgappairs = List_push_existing(*endgappairs,pairptr);
#endif
	debug(printf(" Peeling after gap [");
	      Pair_dump_one(pair,/*zerobasedp*/true);
	      printf("]"));
	if (uppercaseCode[(int) pair->cdna] == uppercaseCode[(int) pair->genome]) {
	  nmatches++;
	}
      }

      rightpair = (*endgappairs)->first;
      if (rightpair->gapp == true || rightpair->comp == INDEL_COMP) {
	debug(printf("Ran into gap; undoing peel, case 2\n"));
	path = Pairpool_transfer(path,*endgappairs);
	*endgappairs = (List_T) NULL;

	if (quit_on_gap_p == true) {
	  path = Pairpool_transfer(path,peeled);
	  *peeled_path = (List_T) NULL;
	  return path;

	} else {
	  /* Put back 1 */
	  if ((pairptr = peeled) != NULL) {
	    peeled = Pairpool_pop(peeled,&pair);
	    path = List_push_existing(path,pairptr);
	    debug(printf(" Putback [");
		  Pair_dump_one(pair,/*zerobasedp*/true);
		  printf("]"));
	  }

	  /* Put back 2 */
	  if ((pairptr = peeled) != NULL) {
	    peeled = Pairpool_pop(peeled,&pair);
	    path = List_push_existing(path,pairptr);
	    debug(printf(" Putback [");
		  Pair_dump_one(pair,/*zerobasedp*/true);
		  printf("]"));
	  }
	}
      }

      if (path != NULL) {
	rightpair = path->first;
	*querydp5_medialgap = rightpair->querypos + 1;
	*genomedp5_medialgap = rightpair->genomepos + 1;
      } else if (peeled != NULL) {
	rightpair = peeled->first;
	*querydp5_medialgap = rightpair->querypos;
	*genomedp5_medialgap = rightpair->genomepos;
      } else {
	fprintf(stderr,"In peel_rightward for medialgap, path and peeled are both NULL\n");
	abort();
      }
    }
  }

  assert(peeled == NULL || path == NULL || ((Pair_T) path->first)->comp != INDEL_COMP);
  debug(
	if (path == NULL) {
	  printf(" => Top of path is NULL.");
	} else {
	  pair = path->first;
	  printf(" => Top of path is ");
	  Pair_dump_one(pair,/*zerobasedp*/true);
	}
	printf("\n => querydp5 = %d, genomedp5 = %d\n",*querydp5,*genomedp5);
	);

  *peeled_path = peeled;
  return path;
}


static List_T
peel_rightward (bool *mismatchp, List_T *peeled_pairs, List_T pairs, int *querydp3, int *genomedp3, 
#ifdef WASTE
		Pairpool_T pairpool,
#endif
		int maxpeelback, bool throughmismatchp, bool quit_on_gap_p,
		List_T *endgappairs, Pair_T *gappair, int *querydp3_medialgap, int *genomedp3_medialgap) {
  List_T peeled = NULL, rest = NULL, pairptr;
  Pair_T pair, nextpair, leftpair;
  int npeelback = 0, nconsecutive = 0, init_dynprogindex = DYNPROGINDEX_MINOR;
  bool stopp;
  int nmatches;
#if 0
  int incursion = 0;
#endif

  *mismatchp = false;
  debug(printf("Peeling rightward:"));
  if (pairs == NULL) {
    debug(printf(" pairs is empty\n"));
  } else {
    pair = pairs->first;
    if (pair->gapp == true) {
      /* Throw away known gap */
      debug(printf(" Known_gap"));
      pairptr = pairs;
      pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
      peeled = Pairpool_push_existing(peeled,pairpool,pair);
#else
      peeled = List_push_existing(peeled,pairptr);
#endif
    }
    rest = pairs->rest;

    stopp = false;
    while (rest != NULL && stopp == false) {
      nextpair = rest->first;
      if (nextpair->gapp == true || nextpair->cdna == ' ' || nextpair->genome == ' ' || nextpair->protectedp == true) {
	stopp = true;
      } else {
	pairptr = pairs;
	pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
	peeled = Pairpool_push_existing(peeled,pairpool,pair);
#else
	peeled = List_push_existing(peeled,pairptr);
#endif
	debug(printf(" Peel [");
	      Pair_dump_one(pair,/*zerobasedp*/true);
	      printf("]"));
      
	if (uppercaseCode[(int) pair->cdna] != uppercaseCode[(int) pair->genome]) {
	  *mismatchp = true;
	}

	if (++npeelback >= maxpeelback) {
	  stopp = true;
	}

	if (init_dynprogindex > 0 && pair->dynprogindex <= 0) {
	  init_dynprogindex = pair->dynprogindex;
	}

	rest = pairs->rest;
      }
    }

    /* Continue to peelback through little skips and mismatches */
    debug(printf("\n||"));

    stopp = false;
    while (rest != NULL && stopp == false) {
      nextpair = rest->first;
      if (nextpair->gapp == true) {
	/* Peel this one, but then stop at end of loop */
      } else if (nextpair->protectedp == true) {
	/* Stop because it's protected */
	stopp = true;
      } else if (nextpair->cdna != ' ' && nextpair->genome != ' ') {
	/* Stop because it looks okay */
	stopp = true;
      }

      pairptr = pairs;
      pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
      peeled = Pairpool_push_existing(peeled,pairpool,pair);
#else
      peeled = List_push_existing(peeled,pairptr);
#endif
      debug(printf(" Extrapeel [");
	    Pair_dump_one(pair,/*zerobasedp*/true);
	    printf("]"));
	
      if (uppercaseCode[(int) pair->cdna] != uppercaseCode[(int) pair->genome]) {
	*mismatchp = true;
      }

#if 0
      if (pair->comp == INDEL_COMP || pair->comp == MISMATCH_COMP) {
	nconsecutive = 0;
      } else if (++nconsecutive >= SUFFCONSECUTIVE) {
	stopp = true;
      }
#endif
	
#if 0
      if (pair->dynprogindex != init_dynprogindex) {
	if (++nincursion >= MAXINCURSION) {
	  stopp = true;
	}
      }
#endif

      if (pair->gapp == true) {
	stopp = true;
      }

      rest = pairs->rest;
    }
  }

#ifdef PMAP
  /* Reverse process to codon boundary.  Cases:

     - X | X X X
     5 5   6 7 8

     X - | X X X
     5 6   6 7 8

     X | X - X X
     5   6 7 7 8

     Rule: pair->querypos % 3 == 0 */

  debug(printf("\n<<"));
  stopp = false;
  while (peeled != NULL && stopp == false) {
    pairptr = peeled;
    peeled = Pairpool_pop(peeled,&pair);
#ifdef WASTE
    pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
    pairs = List_push_existing(pairs,pairptr);
#endif
    debug(printf(" Mod3putback [");
	  Pair_dump_one(pair,/*zerobasedp*/true);
	  printf("]"));
    if (pair->querypos % 3 == 0) {
      stopp = true;
    }
  }
#endif

  if (peeled == NULL) {
    /* Do not alter querydp3 or genomedp3 */
  } else {
    leftpair = peeled->first;
    while (peeled != NULL && (leftpair->gapp == true || leftpair->comp == INDEL_COMP)) {
      debug(printf("Ran into gap; undoing peel, case 3, leftpair gapp %d, comp %c\n",
		   leftpair->gapp,leftpair->comp));
      if (endgappairs != NULL) {
	pairs = Pairpool_transfer(pairs,*endgappairs);
	*endgappairs = (List_T) NULL;
      }

      if (quit_on_gap_p == true) {
	pairs = Pairpool_transfer(pairs,peeled);
	*peeled_pairs = (List_T) NULL;
	return pairs;

      } else {
	/* Put back 1 */
	/* if ((pairptr = peeled) != NULL) { */
	pairptr = peeled;
	peeled = Pairpool_pop(peeled,&pair);
	pairs = List_push_existing(pairs,pairptr);
	debug(printf(" Putback [");
	      Pair_dump_one(pair,/*zerobasedp*/true);
	      printf("]"));
	  /* } */

#if 0
	/* Put back 2 */
	if ((pairptr = peeled) != NULL) {
	  peeled = Pairpool_pop(peeled,&pair);
	  pairs = List_push_existing(pairs,pairptr);
	  debug(printf(" Putback [");
		Pair_dump_one(pair,/*zerobasedp*/true);
		printf("]"));
	}
#endif
      }

      leftpair = pairs->first;
    }

    if (pairs != NULL) {
      leftpair = pairs->first;
      *querydp3 = leftpair->querypos - 1;
      *genomedp3 = leftpair->genomepos - 1;
    } else if (peeled != NULL) {
      leftpair = peeled->first;
      *querydp3 = leftpair->querypos;
      *genomedp3 = leftpair->genomepos;
    } else {
      fprintf(stderr,"In peel_leftward, pairs and peeled are both NULL\n");
      abort();
    }
  }

  if (endgappairs != NULL) {
    if (pairs == NULL || (pair = pairs->first) == NULL || (pair->gapp == false && pair->comp != INDEL_COMP)) {
      *endgappairs = NULL;
      *querydp3_medialgap = *querydp3;
      *genomedp3_medialgap = *genomedp3;
    } else {
      pairptr = pairs;
      pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
      *endgappairs = Pairpool_push_existing(NULL,pairpool,pair);
#else
      *endgappairs = List_push_existing(NULL,pairptr);
#endif
      debug(printf(" Peeling gap [");
	    Pair_dump_one(pair,/*zerobasedp*/true);
	    printf("]"));
      *gappair = pair;
      debug(printf(" gapcomp: '%c'",pair->comp));

      nmatches = 0;
      while (pairs != NULL && nmatches < 3) {
	pairptr = pairs;
	pairs = Pairpool_pop(pairs,&pair);
#ifdef WASTE
	*endgappairs = Pairpool_push_existing(*endgappairs,pairpool,pair);
#else
	*endgappairs = List_push_existing(*endgappairs,pairptr);
#endif
	debug(printf(" Peeling after gap [");
	      Pair_dump_one(pair,/*zerobasedp*/true);
	      printf("]"));
	if (uppercaseCode[(int) pair->cdna] == uppercaseCode[(int) pair->genome]) {
	  nmatches++;
	}
      }

      leftpair = (*endgappairs)->first;
      if (leftpair->gapp == true || leftpair->comp == INDEL_COMP) {
	debug(printf("Ran into gap; undoing peel, case 4\n"));
	pairs = Pairpool_transfer(pairs,*endgappairs);
	*endgappairs = (List_T) NULL;

	if (quit_on_gap_p == true) {
	  pairs = Pairpool_transfer(pairs,peeled);
	  *peeled_pairs = (List_T) NULL;
	  return pairs;

	} else {
	  /* Put back 1 */
	  if ((pairptr = peeled) != NULL) {
	    peeled = Pairpool_pop(peeled,&pair);
	    pairs = List_push_existing(pairs,pairptr);
	    debug(printf(" Putback [");
		  Pair_dump_one(pair,/*zerobasedp*/true);
		  printf("]"));
	  }

	  /* Put back 2 */
	  if ((pairptr = peeled) != NULL) {
	    peeled = Pairpool_pop(peeled,&pair);
	    pairs = List_push_existing(pairs,pairptr);
	    debug(printf(" Putback [");
		  Pair_dump_one(pair,/*zerobasedp*/true);
		  printf("]"));
	  }
	}
      }

      if (pairs != NULL) {
	leftpair = pairs->first;
	*querydp3_medialgap = leftpair->querypos - 1;
	*genomedp3_medialgap = leftpair->genomepos - 1;
      } else if (peeled != NULL) {
	leftpair = peeled->first;
	*querydp3_medialgap = leftpair->querypos;
	*genomedp3_medialgap = leftpair->genomepos;
      } else {
	fprintf(stderr,"In peel_leftward for medialgap, pairs and peeled are both NULL\n");
	abort();
      }
    }
  }

  assert(peeled == NULL || pairs == NULL || ((Pair_T) pairs->first)->comp != INDEL_COMP);
  debug(
	if (pairs == NULL) {
	  printf(" => Top of pairs is NULL.");
	} else {
	  pair = pairs->first;
	  printf(" => Top of pairs is ");
	  Pair_dump_one(pair,/*zerobasedp*/true);
	}
	printf("\n => querydp3 = %d, genomedp3 = %d\n",*querydp3,*genomedp3);
	);

  *peeled_pairs = peeled;
  return pairs;
}


/************************************************************************
 *  Traversal functions
 ************************************************************************/

/* For peel_rightward and peel_leftward, we set quit_on_gap_p = true,
   because we want to merge gaps in initial smoothing steps */

static List_T
traverse_single_gap (bool *filledp, int *dynprogindex, List_T pairs, List_T *path, 
		     Pair_T leftpair, Pair_T rightpair,
		     Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
		     char *queryaaseq_ptr,
#endif
		     char *queryseq_ptr, char *queryuc_ptr,
		     int cdna_direction, bool watsonp, bool jump_late_p, Pairpool_T pairpool, Dynprog_T dynprog,
		     int maxpeelback, int extraband_single, double defect_rate, int close_indels_mode, 
		     bool forcep) {
  List_T gappairs, peeled_pairs, peeled_path;
  int queryjump, genomejump;
  int querydp5, genomedp5, querydp3, genomedp3;
  int nmatches, nmismatches, nopens, nindels;
  int unknowns, qopens, qindels, topens, tindels, ncanonical, nsemicanonical, nnoncanonical;
  int finalscore, origscore;
  bool mismatchp = false;
  Pair_T gappair;
  double min_splice_prob;
  /* int origqueryjump, origgenomejump; */

  debug(printf("\nTRAVERSE_SINGLE_GAP\n"));
  querydp5 = leftpair->querypos + 1;
  genomedp5 = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5--;
  if (leftpair->genome == ' ') genomedp5--;
  querydp3 = rightpair->querypos - 1;
  genomedp3 = rightpair->genomepos - 1;

  /* origqueryjump = querydp3 - querydp5 + 1; */
  /* origgenomejump = genomedp3 - genomedp5 + 1; */

  /* Used to peelback only half as much as for a paired gap, to save
     on dynamic programming, but not any more. */
  pairs = peel_rightward(&mismatchp,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			 pairpool,
#endif
			 maxpeelback,/*throughmismatchp*/false,/*quit_on_gap_p*/true,
			 /*endgappairs*/NULL,&gappair,
			 /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
  *path = peel_leftward(&mismatchp,&peeled_path,*path,&querydp5,&genomedp5,
#ifdef WASTE
			pairpool,
#endif
			maxpeelback,/*throughmismatchp*/false,/*quit_on_gap_p*/true,
			/*endgappairs*/NULL,&gappair,
			/*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

  queryjump = querydp3 - querydp5 + 1;
  genomejump = genomedp3 - genomedp5 + 1;
  
  if (queryjump <= 0 || genomejump <= 0) {
    debug(printf("Unable to perform dynamic programming\n"));
    *filledp = false;

    pairs = Pairpool_transfer(pairs,peeled_pairs);
    *path = Pairpool_transfer(*path,peeled_path);

    return pairs;

  } else {
    gappairs = Dynprog_single_gap(&(*dynprogindex),&finalscore,
				  &nmatches,&nmismatches,&nopens,&nindels,dynprog,
				  &(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
				  queryjump,genomejump,querydp5,genomedp5,
				  chroffset,chrhigh,
#ifdef PMAP
				  queryaaseq_ptr,
#endif
				  cdna_direction,watsonp,jump_late_p,pairpool,
				  extraband_single,defect_rate,
				  close_indels_mode,/*widebandp*/true);
    debug(Pair_dump_list(gappairs,true));
  }
  debug(printf("  Final score: %d\n",finalscore));

#if 0
  /* Old behavior: Depends on amount peeled */
  if (!forcep && nmismatches + nopens > nmatches) {
    /* Put back peeled pairs */
    debug(printf("Bad alignment, so undoing this solution\n"));
    pairs = Pairpool_transfer(pairs,peeled_pairs);
    *path = Pairpool_transfer(*path,peeled_path);
    *filledp = false;
  } else {
    pairs = Pairpool_transfer(pairs,gappairs);
    *filledp = true;
  }
#else
  /* New behavior: Compares new score to orig score */
  if (forcep == true) {
    /* Intended for build_dual_breaks */
    pairs = Pairpool_transfer(pairs,gappairs);
    *filledp = true;
  } else {
    Pair_fracidentity(&nmatches,&unknowns,&nmismatches,&qopens,&qindels, 
		      &topens,&tindels,&ncanonical,&nsemicanonical,&nnoncanonical,
		      &min_splice_prob,peeled_pairs,cdna_direction);
    origscore = Dynprog_score(nmatches,nmismatches,qopens,qindels,topens,tindels,defect_rate);

    Pair_fracidentity(&nmatches,&unknowns,&nmismatches,&qopens,&qindels, 
		      &topens,&tindels,&ncanonical,&nsemicanonical,&nnoncanonical,
		      &min_splice_prob,peeled_path,cdna_direction);
    origscore += Dynprog_score(nmatches,nmismatches,qopens,qindels,topens,tindels,defect_rate);
    debug(printf("  Orig score: %d, ",origscore));

    queryjump = (rightpair->querypos - leftpair->querypos - 1);
    if (queryjump > 0) {
      origscore += Dynprog_score(/*nmatches*/0,/*nmatches*/0,/*qopens*/1,/*qindels*/queryjump,
				 /*topens*/0,/*tindels*/0,defect_rate);
    }
    genomejump = (rightpair->genomepos - leftpair->genomepos - 1);
    if (genomejump > 0) {
      origscore += Dynprog_score(/*nmatches*/0,/*nmatches*/0,/*qopens*/0,/*qindels*/0,
				 /*topens*/1,/*tindels*/genomejump,defect_rate);
    }
    debug(printf("queryjump = %d, genomejump = %d, Orig score: %d\n",queryjump,genomejump,origscore));

    if (finalscore < 0 || finalscore < origscore) {
      /* Put back peeled pairs */
      debug(printf("Bad alignment, so undoing this solution\n"));
      pairs = Pairpool_transfer(pairs,peeled_pairs);
      *path = Pairpool_transfer(*path,peeled_path);
      *filledp = false;
    } else {
      debug(printf("Good alignment, so accepting this solution\n"));
      pairs = Pairpool_transfer(pairs,gappairs);
      *filledp = true;
    }
  }
#endif

  return pairs;
}

static List_T
traverse_cdna_gap (bool *filledp, bool *incompletep, int *dynprogindex_minor, int *dynprogindex_major,
		   List_T pairs, List_T *path, Pair_T leftpair, Pair_T rightpair,
		   Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
		   char *queryaaseq_ptr,
#endif
		   char *queryseq_ptr, char *queryuc_ptr,
#if 0
		   char *genomicseg_ptr, char *genomicuc_ptr,
#endif
		   int cdna_direction, bool watsonp, bool jump_late_p, Pairpool_T pairpool,
		   Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR, 
		   int maxpeelback, int extramaterial_paired, int extraband_paired, int extraband_single,
		   double defect_rate, int close_indels_mode) {
  List_T gappairs, peeled_pairs = NULL, peeled_path = NULL;
  int queryjump, genomejump;
  int querydp5, genomedp5, querydp3, genomedp3;
  int finalscore;
  int nmatches, nmismatches, nopens, nindels;
  bool mismatchp = false, throughmismatchp;
  Pair_T gappair;

  debug(printf("\nTRAVERSE_CDNA_GAP\n"));
  querydp5 = leftpair->querypos + 1;
  genomedp5 = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5--;
  if (leftpair->genome == ' ') genomedp5--;
  querydp3 = rightpair->querypos - 1;
  genomedp3 = rightpair->genomepos - 1;

  if (leftpair->dynprogindex < 0 && leftpair->dynprogindex == rightpair->dynprogindex) {
    debug(printf("Re-peeling prior solution\n"));
    throughmismatchp = false;
  } else {
    debug(printf("No prior solution\n"));
    throughmismatchp = true;
  }

  pairs = peel_rightward(&mismatchp,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			 pairpool,
#endif
			 maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			 /*endgappairs*/NULL,&gappair,
			 /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
  *path = peel_leftward(&mismatchp,&peeled_path,*path,&querydp5,&genomedp5,
#ifdef WASTE
			pairpool,
#endif
			maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			/*endgappairs*/NULL,&gappair,
			/*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

#if 0
  if (peeled_pairs == NULL || peeled_path == NULL) {
    debug(printf("Skipping this because unable to peel\n"));
    *filledp = false;
    pairs = Pairpool_transfer(pairs,peeled_pairs);
    *path = Pairpool_transfer(*path,peeled_path);
    return pairs;
  }
#endif

  queryjump = querydp3 - querydp5 + 1;
  genomejump = genomedp3 - genomedp5 + 1;

  if (queryjump <= genomejump + MININTRONLEN) {
    debug(printf("Really a single gap, not a cDNA gap\n"));
    gappairs = Dynprog_single_gap(&(*dynprogindex_minor),&finalscore,
				  &nmatches,&nmismatches,&nopens,&nindels,dynprogM,
				  &(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
				  queryjump,genomejump,querydp5,genomedp5,
				  chroffset,chrhigh,
#ifdef PMAP
				  queryaaseq_ptr,
#endif
				  cdna_direction,watsonp,jump_late_p,pairpool,
				  extraband_single,defect_rate,
				  close_indels_mode,/*widebandp*/true);
    debug(Pair_dump_list(gappairs,true));
    debug(printf("  Score: %d\n",finalscore));
    pairs = Pairpool_transfer(pairs,gappairs);
    *filledp = true;

  } else {
    /* Set queryjump approximately equal to genomejump to have square
       dynamic programming matrices */
    queryjump = genomejump + extramaterial_paired;
    gappairs = Dynprog_cdna_gap(&(*dynprogindex_major),&finalscore,&(*incompletep),dynprogL,dynprogR,
				&(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
				&(queryseq_ptr[querydp3]),&(queryuc_ptr[querydp3]),
#if 0
				&(genomicseg_ptr[genomedp5]),&(genomicuc_ptr[genomedp5]),
#endif
				/*length1L*/queryjump,/*length1R*/queryjump,/*length2*/genomejump,
				/*offset1L*/querydp5,/*revoffset1R*/querydp3,/*offset2*/genomedp5,
				chroffset,chrhigh,
#ifdef PMAP
				queryaaseq_ptr,
#endif
				cdna_direction,watsonp,jump_late_p,pairpool,
				extraband_paired,defect_rate);
    debug(Pair_dump_list(gappairs,true));
    *filledp = true;
    if (gappairs == NULL) {
      pairs = Pairpool_transfer(pairs,peeled_pairs);
      *path = Pairpool_transfer(*path,peeled_path);
      pairs = Pairpool_push_gapholder(pairs,pairpool,/*queryjump*/UNKNOWNJUMP,/*genomejump*/UNKNOWNJUMP,/*knownp*/false);
    } else {
      pairs = Pairpool_transfer(pairs,gappairs);
    }
  }

  return pairs;
}


/* genome_gap is usually an intron */
/* Do not set shiftp to false */
static List_T
traverse_genome_gap (bool *filledp, bool *shiftp, int *dynprogindex_minor, int *dynprogindex_major,
		     List_T pairs, List_T *path, Pair_T leftpair, Pair_T rightpair,
		     Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
		     char *queryaaseq_ptr,
#endif
		     char *queryseq_ptr, char *queryuc_ptr,
		     int cdna_direction, bool watsonp, bool jump_late_p,
		     Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		     int maxpeelback, int extramaterial_paired, int extraband_paired, int extraband_single,
		     double defect_rate, int close_indels_mode, bool finalp) {
  List_T gappairs, gappairs_alt, peeled_pairs = NULL, peeled_path = NULL, p;
  Pair_T pair;
  int queryjump, genomejump;
  int querydp5, genomedp5, querydp3, genomedp3;
  int new_leftgenomepos, new_rightgenomepos;
  double left_prob, right_prob, left_prob_alt, right_prob_alt;
  int finalscore, finalscore_alt, nmatches, nmismatches, nopens, nindels, exonhead, introntype;
  int nmismatches_alt;
  int acceptable_nmismatches;
  bool mismatch_rightward_p = false, mismatch_leftward_p = false, throughmismatchp;
  double prob2, prob3;
#ifndef PMAP
  List_T micropairs;
  int microintrontype;
#endif

#ifdef SHORTCUT
  char left1, left2, right2, right1, left1_alt, left2_alt, right2_alt, right1_alt;
#endif
  Pair_T gappair;

  debug(printf("\nTRAVERSE_GENOME_GAP\n"));

  querydp5 = leftpair->querypos + 1;
  genomedp5 = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5--;
  if (leftpair->genome == ' ') genomedp5--;
  querydp3 = rightpair->querypos - 1;
  genomedp3 = rightpair->genomepos - 1;

  if (leftpair->dynprogindex < 0 && leftpair->dynprogindex == rightpair->dynprogindex) {
    debug(printf("Re-peeling prior solution\n"));
    throughmismatchp = false;
  } else {
    debug(printf("No prior solution\n"));
    throughmismatchp = true;
  }

#ifdef SHORTCUT
  queryjump = querydp3 - querydp5 + 1;
  genomejump = genomedp3 - genomedp5 + 1;

  if (querydp5 != querydp3 + 1) {
    pairs = peel_rightward(&mismatch_rightward_p,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			   pairpool,
#endif
			   maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			   /*endgappairs*/NULL,&gappair,
			   /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
    *path = peel_leftward(&mismatch_leftward_p,&peeled_path,*path,&querydp5,&genomedp5,
#ifdef WASTE
			  pairpool,
#endif
			  maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			  /*endgappairs*/NULL,&gappair,
			  /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

  } else {

#if 0
    left1 = genomicuc_ptr[genomedp5];
    left2 = genomicuc_ptr[genomedp5+1];
    right2 = genomicuc_ptr[genomedp3-1];
    right1 = genomicuc_ptr[genomedp3];
#else
    left1 = get_genomic_nt(&left1_alt,genomedp5,chroffset,chrhigh,watsonp);
    left2 = get_genomic_nt(&left2_alt,genomedp5+1,chroffset,chrhigh,watsonp);
    right2 = get_genomic_nt(&right2_alt,genomedp3-1,chroffset,chrhigh,watsonp);
    right1 = get_genomic_nt(&right1_alt,genomedp3,chroffset,chrhigh,watsonp);
#endif

    introntype = Intron_type(left1,left2,right2,right1,
			     left1_alt,left2_alt,right2_alt,right1_alt,
			     cdna_direction);
    debug(printf("Introntype at %u..%u is %s\n",genomedp5-1,genomedp3+1,Intron_type_string(introntype)));

    pairs = peel_rightward(&mismatch_rightward_p,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			   pairpool,
#endif
			   maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			   /*endgappairs*/NULL,&gappair,
			   /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
    *path = peel_leftward(&mismatch_leftward_p,&peeled_path,*path,&querydp5,&genomedp5,
#ifdef WASTE
			  pairpool,
#endif
			  maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			  /*endgappairs*/NULL,&gappair,
			  /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

    if (novelsplicingp == true && mismatch_rightward_p == false && mismatch_leftward_p == false) {
      debug(printf("No mismatches seen\n"));
      if ((cdna_direction > 0 && introntype == GTAG_FWD)
#ifndef PMAP
	  || (cdna_direction < 0 && introntype == GTAG_REV)
#endif
	  ) {
	debug(printf("Skipping because intron is already canonical\n"));
	*filledp = false;		/* Calling procedure will replace the gap */
	pairs = Pairpool_transfer(pairs,peeled_pairs);
	*path = Pairpool_transfer(*path,peeled_path);
	return pairs;
      }
    }
  }

#else
  pairs = peel_rightward(&mismatch_rightward_p,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			 pairpool,
#endif
			 maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			 /*endgappairs*/NULL,&gappair,
			 /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
  *path = peel_leftward(&mismatch_leftward_p,&peeled_path,*path,&querydp5,&genomedp5,
#ifdef WASTE
			pairpool,
#endif
			maxpeelback,throughmismatchp,/*quit_on_gap_p*/false,
			/*endgappairs*/NULL,&gappair,
			/*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
#endif


  queryjump = querydp3 - querydp5 + 1;
  genomejump = genomedp3 - genomedp5 + 1;

  /* genomedp5 + genomejump - 1 >= genomedp3 - genomejump + 1) ?  but doesn't work on AA669154, chr1*/
  if (genomejump <= queryjump + MININTRONLEN) {
    debug(printf("Really a single gap, not an intron\n"));
    gappairs = Dynprog_single_gap(&(*dynprogindex_minor),&finalscore,
				  &nmatches,&nmismatches,&nopens,&nindels,dynprogM,
				  &(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
				  queryjump,genomejump,querydp5,genomedp5,
				  chroffset,chrhigh,
#ifdef PMAP
				  queryaaseq_ptr,
#endif
				  cdna_direction,watsonp,jump_late_p,pairpool,
				  extraband_single,defect_rate,
				  close_indels_mode,/*widebandp*/true);
    debug(Pair_dump_list(gappairs,true));
    debug(printf("  Score: %d\n",finalscore));

    pairs = Pairpool_transfer(pairs,gappairs);
    *filledp = true;

  } else {
    /* Set genomejump approximately equal to queryjump to have square
       dynamic programming matrices */
    genomejump = queryjump + extramaterial_paired;
    gappairs = Dynprog_genome_gap(&(*dynprogindex_major),&finalscore,&new_leftgenomepos,&new_rightgenomepos,
				  &left_prob,&right_prob,&nmatches,&nmismatches,&nopens,&nindels,
				  &exonhead,&introntype,dynprogL,dynprogR,
				  &(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
				  queryjump,genomejump,genomejump,querydp5,genomedp5,genomedp3,
				  chrnum,chroffset,chrhigh,
#ifdef PMAP
				  queryaaseq_ptr,
#endif
				  cdna_direction,watsonp,jump_late_p,pairpool,extraband_paired,
				  defect_rate,maxpeelback,/*halfp*/false,finalp,
				  /*use_probabilities_p*/false,/*score_threshold*/0,
				  splicingp);

    if (gappairs == NULL) {
      /* *shiftp = false; */
      debug(printf("No shift, since gappairs is NULL: intron is disallowed?\n"));

    } else if (new_leftgenomepos != (int) leftpair->genomepos || new_rightgenomepos != (int) rightpair->genomepos) {
      *shiftp = true;
      debug(printf("Shift in intron location from %d..%d to %d..%d\n",
		   leftpair->genomepos,rightpair->genomepos,new_leftgenomepos,new_rightgenomepos));
    } else {
      /* *shiftp = false; */
      debug(printf("No shift in intron location\n"));
    }
    debug(Pair_dump_list(gappairs,true));
    debug(printf("  gappairs score: %d\n",finalscore));
    debug(fprintf(stderr,"  gappairs score: %d\n",finalscore));

    /* prob = 1.0 - (1.0 - left_prob)*(1.0 - right_prob); */
    if (finalp == true && novelsplicingp == true && (left_prob < 0.90 || right_prob < 0.90)) {
      /* Bad intron.  See if alternative with indel is better.  Check
	 only on finalp, because earlier steps may need to iterate. */
      debug(printf("Checking alternative because found a bad intron with probs %f and %f\n",
		   left_prob,right_prob));
      gappairs_alt = Dynprog_genome_gap(&(*dynprogindex_major),&finalscore_alt,&new_leftgenomepos,&new_rightgenomepos,
					&left_prob_alt,&right_prob_alt,&nmatches,&nmismatches_alt,&nopens,&nindels,
					&exonhead,&introntype,dynprogL,dynprogR,
					&(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
					queryjump,genomejump,genomejump,querydp5,genomedp5,genomedp3,
					chrnum,chroffset,chrhigh,
#ifdef PMAP
					queryaaseq_ptr,
#endif
					cdna_direction,watsonp,jump_late_p,pairpool,extraband_paired,
					defect_rate,maxpeelback,/*halfp*/false,/*finalp*/true,
					/*use_probabilities_p*/true,/*score_threshold*/finalscore + QOPEN + 3*QINDEL,
					splicingp);

      debug(Pair_dump_list(gappairs_alt,true));
      debug(printf("  gappairs_alt score: %d, left prob %f, right prob %f\n",finalscore_alt,left_prob_alt,right_prob_alt));
      debug(fprintf(stderr,"  gappairs_alt score: %d\n",finalscore_alt));
      if (gappairs_alt != NULL && left_prob_alt > left_prob && right_prob_alt > right_prob) {
	debug(printf(" switching to alt\n"));
	gappairs = gappairs_alt;
      } else {
	debug(printf(" keeping original\n"));
      }
    }

    if (defect_rate < DEFECT_HIGHQ) {
      acceptable_nmismatches = 2;
    } else if (defect_rate < DEFECT_MEDQ) {
      acceptable_nmismatches = 2;
    } else {
      acceptable_nmismatches = 3;
    }

    debug(printf("nmismatches = %d, nopens = %d, nindels = %d.  acceptable nmismatches = %d\n",
		 nmismatches,nopens,nindels,acceptable_nmismatches));
    
    if (gappairs == NULL) {
      *filledp = false;
      /* Put back peeled pairs, but mark pairs as disallowed */
      debug(printf("gappairs is false, so disallowed\n"));
      for (p = peeled_pairs; p != NULL; p = p->rest) {
	pair = (Pair_T) p->first;
	pair->disallowedp = true;
      }
      for (p = peeled_path; p != NULL; p = p->rest) {
	pair = (Pair_T) p->first;
	pair->disallowedp = true;
      }

      pairs = Pairpool_transfer(pairs,peeled_pairs);
      *path = Pairpool_transfer(*path,peeled_path);
      introntype = NONINTRON;

    } else if (!finalp && finalscore < 0) {
      *filledp = false;
      /* Put back peeled pairs */
      debug(printf("Not forced and finalscore is negative\n"));
      pairs = Pairpool_transfer(pairs,peeled_pairs);
      *path = Pairpool_transfer(*path,peeled_path);
      introntype = NONINTRON;

#if 0
    } else if (false && defect_rate > DEFECT_MEDQ) {
      /* Don't look for microexons in low-quality sequences */
      debug(printf("Don't look for microexon in low-quality sequence\n"));
      *filledp = true;
      pairs = Pairpool_transfer(pairs,gappairs);
#endif

    } else if (introntype != NONINTRON && nmismatches <= acceptable_nmismatches && nopens <= 1 && nindels <= 3) {
      debug(printf("introntype != NONINTRON and nmismatches, nopens, nindels low\n"));
      *filledp = true;
      pairs = Pairpool_transfer(pairs,gappairs);

    } else {
#ifdef PMAP
      *filledp = true;
      pairs = Pairpool_transfer(pairs,gappairs);
#else      
      *filledp = true;
      debug(printf("Calling microexon because introntype == %d or nmismatches %d > acceptable %d or nopens %d > 1 or nindels %d > 3\n",
		   introntype,nmismatches,acceptable_nmismatches,nopens,nindels));
      micropairs = Dynprog_microexon_int(&prob2,&prob3,&(*dynprogindex_major),&microintrontype,
					 /*sequence1*/&(queryseq_ptr[querydp5]),
					 /*sequenceuc1*/&(queryuc_ptr[querydp5]),
					 /*length1*/queryjump,/*length2L*/genomejump,/*length2R*/genomejump,
					 /*offset1*/querydp5,/*offset2L*/genomedp5,/*revoffset2R*/genomedp3,
					 cdna_direction,
#ifdef PMAP
					 queryaaseq_ptr,genomicuc_ptr,
#endif
					 queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,
					 pairpool,defect_rate);
      
      if (micropairs == NULL) {
	debug(printf("No microexon found\n"));
	pairs = Pairpool_transfer(pairs,gappairs);
	/* *shiftp = false; */
      } else {
	debug(printf("Microexon found with probs %f and %f:\n",prob2,prob3));
	debug(Pair_dump_list(micropairs,/*zerobasedp*/true));
	debug(printf("\n"));

	if (nindels == 0 && nmismatches < 4) {
	  /* Have a higher standard */
	  if (prob2 >= 0.95 && prob3 >= 0.95) {
	    debug(printf("Transferring microexon pairs\n"));
	    pairs = Pairpool_transfer(pairs,micropairs);
	    introntype = microintrontype;
	    *shiftp = true;
	  } else {
	    pairs = Pairpool_transfer(pairs,gappairs);
	  }
	} else {
	  /* Have a lower standard */
	  if (prob2 >= 0.90 || prob3 >= 0.90) {
	    debug(printf("Transferring microexon pairs\n"));
	    pairs = Pairpool_transfer(pairs,micropairs);
	    introntype = microintrontype;
	    *shiftp = true;
	  } else {
	    pairs = Pairpool_transfer(pairs,gappairs);
	  }
	}
      }
#endif
    }
  }

  return pairs;
}


static List_T
traverse_dual_genome_gap (int *dynprogindex, List_T pairs, List_T *path, 
			  Pair_T leftpair, Pair_T rightpair, bool left_end_intron_p, bool right_end_intron_p,
			  Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
			  int midquerypos, int midgenomepos, 
#ifdef PMAP
			  char *queryaaseq_ptr,
#endif
			  char *queryseq_ptr, char *queryuc_ptr, int cdna_direction, bool watsonp,
			  bool jump_late_p, Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogR, 
			  int maxpeelback, int nullgap, int extramaterial_paired, int extraband_paired,
			  double defect_rate) {
  List_T single_gappairs, dual_gappairs_1 = NULL, dual_gappairs_2 = NULL,
    right_gappairs = NULL, left_gappairs = NULL, peeled_pairs, peeled_path;
  int queryjump, genomejump;
  int querydp5, genomedp5, querydp3, genomedp3;
  int new_leftgenomepos, new_rightgenomepos;
  double left_prob, right_prob;
  int querydp5_dual, querydp3_dual, genomedp5_dual, genomedp3_dual;
  int querydp5_left, querydp3_left, genomedp5_left, genomedp3_left;
  int querydp5_right, querydp3_right, genomedp5_right, genomedp3_right;
  int single_nmatches = 0, dual_nmatches_1 = 0, dual_nmatches_2 = 0, left_nmatches = 0, right_nmatches = 0;
  int single_score, dual_score_1, dual_score_2, single_goodness, dual_goodness, 
    nmismatches, nopens, nindels, exonhead, right_exonhead, left_exonhead;
  int left_score, right_score, left_goodness = 0, right_goodness = 0;
  int middle_exonlength, interexon_region;
  int single_introntype, dual_introntype_1, dual_introntype_2, left_introntype, right_introntype,
    canonical_introntype, semicanonical_introntype_1, semicanonical_introntype_2;
  double middle_exonprob;
  bool singlep = false, mismatchp = false, single_canonical_p, dual_canonical_p;
  Pair_T gappair;

  debug(printf("\nTRAVERSE_DUAL_GENOME_GAP: left_end_intron_p %d, right_end_intron_p %d\n",
	       left_end_intron_p,right_end_intron_p));
  if (cdna_direction > 0) {
    canonical_introntype = GTAG_FWD;
    semicanonical_introntype_1 = ATAC_FWD;
    semicanonical_introntype_2 = GCAG_FWD;
#ifndef PMAP
  } else {
    canonical_introntype = GTAG_REV;
    semicanonical_introntype_1 = ATAC_REV;
    semicanonical_introntype_2 = GCAG_REV;
#endif
  }

  querydp5 = leftpair->querypos + 1;
  genomedp5 = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5--;
  if (leftpair->genome == ' ') genomedp5--;
  querydp3 = rightpair->querypos - 1;
  genomedp3 = rightpair->genomepos - 1;

  pairs = peel_rightward(&mismatchp,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			 pairpool,
#endif
			 maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
			 /*endgappairs*/NULL,&gappair,
			 /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
  *path = peel_leftward(&mismatchp,&peeled_path,*path,&querydp5,&genomedp5,
#ifdef WASTE
			pairpool,
#endif
			maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
			/*endgappairs*/NULL,&gappair,
			/*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

  queryjump = querydp3 - querydp5 + 1;
  genomejump = queryjump + extramaterial_paired;

  if (queryjump > nullgap) {
    pairs = Pairpool_transfer(pairs,peeled_pairs);
    *path = Pairpool_transfer(*path,peeled_path);

    /* No need to compute queryjump and genomejump */
    /*
      leftpair = (*path)->first;
      rightpair = pairs->first;
      queryjump = rightpair->querypos - leftpair->querypos - 1;
      genomejump = rightpair->genomepos - leftpair->genomepos - 1;
      if (leftpair->cdna == ' ') queryjump++;
      if (leftpair->genome == ' ') genomejump++;
    */

    pairs = Pairpool_push_gapholder(pairs,pairpool,/*queryjump*/UNKNOWNJUMP,/*genomejump*/UNKNOWNJUMP,/*knownp*/false);

    return pairs;
  }

  if (genomedp5 + genomejump - 1 >= genomedp3 - genomejump + 1) {
    debug(printf("Bounds don't make sense for dual intron gap: %d + %d - 1 >= %d - %d + 1\n\n",
		 genomedp5,genomejump,genomedp3,genomejump));
    pairs = Pairpool_transfer(pairs,peeled_pairs);
    *path = Pairpool_transfer(*path,peeled_path);

    /* No need to compute queryjump and genomejump */
    /*
      leftpair = (*path)->first;
      rightpair = pairs->first;
      queryjump = rightpair->querypos - leftpair->querypos - 1;
      genomejump = rightpair->genomepos - leftpair->genomepos - 1;
      if (leftpair->cdna == ' ') queryjump++;
      if (leftpair->genome == ' ') genomejump++;
    */

    pairs = Pairpool_push_gapholder(pairs,pairpool,/*queryjump*/UNKNOWNJUMP,/*genomejump*/UNKNOWNJUMP,/*knownp*/false);

    return pairs;
  }
  
  single_gappairs = Dynprog_genome_gap(&(*dynprogindex),&single_score,&new_leftgenomepos,&new_rightgenomepos,
				       &left_prob,&right_prob,&single_nmatches,&nmismatches,&nopens,&nindels,
				       &exonhead,&single_introntype,dynprogL,dynprogR,
				       &(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
				       queryjump,genomejump,genomejump,querydp5,genomedp5,genomedp3,
				       chrnum,chroffset,chrhigh,
#ifdef PMAP
				       queryaaseq_ptr,
#endif
				       cdna_direction,watsonp,jump_late_p,pairpool,extraband_paired,
				       defect_rate,maxpeelback,/*halfp*/false,/*finalp*/false,
				       /*use_probabilities_p*/false,/*score_threshold*/0,splicingp);

  debug(Pair_check_list(single_gappairs));

  /* Okay to have one indel, because may need to shift an island */
  if (nopens <= 1) {
    single_goodness = (single_nmatches + nindels) + MISMATCH*nmismatches;
  } else {
    single_goodness = single_nmatches + MISMATCH*nmismatches + QOPEN*(nopens-1) + QINDEL*nindels;
  }

  if (single_introntype == canonical_introntype) {
    single_canonical_p = true;
  } else if (single_introntype == semicanonical_introntype_1 ||
	     single_introntype == semicanonical_introntype_2) {
    single_canonical_p = false;
  } else {
    single_canonical_p = false;
  }


  /* Right of short exon */
  querydp5_dual = midquerypos;
  genomedp5_dual = midgenomepos;
  querydp3_dual = querydp3;	/* From peel_rightward */
  genomedp3_dual = genomedp3;	/* From peel_rightward */

  queryjump = querydp3_dual - querydp5_dual + 1;
  genomejump = queryjump + extramaterial_paired;

  if (genomedp5_dual + genomejump - 1 >= genomedp3_dual) {
    /* Bounds don't make sense */
    debug(printf("Bounds don't make sense on right of dual intron gap: %d + %d - 1 >= %d\n\n",
		 genomedp5_dual,genomejump,genomedp3_dual));
    dual_gappairs_2 = NULL;

  } else {
    dual_gappairs_2 = Dynprog_genome_gap(&(*dynprogindex),&dual_score_2,&new_leftgenomepos,&new_rightgenomepos,
					 &left_prob,&right_prob,&dual_nmatches_2,&nmismatches,&nopens,&nindels,
					 &right_exonhead,&dual_introntype_2,dynprogL,dynprogR,
					 &(queryseq_ptr[querydp5_dual]),&(queryuc_ptr[querydp5_dual]),
					 queryjump,genomejump,genomejump,
					 querydp5_dual,genomedp5_dual,genomedp3_dual,
					 chrnum,chroffset,chrhigh,
#ifdef PMAP
					 queryaaseq_ptr,
#endif
					 cdna_direction,watsonp,jump_late_p,pairpool,extraband_paired,
					 defect_rate,maxpeelback,/*halfp*/true,/*finalp*/false,
					 /*use_probabilities_p*/false,/*score_threshold*/0,splicingp);

    dual_goodness = dual_nmatches_2 + MISMATCH*nmismatches + QOPEN*nopens + QINDEL*nindels;

    /* Left of short exon */
    querydp5_dual = querydp5;	/* From peel_leftward */
    genomedp5_dual = genomedp5;	/* From peel_leftward */
    querydp3_dual = midquerypos-1;
    genomedp3_dual = midgenomepos-1;

    queryjump = querydp3_dual - querydp5_dual + 1;
    genomejump = queryjump + extramaterial_paired;

    if (genomedp5_dual + genomejump - 1 >= genomedp3_dual) {
      /* Bounds don't make sense */
      debug(printf("Bounds don't make sense on left of dual intron gap: %d + %d - 1 >= %d\n\n",
		   genomedp5_dual,genomejump,genomedp3_dual));
      dual_gappairs_1 = NULL;

    } else {
      dual_gappairs_1 = Dynprog_genome_gap(&(*dynprogindex),&dual_score_1,&new_leftgenomepos,&new_rightgenomepos,
					   &left_prob,&right_prob,&dual_nmatches_1,&nmismatches,&nopens,&nindels,
					   &left_exonhead,&dual_introntype_1,dynprogL,dynprogR,
					   &(queryseq_ptr[querydp5_dual]),&(queryuc_ptr[querydp5_dual]),
					   queryjump,genomejump,genomejump,
					   querydp5_dual,genomedp5_dual,genomedp3_dual,
					   chrnum,chroffset,chrhigh,
#ifdef PMAP
					   queryaaseq_ptr,
#endif
					   cdna_direction,watsonp,jump_late_p,pairpool,extraband_paired,
					   defect_rate,maxpeelback,/*halfp*/true,/*finalp*/false,
					   /*use_probabilities_p*/false,/*score_threshold*/0,splicingp);
      
      dual_goodness += dual_nmatches_1 + MISMATCH*nmismatches + QOPEN*nopens + QINDEL*nindels;

      if (dual_introntype_1 == canonical_introntype && dual_introntype_2 == canonical_introntype) {
	dual_canonical_p = true;
      } else {
	dual_canonical_p = false;
      }
    }
  }

  if (dual_gappairs_2 == NULL || dual_gappairs_1 == NULL) {
    debug(printf("Single score wins\n"));
    debug(printf("Loser: dual_gappairs\n"));
    debug(Pair_dump_list(dual_gappairs_2,true));
    debug(Pair_dump_list(dual_gappairs_1,true));
    debug(printf("Winner: single gap pairs\n"));
    debug(Pair_dump_list(single_gappairs,true));
    /* pairs = Pairpool_transfer(pairs,single_gappairs); -- Wait until we check for left_goodness and right_goodness */
    singlep = true;

  } else {
    middle_exonlength = right_exonhead-left_exonhead;
    debug(printf("Middle exon is %d - %d = %d bp in interexon region of %d bp\n",
		 right_exonhead,left_exonhead,right_exonhead-left_exonhead,new_rightgenomepos-new_leftgenomepos));
    if (middle_exonlength <= 0) {
      middle_exonprob = 0.0;
    } else {
      interexon_region = new_rightgenomepos - new_leftgenomepos;
      
      if (dual_introntype_2 == canonical_introntype) {
	middle_exonlength += DUAL_HALFCANONICAL_POINTS;
	debug(printf("Add canonical credit of %d for right intron\n",DUAL_HALFCANONICAL_POINTS));
      }
      if (dual_introntype_1 == canonical_introntype) {
	middle_exonlength += DUAL_HALFCANONICAL_POINTS;
	debug(printf("Add canonical credit of %d for left intron\n",DUAL_HALFCANONICAL_POINTS));
      }

      middle_exonprob = 1.0-pow(1.0-pow(4.0,-(double) middle_exonlength),(double) interexon_region);
      
      debug(printf("Single score = %d (%d matches).  Single canonical: %d.  Dual score = %d & %d (%d & %d matches).  Dual canonical: %d/%d.  ",
		   single_score,single_nmatches,single_canonical_p,
		   dual_score_1,dual_score_2,dual_nmatches_1,dual_nmatches_2,
		   dual_introntype_1 == canonical_introntype,
		   dual_introntype_2 == canonical_introntype));
      debug(printf("Single goodness = %d.  Dual goodness = %d.  ",
		   single_goodness,dual_goodness));
      debug(printf("Probability is %g.  ",middle_exonprob));
    }

    /* Want high threshold for accepting dual intron */
    if (dual_canonical_p == true && middle_exonprob < 0.001 &&
	(single_canonical_p == false || single_goodness <= dual_goodness)) {
      debug(printf("Dual scores win\n"));
      debug(printf("Loser: single_gappairs\n"));
      debug(Pair_dump_list(single_gappairs,true));
      debug(printf("Winner: Transferring dual_gappairs_2 onto pairs\n"));
      debug(Pair_dump_list(dual_gappairs_2,true));
      pairs = Pairpool_transfer(pairs,dual_gappairs_2);
      debug(printf("Winner: Transferring dual_gappairs_1 onto pairs\n"));
      debug(Pair_dump_list(dual_gappairs_1,true));
      pairs = Pairpool_transfer(pairs,dual_gappairs_1);
    } else {
      debug(printf("Single score wins\n"));
      debug(printf("Loser: dual_gappairs\n"));
      debug(Pair_dump_list(dual_gappairs_2,true));
      debug(Pair_dump_list(dual_gappairs_1,true));
      debug(printf("Winner: single gappairs\n"));
      debug(Pair_dump_list(single_gappairs,true));
      /* pairs = Pairpool_transfer(pairs,single_gappairs); -- Wait until we check for left_goodness and right_goodness */
      singlep = true;
    }
  }

  if (singlep == true) {
    if (right_end_intron_p == true) {
      /* Keep left intron only and extend right from short exon */
      querydp5_right = querydp5;
      genomedp5_right = genomedp5;
      querydp3_right = midquerypos;
      genomedp3_right = midgenomepos;

      queryjump = querydp3_right - querydp5_right + 1;
      genomejump = queryjump + extramaterial_paired;

      if (genomedp5_right + genomejump - 1 >= genomedp3_right) {
	/* Bounds don't make sense */
	debug(printf("Bounds don't make sense if we omit right intron: %d + %d - 1 >= %d\n\n",
		     genomedp5_right,genomejump,genomedp3_right));
	right_gappairs = NULL;

      } else {
	right_gappairs = Dynprog_genome_gap(&(*dynprogindex),&right_score,&new_leftgenomepos,&new_rightgenomepos,
					    &left_prob,&right_prob,&right_nmatches,&nmismatches,&nopens,&nindels,
					    &right_exonhead,&right_introntype,dynprogL,dynprogR,
					    &(queryseq_ptr[querydp5_right]),&(queryuc_ptr[querydp5_right]),
					    queryjump,genomejump,genomejump,
					    querydp5_right,genomedp5_right,genomedp3_right,
					    chrnum,chroffset,chrhigh,
#ifdef PMAP
					    queryaaseq_ptr,
#endif
					    cdna_direction,watsonp,jump_late_p,pairpool,extraband_paired,
					    defect_rate,maxpeelback,/*halfp*/false,/*finalp*/false,
					    /*use_probabilities_p*/false,/*score_threshold*/0,splicingp);

	right_goodness = right_nmatches + MISMATCH*nmismatches + QOPEN*nopens + QINDEL*nindels;
	debug(printf("Right goodness (keeping left intron only) = %d\n",right_goodness));

	if (right_goodness > single_goodness) {
	  debug(printf("New winner: right gappairs\n"));
	  debug(Pair_dump_list(right_gappairs,true));
	  single_gappairs = right_gappairs;
	  single_goodness = right_goodness;
	}
      }
    }

    if (left_end_intron_p == true) {
      /* Keep right intron only and extend left from short exon */
      querydp5_left = midquerypos;
      genomedp5_left = midgenomepos;
      querydp3_left = querydp3;
      genomedp3_left = genomedp3;

      queryjump = querydp3_left - querydp5_left + 1;
      genomejump = queryjump + extramaterial_paired;

      if (genomedp5_left + genomejump - 1 >= genomedp3_left) {
	/* Bounds don't make sense */
	debug(printf("Bounds don't make sense if we omit left intron: %d + %d - 1 >= %d\n\n",
		     genomedp5_left,genomejump,genomedp3_left));
	left_gappairs = NULL;

      } else {
	left_gappairs = Dynprog_genome_gap(&(*dynprogindex),&left_score,&new_leftgenomepos,&new_rightgenomepos,
					   &left_prob,&right_prob,&left_nmatches,&nmismatches,&nopens,&nindels,
					   &left_exonhead,&left_introntype,dynprogL,dynprogR,
					   &(queryseq_ptr[querydp5_left]),&(queryuc_ptr[querydp5_left]),
					   queryjump,genomejump,genomejump,
					   querydp5_left,genomedp5_left,genomedp3_left,
					   chrnum,chroffset,chrhigh,
#ifdef PMAP
					   queryaaseq_ptr,
#endif
					   cdna_direction,watsonp,jump_late_p,pairpool,extraband_paired,
					   defect_rate,maxpeelback,/*halfp*/false,/*finalp*/false,
					   /*use_probabilities_p*/false,/*score_threshold*/0,splicingp);
      

	left_goodness = left_nmatches + MISMATCH*nmismatches + QOPEN*nopens + QINDEL*nindels;
	debug(printf("Left goodness (keeping right intron only) = %d\n",left_goodness));

	if (left_goodness > single_goodness) {
	  debug(printf("New winner: left gappairs\n"));
	  debug(Pair_dump_list(left_gappairs,true));
	  single_gappairs = left_gappairs;
	  single_goodness = left_goodness;
	}
      }
    }

    /* Finally transfer best single result */
    pairs = Pairpool_transfer(pairs,single_gappairs);

  }

  return pairs;
}


static bool
good_end_intron_p (Pair_T gappair, int cdna_direction) {
  if (gappair->knowngapp == true) {
    return true;

  } else if (cdna_direction > 0) {
    if (gappair->comp == FWD_CANONICAL_INTRON_COMP || (gappair->donor_prob >= 0.90 && gappair->acceptor_prob >= 0.90)) {
      return true;
    } else {
      return false;
    }
  } else if (cdna_direction < 0) {
    if (gappair->comp == REV_CANONICAL_INTRON_COMP || (gappair->donor_prob >= 0.90 && gappair->acceptor_prob >= 0.90)) {
      return true;
    } else {
      return false;
    }
  } else {
    if (gappair->comp == FWD_CANONICAL_INTRON_COMP || gappair->comp == REV_CANONICAL_INTRON_COMP || 
	(gappair->donor_prob >= 0.90 && gappair->acceptor_prob >= 0.90)) {
      return true;
    } else {
      return false;
    }
  }
}


/* Note on QUERYEND_INDELS.  Profiling shows that using
   QUERYEND_INDELS caused compute_scores_lookup to be called too
   often, slowing program down. */


/* to_queryend_p must be true for distalmedial_ending, since we are
   comparing alternatives.  But it can be false for extend_ending,
   which just tries to improve the ends. */

static List_T
distalmedial_ending5 (bool *knownsplicep, bool *chop_exon_p, int *dynprogindex_minor,
		      int *finalscore, int *ambig_end_length, Splicetype_T *ambig_splicetype, List_T *pairs, 
		      int leftquerypos, int leftgenomepos, Pair_T rightpair,
		      Univcoord_T chroffset, Univcoord_T chrhigh,
		      Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		      int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
		      char *queryaaseq_ptr,
#endif
		      char *queryseq_ptr, char *queryuc_ptr,
		      int cdna_direction, bool watsonp, bool jump_late_p, Pairpool_T pairpool,
		      Dynprog_T dynprog, int maxpeelback, int extramaterial_end,
		      int extraband_end, double defect_rate) {
  List_T peeled_pairs, endgappairs, continuous_gappairs_medialgap = NULL;
  int queryjump, genomejump;
  int querydp5, genomedp5, querydp3_distalgap, genomedp3_distalgap, querydp3_medialgap, genomedp3_medialgap;
  int continuous_goodness_distalgap = 0, continuous_goodness_medialgap = 0,
    nmatches, nmismatches, nopens, nindels;
  bool mismatchp = false;
  Pair_T gappair;
  bool knownsplice_medial_p = false;
  Endalign_T endalign;

  debug(printf("\nDISTALMEDIAL_ENDING5\n"));

  querydp5 = leftquerypos + 1;
  genomedp5 = leftgenomepos + 1; /* 0 */
  querydp3_distalgap = querydp3_medialgap = rightpair->querypos - 1;
  genomedp3_distalgap = genomedp3_medialgap = rightpair->genomepos - 1;

  /* Used to peelback only half as much as for a paired gap, to save
     on dynamic programming, but not any more. */
  *pairs = peel_rightward(&mismatchp,&peeled_pairs,*pairs,&querydp3_distalgap,&genomedp3_distalgap,
#ifdef WASTE
			  pairpool,
#endif
			  maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
			  &endgappairs,&gappair,
			  &querydp3_medialgap,&genomedp3_medialgap);
  if (endgappairs == NULL) {
    *chop_exon_p = false;
    return peeled_pairs;

  } else {
#if 0
    continuous_goodness_distalgap = Pair_fracidentity_max(&changepoint,peeled_pairs,cdna_direction);
#else
    continuous_goodness_distalgap = Pair_fracidentity_score(peeled_pairs,cdna_direction);
    /* continuous_goodness_distalgap += Pair_fracidentity_score(endgappairs,cdna_direction); */
#endif
    debug(printf("continuous_goodness_distalgap (%d+%d pairs) is %d, with gapcomp '%c' with probs %f and %f\n",
		 List_length(peeled_pairs),List_length(endgappairs),continuous_goodness_distalgap,
		 gappair->comp,gappair->donor_prob,gappair->acceptor_prob));

    if (good_end_intron_p(gappair,cdna_direction) == false) {
      debug(printf("Subtracting points from continuous distal because noncanonical\n"));
      continuous_goodness_distalgap -= CANONICAL_POINTS;
    } else if (gappair->comp == DUALBREAK_COMP) {
      debug(printf("Subtracting points from continuous distal because of dual break\n"));
      continuous_goodness_distalgap -= (CANONICAL_POINTS + CANONICAL_POINTS);
    }

    /* Solve if gap were not present */
    queryjump = querydp3_medialgap - querydp5 + 1;
    genomejump = queryjump + extramaterial_end; /* proposed */
    /* Previously, we limited genomejump = min(2*queryjump,queryjump+extramaterial_end) */

    genomedp5 = genomedp3_medialgap - genomejump + 1;
#ifdef EXTRACT_GENOMICSEG
    /* Make sure we don't go past the beginning */
    if (genomedp5 < 0) {
      genomedp5 = 0;
      genomejump = genomedp3_medialgap - genomedp5 + 1;
    }
#endif

    debug(printf("Stage 3 (dir %d): traverse_ending5: Dynamic programming at 5' end (medial to gap): querydp5 = %d, querydp3 = %d, genomedp5 = %d, genomedp3 = %d\n",
		 cdna_direction,querydp5,querydp3_medialgap,genomedp5,genomedp3_medialgap));

    if (0 /* genomedp3_medialgap > genomiclength */) {
      debug(printf("Not feasible to do medial gap\n"));
      *ambig_end_length = 0;

      *pairs = Pairpool_transfer(*pairs,endgappairs);
      *chop_exon_p = false;
      /* Let previous value of knownsplicep stand */

      return peeled_pairs;

    } else {
      debug(printf("Before solving the 5' end, here are the pairs:\n"));
      debug(Pair_dump_list(*pairs,true));
      debug(printf("\n"));

#ifdef PMAP
      continuous_gappairs_medialgap = Dynprog_end5_gap(&(*dynprogindex_minor),&(*finalscore),
						       &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						       &(queryseq_ptr[querydp3_medialgap]),&(queryuc_ptr[querydp3_medialgap]),
						       queryjump,genomejump,querydp3_medialgap,genomedp3_medialgap,
						       chroffset,chrhigh,queryaaseq_ptr,cdna_direction,watsonp,jump_late_p,pairpool,
						       extraband_end,defect_rate,/*endalign*/QUERYEND_INDELS);
      *ambig_end_length = 0;


#else
      if (0 && splicesites != NULL) {
	/* Use only for extend_ending5 */
	continuous_gappairs_medialgap = Dynprog_end5_known(&knownsplice_medial_p,&(*dynprogindex_minor),&(*finalscore),
							   &(*ambig_end_length),&(*ambig_splicetype),
							   &nmatches,&nmismatches,&nopens,&nindels,dynprog,
							   &(queryseq_ptr[querydp3_medialgap]),&(queryuc_ptr[querydp3_medialgap]),
							   queryjump,genomejump,querydp3_medialgap,genomedp3_medialgap,
							   chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
							   cutoff_level,queryptr,querylength,query_compress,
#endif
#endif
							   cdna_direction,watsonp,jump_late_p,pairpool,
							   extraband_end,defect_rate);
      } else {
	continuous_gappairs_medialgap = Dynprog_end5_gap(&(*dynprogindex_minor),&(*finalscore),
							 &nmatches,&nmismatches,&nopens,&nindels,dynprog,
							 &(queryseq_ptr[querydp3_medialgap]),&(queryuc_ptr[querydp3_medialgap]),
							 queryjump,genomejump,querydp3_medialgap,genomedp3_medialgap,
							 chroffset,chrhigh,cdna_direction,watsonp,jump_late_p,pairpool,
							 extraband_end,defect_rate,/*endalign*/QUERYEND_INDELS);
	*ambig_end_length = 0;
      }
#endif

      continuous_goodness_medialgap = nmatches + MISMATCH*nmismatches + QOPEN*nopens + QINDEL*nindels;
      debug(printf("Continuous_goodness_medialgap %d = %d + %d*%d + %d*%d + %d*%d\n",
		   continuous_goodness_medialgap,nmatches,MISMATCH,nmismatches,QOPEN,nopens,QINDEL,nindels));

      if (continuous_goodness_distalgap > continuous_goodness_medialgap) {
	debug(printf("Continuous distal wins: %d > %d\n",continuous_goodness_distalgap,continuous_goodness_medialgap));
	*ambig_end_length = 0;

#if 0
	debug(printf("Before transferring endgappairs, here is pairs:\n"));
	debug(Pair_dump_list(*pairs,true));
	debug(printf("\n"));
#endif

	*pairs = Pairpool_transfer(*pairs,endgappairs);
	*chop_exon_p = false;
	/* Let previous value of knownsplicep stand */
#if 0
	changepoint = Pair_fracidentity_changepoint(peeled_pairs,cdna_direction);
	return List_truncate(peeled_pairs,changepoint);
#else
	debug(printf("Returning peeled pairs:\n"));
	debug(Pair_dump_list(peeled_pairs,true));
	debug(printf("\n"));
	return peeled_pairs;
#endif
      } else {
	debug(printf("Continuous medial wins: %d > %d\n",
		     continuous_goodness_medialgap,continuous_goodness_distalgap));
	*chop_exon_p = true;
	*knownsplicep = knownsplice_medial_p;
	return continuous_gappairs_medialgap;
      }
    }
  }
}


static List_T
extend_ending5 (bool *knownsplicep, int *dynprogindex_minor,
		int *finalscore, int *ambig_end_length, Splicetype_T *ambig_splicetype,
		List_T *pairs, int leftquerypos, int leftgenomepos, Pair_T rightpair,
		Univcoord_T chroffset, Univcoord_T chrhigh,
		Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
		char *queryaaseq_ptr,
#endif
		char *queryseq_ptr, char *queryuc_ptr,
		int cdna_direction, bool watsonp, bool jump_late_p, Pairpool_T pairpool,
		Dynprog_T dynprog, int maxpeelback, int extramaterial_end,
		int extraband_end, double defect_rate, Endalign_T endalign) {
  List_T continuous_gappairs_distalgap = NULL, peeled_pairs;
  int queryjump, genomejump;
  int querydp5, genomedp5, querydp3_distalgap, genomedp3_distalgap;
  int nmatches, nmismatches, nopens, nindels;
  bool mismatchp = false;
  Pair_T gappair, firstpair;


  debug(printf("\nEXTEND_ENDING5 with endalign %s and maxpeelback %d\n",
	       Dynprog_endalign_string(endalign),maxpeelback));

  querydp5 = leftquerypos + 1;
  genomedp5 = leftgenomepos + 1; /* 0 */
  querydp3_distalgap = rightpair->querypos - 1;
  genomedp3_distalgap = rightpair->genomepos - 1;

  /* Used to peelback only half as much as for a paired gap, to save
     on dynamic programming, but not any more. */

  if (endalign == QUERYEND_NOGAPS) {
    /* Don't peelback on extension */
  } else if (maxpeelback == 0) {
    /* Don't peelback on BEST_LOCAL after trim_ends */
  } else {
    *pairs = peel_rightward(&mismatchp,&peeled_pairs,*pairs,&querydp3_distalgap,&genomedp3_distalgap,
#ifdef WASTE
			    pairpool,
#endif
			    maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/true,
			    /*endgappairs*/NULL,&gappair,
			    /*querydp3_medialgap*/NULL,/*genomedp3_medialgap*/NULL);
  }
  
  queryjump = querydp3_distalgap - querydp5 + 1;
  genomejump = queryjump + extramaterial_end; /* proposed */
  /* Previously, we limited genomejump = min(2*queryjump,queryjump+extramaterial_end) */

  genomedp5 = genomedp3_distalgap - genomejump + 1;
#ifdef EXTRACT_GENOMICSEG
  /* Make sure we don't go past the beginning */
  if (genomedp5 < 0) {
    genomedp5 = 0;
    genomejump = genomedp3_distalgap - genomedp5 + 1;
  }
#endif

  debug(printf("Stage 3 (dir %d), extend_ending5: Dynamic programming at 5' end (distal to gap): querydp5 = %d, querydp3 = %d, genomedp5 = %d, genomedp3 = %d\n",
	       cdna_direction,querydp5,querydp3_distalgap,genomedp5,genomedp3_distalgap));


#ifdef PMAP
  continuous_gappairs_distalgap = Dynprog_end5_gap(&(*dynprogindex_minor),&(*finalscore),
						   &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						   &(queryseq_ptr[querydp3_distalgap]),&(queryuc_ptr[querydp3_distalgap]),
						   queryjump,genomejump,querydp3_distalgap,genomedp3_distalgap,
						   chroffset,chrhigh,queryaaseq_ptr,cdna_direction,watsonp,jump_late_p,pairpool,
						   extraband_end,defect_rate,endalign);
  *ambig_end_length = 0;
#else
  if (endalign == QUERYEND_GAP && splicesites != NULL) {
    continuous_gappairs_distalgap = Dynprog_end5_known(&(*knownsplicep),&(*dynprogindex_minor),&(*finalscore),
						       &(*ambig_end_length),&(*ambig_splicetype),
						       &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						       &(queryseq_ptr[querydp3_distalgap]),&(queryuc_ptr[querydp3_distalgap]),
						       queryjump,genomejump,querydp3_distalgap,genomedp3_distalgap,
						       chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
						       cutoff_level,queryptr,querylength,query_compress,
#endif
#endif
						       cdna_direction,watsonp,jump_late_p,pairpool,
						       extraband_end,defect_rate);
  } else {
    continuous_gappairs_distalgap = Dynprog_end5_gap(&(*dynprogindex_minor),&(*finalscore),
						     &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						     &(queryseq_ptr[querydp3_distalgap]),&(queryuc_ptr[querydp3_distalgap]),
						     queryjump,genomejump,querydp3_distalgap,genomedp3_distalgap,
						     chroffset,chrhigh,cdna_direction,watsonp,jump_late_p,pairpool,
						     extraband_end,defect_rate,endalign);
    *ambig_end_length = 0;
    *knownsplicep = false;
  }
#endif

  debug(printf("  finalscore: %d\n",*finalscore));
  if (continuous_gappairs_distalgap == NULL) {
    return (List_T) NULL;
  } else {
    firstpair = List_head(continuous_gappairs_distalgap);
    if (0 && firstpair->querypos != querydp3_distalgap) {
      /* Not a good test anymore, since we are halting peelbacks at gaps */
      /* Must have an indel between the gappairs and the rest of the read */
      debug(printf("Detected indel between gappairs %d and the rest of the read %d\n",
		   firstpair->querypos,querydp3_distalgap));
      return (List_T) NULL;

    } else if (*finalscore < 0) {
      *knownsplicep = false;
#if 0
      return (List_T) NULL;
#endif
      return continuous_gappairs_distalgap;
    } else {
      return continuous_gappairs_distalgap;
    }
  }
}


static List_T
distalmedial_ending3 (bool *knownsplicep, bool *chop_exon_p, int *dynprogindex_minor,
		      int *finalscore, int *ambig_end_length, Splicetype_T *ambig_splicetype, List_T *path,
		      Pair_T leftpair, int rightquerypos, int querylength,
		      Univcoord_T chroffset, Univcoord_T chrhigh,
		      Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		      int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
		      char *queryaaseq_ptr,
#endif
		      char *queryseq_ptr, char *queryuc_ptr,
		      int cdna_direction, bool watsonp, bool jump_late_p,
		      Pairpool_T pairpool, Dynprog_T dynprog, int maxpeelback, int extramaterial_end,
		      int extraband_end, double defect_rate) {
  List_T peeled_path, endgappairs, continuous_gappairs_medialgap = NULL;
  int queryjump, genomejump;
  int querydp5_distalgap, genomedp5_distalgap, querydp3, querydp5_medialgap, genomedp5_medialgap;
  int continuous_goodness_distalgap = 0, continuous_goodness_medialgap = 0,
    nmatches, nmismatches, nopens, nindels;
  bool mismatchp = false;
  bool knownsplice_medial_p = false;
  Pair_T gappair;
  Endalign_T endalign;


  debug(printf("\nDISTALMEDIAL_ENDING3\n"));
  
  querydp5_distalgap = leftpair->querypos + 1;
  genomedp5_distalgap = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5_distalgap--;
  if (leftpair->genome == ' ') genomedp5_distalgap--;
  querydp5_medialgap = querydp5_distalgap;
  genomedp5_medialgap = genomedp5_distalgap;
  querydp3 = rightquerypos - 1;
  /* genomedp3 = rightgenomepos - 1; */

  /* Used to peelback only half as much as for a paired gap, to save
     on dynamic programming, but not any more. */
  *path = peel_leftward(&mismatchp,&peeled_path,*path,&querydp5_distalgap,&genomedp5_distalgap,
#ifdef WASTE
			pairpool,
#endif
			maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
			&endgappairs,&gappair,
			&querydp5_medialgap,&genomedp5_medialgap);
  
  if (endgappairs == NULL) {
    *chop_exon_p = false;
    return peeled_path;

  } else {
#if 0
    continuous_goodness_distalgap = Pair_fracidentity_max(&changepoint,peeled_path,cdna_direction);
#else
    continuous_goodness_distalgap = Pair_fracidentity_score(peeled_path,cdna_direction);
    /* continuous_goodness_distalgap += Pair_fracidentity_score(endgappairs,cdna_direction); */
#endif
    debug(printf("continuous_goodness_distalgap (%d+%d pairs) is %d, with gapcomp '%c', probs %f and %f\n",
		 List_length(peeled_path),List_length(endgappairs),continuous_goodness_distalgap,
		 gappair->comp,gappair->donor_prob,gappair->acceptor_prob));

    if (good_end_intron_p(gappair,cdna_direction) == false) {
      debug(printf("Subtracting points from continuous distal because noncanonical\n"));
      continuous_goodness_distalgap -= CANONICAL_POINTS;
    } else if (gappair->comp == DUALBREAK_COMP) {
      debug(printf("Subtracting points from continuous distal because of dual break\n"));
      continuous_goodness_distalgap -= (CANONICAL_POINTS + CANONICAL_POINTS);
    }

    /* Solve if gap were not present */
    queryjump = querydp3 - querydp5_medialgap + 1;
    genomejump = queryjump + extramaterial_end; /* proposed */
    /* Previously, we limited genomejump = min(2*queryjump,queryjump+extramaterial_end) */

    /* genomedp3 = genomedp5_medialgap + genomejump - 1; */
#ifdef EXTRACT_GENOMICSEG
    /* Make sure we don't go past the end */
    if (genomedp3 > genomiclength - 1) {
      genomedp3 = genomiclength - 1;
      genomejump = genomedp3 - genomedp5_medialgap + 1;
    }
#endif
    
    debug(printf("Stage 3 (dir %d): distalmedial_ending3: Dynamic programming at 3' end (medial to gap): querydp5 = %d, querydp3 = %d, genomedp5 = %u\n",
		 cdna_direction,querydp5_medialgap,querydp3,genomedp5_medialgap));

    if (genomedp5_medialgap < 0) {
      debug(printf("Not feasible to do medial gap\n"));
      *ambig_end_length = 0;

      *path = Pairpool_transfer(*path,endgappairs);
      *chop_exon_p = false;
      /* Let previous value of knownsplicep stand */

      return peeled_path;

    } else {
      debug(printf("Before solving the 3' end, here is the path:\n"));
      debug(Pair_dump_list(*path,true));
      debug(printf("\n"));

#ifdef PMAP
      continuous_gappairs_medialgap = Dynprog_end3_gap(&(*dynprogindex_minor),&(*finalscore),
						       &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						       &(queryseq_ptr[querydp5_medialgap]),&(queryuc_ptr[querydp5_medialgap]),
						       queryjump,genomejump,querydp5_medialgap,genomedp5_medialgap,
						       chroffset,chrhigh,queryaaseq_ptr,cdna_direction,watsonp,jump_late_p,pairpool,
						       extraband_end,defect_rate,/*endalign*/QUERYEND_INDELS);
      *ambig_end_length = 0;
#else
      if (0 && splicesites != NULL) {
	continuous_gappairs_medialgap = Dynprog_end3_known(&knownsplice_medial_p,&(*dynprogindex_minor),&(*finalscore),
							   &(*ambig_end_length),&(*ambig_splicetype),
							   &nmatches,&nmismatches,&nopens,&nindels,dynprog,
							   &(queryseq_ptr[querydp5_medialgap]),&(queryuc_ptr[querydp5_medialgap]),
							   queryjump,genomejump,querydp5_medialgap,genomedp5_medialgap,
							   querylength,chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
							   cutoff_level,queryptr,querylength,query_compress,
#endif
#endif							   
							   cdna_direction,watsonp,jump_late_p,pairpool,
							   extraband_end,defect_rate);
      } else {
	continuous_gappairs_medialgap = Dynprog_end3_gap(&(*dynprogindex_minor),&(*finalscore),
							 &nmatches,&nmismatches,&nopens,&nindels,dynprog,
							 &(queryseq_ptr[querydp5_medialgap]),&(queryuc_ptr[querydp5_medialgap]),
							 queryjump,genomejump,querydp5_medialgap,genomedp5_medialgap,
							 chroffset,chrhigh,cdna_direction,watsonp,jump_late_p,pairpool,
							 extraband_end,defect_rate,/*endalign*/QUERYEND_INDELS);
	*ambig_end_length = 0;
      }
#endif

      continuous_goodness_medialgap = nmatches + MISMATCH*nmismatches + QOPEN*nopens + QINDEL*nindels;
      debug(printf("Continuous_goodness_medialgap %d = %d + %d*%d + %d*%d + %d*%d\n",
		   continuous_goodness_medialgap,nmatches,MISMATCH,nmismatches,QOPEN,nopens,QINDEL,nindels));

      if (continuous_goodness_distalgap > continuous_goodness_medialgap) {
	debug(printf("Continuous distal wins: %d > %d\n",continuous_goodness_distalgap,continuous_goodness_medialgap));
	*ambig_end_length = 0;

#if 0
	debug(printf("Before transferring endgappairs, here is path:\n"));
	debug(Pair_dump_list(*path,true));
	debug(printf("\n"));
#endif

	*path = Pairpool_transfer(*path,endgappairs);
	*chop_exon_p = false;
	/* Let previous value of knownsplicep stand */
#if 0
	changepoint = Pair_fracidentity_changepoint(peeled_path,cdna_direction);
	return List_truncate(peeled_path,changepoint);
#else
	debug(printf("Returning peeled path:\n"));
	debug(Pair_dump_list(peeled_path,true));
	debug(printf("\n"));
	return peeled_path;
#endif
      } else {
	debug(printf("Continuous medial wins: %d > %d\n",continuous_goodness_medialgap,continuous_goodness_distalgap));
	*chop_exon_p = true;
	*knownsplicep = knownsplice_medial_p;
	return List_reverse(continuous_gappairs_medialgap);
      }
    }
  }

}


static List_T
extend_ending3 (bool *knownsplicep, int *dynprogindex_minor, int *finalscore,
		int *ambig_end_length, Splicetype_T *ambig_splicetype,
		List_T *path, Pair_T leftpair, int rightquerypos,
		int querylength, Univcoord_T chroffset, Univcoord_T chrhigh,
		Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
		char *queryaaseq_ptr,
#endif
		char *queryseq_ptr, char *queryuc_ptr,
		int cdna_direction, bool watsonp, bool jump_late_p,
		Pairpool_T pairpool, Dynprog_T dynprog, int maxpeelback, int extramaterial_end,
		int extraband_end, double defect_rate, Endalign_T endalign) {
  List_T continuous_gappairs_distalgap = NULL, peeled_path;
  int queryjump, genomejump;
  int querydp5_distalgap, genomedp5_distalgap, querydp3;
  int nmatches, nmismatches, nopens, nindels;
  bool mismatchp = false;
  Pair_T gappair, firstpair;

  debug(printf("\nEXTEND_ENDING3 with endalign %s and maxpeelback %d\n",
	       Dynprog_endalign_string(endalign),maxpeelback));
  
  querydp5_distalgap = leftpair->querypos + 1;
  genomedp5_distalgap = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5_distalgap--;
  if (leftpair->genome == ' ') genomedp5_distalgap--;
  querydp3 = rightquerypos - 1;
  /* genomedp3 = rightgenomepos - 1; */
  debug(printf("Set dynprog 3 end to be querydp3 = %d\n",querydp3));

  /* Used to peelback only half as much as for a paired gap, to save
     on dynamic programming, but not any more. */
  if (endalign == QUERYEND_NOGAPS) {
    /* Don't peelback on extension */
  } else if (maxpeelback == 0) {
    /* Don't peelback on BEST_LOCAL after trim_ends */
  } else {
    *path = peel_leftward(&mismatchp,&peeled_path,*path,&querydp5_distalgap,&genomedp5_distalgap,
#ifdef WASTE
			  pairpool,
#endif
			  maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/true,
			  /*endgappairs*/NULL,&gappair,
			  /*querydp5_medialgap*/NULL,/*genomedp5_medialgap*/NULL);
  }

  queryjump = querydp3 - querydp5_distalgap + 1;
  genomejump = queryjump + extramaterial_end; /* proposed */
  /* Previously, we limited genomejump = min(2*queryjump,queryjump+extramaterial_end) */

  /* genomedp3 = genomedp5_distalgap + genomejump - 1; */
#ifdef EXTRACT_GENOMICSEG
  /* Make sure we don't go past the end */
  if (genomedp3 > genomiclength - 1) {
    genomedp3 = genomiclength - 1;
    genomejump = genomedp3 - genomedp5_distalgap + 1;
  }
#endif

  debug(printf("Stage 3 (dir %d), extend_ending3: Dynamic programming at 3' end (distal to gap): querydp5 = %d, querydp3 = %d, genomedp5 = %d\n",
	       cdna_direction,querydp5_distalgap,querydp3,genomedp5_distalgap));
  
#ifdef PMAP
  continuous_gappairs_distalgap = Dynprog_end3_gap(&(*dynprogindex_minor),&(*finalscore),
						   &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						   &(queryseq_ptr[querydp5_distalgap]),&(queryuc_ptr[querydp5_distalgap]),
						   queryjump,genomejump,querydp5_distalgap,genomedp5_distalgap,
						   chroffset,chrhigh,queryaaseq_ptr,cdna_direction,watsonp,jump_late_p,pairpool,
						   extraband_end,defect_rate,endalign);
  *ambig_end_length = 0;
#else
  if (endalign == QUERYEND_GAP && splicesites != NULL) {
    continuous_gappairs_distalgap = Dynprog_end3_known(&(*knownsplicep),&(*dynprogindex_minor),&(*finalscore),
						       &(*ambig_end_length),&(*ambig_splicetype),
						       &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						       &(queryseq_ptr[querydp5_distalgap]),&(queryuc_ptr[querydp5_distalgap]),
						       queryjump,genomejump,querydp5_distalgap,genomedp5_distalgap,
						       querylength,chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
						       cutoff_level,queryptr,querylength,query_compress,
#endif
#endif
						       cdna_direction,watsonp,jump_late_p,pairpool,
						       extraband_end,defect_rate);
  } else {
    continuous_gappairs_distalgap = Dynprog_end3_gap(&(*dynprogindex_minor),&(*finalscore),
						     &nmatches,&nmismatches,&nopens,&nindels,dynprog,
						     &(queryseq_ptr[querydp5_distalgap]),&(queryuc_ptr[querydp5_distalgap]),
						     queryjump,genomejump,querydp5_distalgap,genomedp5_distalgap,
						     chroffset,chrhigh,cdna_direction,watsonp,jump_late_p,pairpool,
						     extraband_end,defect_rate,endalign);
    *ambig_end_length = 0;
    *knownsplicep = false;
  }
#endif

  debug(printf("  finalscore: %d\n",*finalscore));
  if (continuous_gappairs_distalgap == NULL) {
    return (List_T) NULL;
  } else {
    continuous_gappairs_distalgap = List_reverse(continuous_gappairs_distalgap);
    firstpair = List_head(continuous_gappairs_distalgap);
    if (0 && firstpair->querypos != querydp5_distalgap) {
      /* Not a good test anymore, since we are halting peelbacks at gaps */
      /* Must have an indel between the gappairs and the rest of the read */
      debug(printf("Detected indel between gappairs %d and the rest of the read %d\n",
		   firstpair->querypos,querydp5_distalgap));
      return (List_T) NULL;
      
    } else if (*finalscore < 0) {
      *knownsplicep = false;
#if 0
      return (List_T) NULL;
#endif
      return continuous_gappairs_distalgap;
    } else {
      return continuous_gappairs_distalgap;
    }
  }
}




static List_T
traverse_dual_break (List_T pairs, List_T *path, Pair_T leftpair, Pair_T rightpair,
		     Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
		     char *queryaaseq_ptr, char *genomicuc_ptr,
#endif
		     char *queryseq_ptr, char *queryuc_ptr,
		     bool watsonp, int genestrand, Pairpool_T pairpool,
		     Oligoindex_T *oligoindices_minor, int noligoindices_minor,
		     Diagpool_T diagpool, int sufflookback, int nsufflookback, int maxintronlen_bound) {
  List_T gappairs, peeled_pairs = NULL, peeled_path = NULL;
  int querydp5, genomedp5, querydp3, genomedp3, source, indexsize;
  bool mismatchp;
  Pair_T gappair, firstpair, lastpair;
  int maxpeelback = 1;
  Chrpos_T chrstart, chrend;


  debug14(printf("\nTRAVERSE_DUAL_BREAK\n"));
  querydp5 = leftpair->querypos + 1;
  genomedp5 = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5--;
  if (leftpair->genome == ' ') genomedp5--;
  querydp3 = rightpair->querypos - 1;
  genomedp3 = rightpair->genomepos - 1;

  /* Previously skipped this, but need to do at least a little
     peelback to avoid gaps at either end */
  pairs = peel_rightward(&mismatchp,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			 pairpool,
#endif
			 maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
			 /*endgappairs*/NULL,&gappair,
			 /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);
  *path = peel_leftward(&mismatchp,&peeled_path,*path,&querydp5,&genomedp5,
#ifdef WASTE
			pairpool,
#endif
			maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
			/*endgappairs*/NULL,&gappair,
			/*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

  debug14(printf("genome %d..%d, query %d..%d\n",genomedp5,genomedp3,querydp5,querydp3));
#ifdef EXTRACT_GENOMICSEG
  debug14(printf("genome %.*s\n",genomedp3-genomedp5+1,&(genomicseg_ptr[genomedp5])));
#endif
  debug14(printf("query %.*s\n",querydp3-querydp5+1,&(queryseq_ptr[querydp5])));

  if (watsonp) {
    chrstart = genomedp5;
    chrend = genomedp3;
  } else {
    chrstart = (chrhigh - chroffset) - genomedp3;
    chrend = (chrhigh - chroffset) - genomedp5;
  }

  debug14(printf("Starting stage2 with chrstart %u, chrend %u, watsonp %d\n",
	       chrstart,chrend,watsonp));
  gappairs = Stage2_compute_one(&source,&indexsize,
				&(queryseq_ptr[querydp5]),&(queryuc_ptr[querydp5]),
				/*querylength*/querydp3-querydp5+1,/*query_offset*/querydp5,
#ifdef PMAP
				genomicuc_ptr,
#endif
				chrstart,chrend,chroffset,chrhigh,
				/*plusp*/watsonp,genestrand,

				oligoindices_minor,noligoindices_minor,/*proceed_pctcoverage*/0.80,
				pairpool,diagpool,sufflookback,nsufflookback,maxintronlen_bound,
				/*localp should be false*/true,/*skip_repetitive_p*/false,
				/*use_shifted_canonical_p*/true,/*favor_right_p*/false,
				/*debug_graphic_p*/false,/*diagnosticp*/false,/*stopwatch*/NULL,/*diag_debug*/false);
  
  debug14(printf("Internal stage2 result:\n"));
  debug14(Pair_dump_list(gappairs,true));

  if (gappairs == NULL) {
    pairs = Pairpool_transfer(pairs,peeled_pairs);
    *path = Pairpool_transfer(*path,peeled_path);
    pairs = Pairpool_push_gapholder(pairs,pairpool,/*queryjump*/UNKNOWNJUMP,/*genomejump*/UNKNOWNJUMP,/*knownp*/false);
  } else {
    lastpair = (Pair_T) gappairs->first;
    firstpair = (Pair_T) List_last_value(gappairs);
    debug14(printf("gappairs goes from %d to %d\n",firstpair->querypos,lastpair->querypos));
    if (firstpair->querypos == querydp5 && lastpair->querypos == querydp3) {
      debug14(printf("  => entire query sequence bridged\n"));
      pairs = Pairpool_transfer(pairs,gappairs);
    } else {
      debug14(printf("  => entire query sequence not bridged, so abort\n"));
      pairs = Pairpool_transfer(pairs,peeled_pairs);
      *path = Pairpool_transfer(*path,peeled_path);
      pairs = Pairpool_push_gapholder(pairs,pairpool,/*queryjump*/UNKNOWNJUMP,/*genomejump*/UNKNOWNJUMP,/*knownp*/false);
    }
  }

  return pairs;
}


/************************************************************************
 *   End of traversal functions
 ************************************************************************/

static List_T
build_dual_breaks (bool *dual_break_p, int *dynprogindex_minor, int *dynprogindex_major, List_T path,
		   Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
		   char *queryaaseq_ptr, char *genomicuc_ptr,
#endif
		   char *queryseq_ptr, char *queryuc_ptr,
		   int cdna_direction, bool watsonp, int genestrand, bool jump_late_p,
		   Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		   int maxpeelback, Oligoindex_T *oligoindices_minor, int noligoindices_minor,
		   Diagpool_T diagpool, int sufflookback, int nsufflookback, int maxintronlen_bound,
		   int extramaterial_paired, int extraband_paired, int extraband_single,
		   double defect_rate, int close_indels_mode) {

  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  bool filledp, shiftp;


  *dual_break_p = false;

  debug(Pair_dump_list(path,true));
  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->gapp == false || pair->comp != DUALBREAK_COMP) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif
    } else if (path == NULL || pairs == NULL) {
      debug14(printf("Observed a dual break at the end of the alignment, case 1\n"));
    } else {
      leftpair = path->first;
      rightpair = pairs->first;
      if (leftpair->querypos < 0 || rightpair->querypos < 0) {
	debug14(printf("Observed a dual break at the end of the alignment, case 2\n"));
      } else {
	debug14(printf("Observed a dual break at %d..%d with queryjump = %d, genomejump = %d\n",
		     leftpair->querypos,rightpair->querypos,pair->queryjump,pair->genomejump));
	
	if (pair->genomejump - pair->queryjump < SINGLESLEN &&
		   pair->queryjump - pair->genomejump < SINGLESLEN) {
	  debug14(printf("  Can be solved as a single gap\n"));
	  pairs = traverse_single_gap(&filledp,&(*dynprogindex_minor),pairs,&path,leftpair,rightpair,
				      chroffset,chrhigh,
#ifdef PMAP
				      queryaaseq_ptr,
#endif
				      queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
				      jump_late_p,pairpool,dynprogM,maxpeelback,extraband_single,defect_rate,
				      close_indels_mode,/*forcep*/true);

	} else if (pair->queryjump < MIN_STAGE2_FOR_DUALBREAK) {
	  debug14(printf("  Can be solved as a genome gap\n"));
	  pairs = traverse_genome_gap(&filledp,&shiftp,&(*dynprogindex_minor),&(*dynprogindex_major),
				      pairs,&path,leftpair,rightpair,chrnum,chroffset,chrhigh,
#ifdef PMAP
				      queryaaseq_ptr,
#endif
				      queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,jump_late_p,
				      pairpool,dynprogL,dynprogM,dynprogR,maxpeelback,extramaterial_paired,
				      extraband_paired,extraband_single,defect_rate,close_indels_mode,
				      /*finalp*/false);

	} else {
	  debug14(printf("  Solving as a dual break\n"));
	  *dual_break_p = true;
	  pairs = traverse_dual_break(pairs,&path,leftpair,rightpair,chroffset,chrhigh,
#ifdef PMAP
				      queryaaseq_ptr,genomicuc_ptr,
#endif
				      queryseq_ptr,queryuc_ptr,watsonp,genestrand,
				      pairpool,oligoindices_minor,noligoindices_minor,diagpool,
				      sufflookback,nsufflookback,maxintronlen_bound);
	}
      }
    }
  }

  debug14(printf("After build_dual_breaks:\n"));
  debug(Pair_dump_list(pairs,true));

  return pairs;
}


  
/* Note: querypos is actually indexsize nt to the left of the last nt match.
   
	||||||||********   X  X	 XX X	X
	       ^	 <- queryjump->	 ^
	    leftquerypos		 rightquerypos

		<-     querydpspan     ->
*/
static List_T
build_path_end3 (bool *knownsplicep, int *ambig_end_length_3, Splicetype_T *ambig_splicetype_3,
		 bool *chop_exon_p, int *dynprogindex_minor,
		 List_T path, Univcoord_T chroffset, Univcoord_T chrhigh, int querylength,
		 Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		 int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
		 char *queryaaseq_ptr,
#endif
		 char *queryseq_ptr, char *queryuc_ptr,
		 int cdna_direction, bool watsonp, bool jump_late_p, int maxpeelback,
		 int maxpeelback_distalmedial, int nullgap,
		 int extramaterial_end, int extraband_end,
		 double defect_rate, Pairpool_T pairpool, Dynprog_T dynprogL,
		 bool extendp, Endalign_T endalign) {
  List_T gappairs;
  Pair_T leftpair;
  /* int genomejump */
  int queryjump, rightquerypos;
  int finalscore;

  if (*ambig_end_length_3 > 0) {
    debug(printf("ambig_end_length_3 is %d, so returning path\n",*ambig_end_length_3));
    return path;
  }

  *knownsplicep = false;
  if (path == NULL) {
    *ambig_end_length_3 = 0;
    return (List_T) NULL;
  } else {
    leftpair = path->first;
  }
  debug(printf("Stage 3 (dir %d): 3' end: leftquerypos = %d, rightquerypos = %d, leftgenomepos = %d\n",
	       cdna_direction,leftpair->querypos,querylength,leftpair->genomepos));
  if (leftpair->querypos < 0) {
    *ambig_end_length_3 = 0;
    return (List_T) NULL;
    /* abort(); */
  }

  queryjump = querylength - leftpair->querypos - 1;
  /* genomejump = genomiclength - leftpair->genomepos - 1; */
  if (leftpair->cdna == ' ') queryjump++;
  /* if (leftpair->genome == ' ') genomejump++; */

  /* Note difference with 5' case.  We use queryjump+1 here instead of queryjump and genomejump */
  /* Do use nullgap here.  Truncating back to entire exon can slow algorithm down significantly. */
  if (/* 0 && */ queryjump+1 > nullgap) {
    rightquerypos = leftpair->querypos + nullgap + 1;
    debug(printf("Since queryjump+1 %d > nullgap %d, setting rightquerypos %d = %d + %d + 1\n",
		 queryjump+1,nullgap,rightquerypos,leftpair->querypos,nullgap));
  } else {
    rightquerypos = querylength;
  }

  if (extendp == true) {
    debug(printf("Running extend_ending3\n"));
    *chop_exon_p = false;
    gappairs = extend_ending3(&(*knownsplicep),&(*dynprogindex_minor),&finalscore,
			      &(*ambig_end_length_3),&(*ambig_splicetype_3),&path,
			      leftpair,rightquerypos,querylength,
			      chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			      cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			      queryaaseq_ptr,
#endif
			      queryseq_ptr,queryuc_ptr,
			      cdna_direction,watsonp,jump_late_p,pairpool,dynprogL,maxpeelback,
			      extramaterial_end,extraband_end,defect_rate,endalign);
  } else {
    /* Looks like we aren't calling this anymore */
    debug(printf("Running distalmedial_ending3\n"));
    gappairs = distalmedial_ending3(&(*knownsplicep),&(*chop_exon_p),&(*dynprogindex_minor),
				    &finalscore,&(*ambig_end_length_3),&(*ambig_splicetype_3),
				    &path,leftpair,rightquerypos,querylength,
				    chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
				    cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
				    queryaaseq_ptr,
#endif
				    queryseq_ptr,queryuc_ptr,
				    cdna_direction,watsonp,jump_late_p,pairpool,dynprogL,maxpeelback_distalmedial,
				    extramaterial_end,extraband_end,defect_rate);
  }

  debug(printf("Gappairs from build_path_end3:\n"));
  debug(Pair_dump_list(gappairs,true));

  path = Pairpool_transfer(path,gappairs);

  debug(printf("Final result of build_path_end3:\n"));
  debug(Pair_dump_list(path,true));
  debug(printf("\n"));

  return path;
}


/* Schematic:
   
       <- queryjump ->
       X  X  XX X   X ********||||||||
      ^		      ^
   leftquerypos	      rightquerypos

       <-    querydpspan    ->

*/
static List_T
build_pairs_end5 (bool *knownsplicep, int *ambig_end_length_5, Splicetype_T *ambig_splicetype_5,
		  bool *chop_exon_p, int *dynprogindex_minor, List_T pairs,
		  Univcoord_T chroffset, Univcoord_T chrhigh,
		  Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		  int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
		  char *queryaaseq_ptr,
#endif
		  char *queryseq_ptr, char *queryuc_ptr,
		  int cdna_direction, bool watsonp, bool jump_late_p, int maxpeelback,
		  int maxpeelback_distalmedial, int nullgap,
		  int extramaterial_end, int extraband_end,
		  double defect_rate, Pairpool_T pairpool, Dynprog_T dynprogR,
		  bool extendp, Endalign_T endalign) {
  List_T gappairs;
  Pair_T rightpair;
  int queryjump, leftquerypos;
  int finalscore;
  /* int genomejump */

  if (*ambig_end_length_5 > 0) {
    debug(printf("ambig_end_length_5 is %d, so returning pairs\n",*ambig_end_length_5));
    return pairs;
  }

  *knownsplicep = false;
  if (pairs == NULL) {
    *ambig_end_length_5 = 0;
    return (List_T) NULL;
  } else {
    rightpair = pairs->first;
  }
  debug(printf("Stage 3 (dir %d): 5' end: leftquerypos = %d, rightquerypos = %d, leftgenomepos = %d\n",
	       cdna_direction,-1,rightpair->querypos,-1));
  if (rightpair->querypos < 0) {
    *ambig_end_length_5 = 0;
    return (List_T) NULL;
    /* abort(); */
  }

  queryjump = rightpair->querypos; /* - leftquerypos (-1) - 1 */
  /* genomejump = rightpair->genomepos; */  /* - leftgenomepos (-1) - 1 */

  /* Note difference with 3' case.  We use queryjump here instead of queryjump+1 */
  /* Do use nullgap here.  Truncating back to entire exon can slow algorithm significantly. */
  if (/*0 && */ queryjump > nullgap) {
    leftquerypos = rightpair->querypos - nullgap - 1;
  } else {
    leftquerypos = -1;
  }

  if (extendp == true) {
    debug(printf("Running extend_ending5\n"));
    *chop_exon_p = false;
    gappairs = extend_ending5(&(*knownsplicep),&(*dynprogindex_minor),
			      &finalscore,&(*ambig_end_length_5),&(*ambig_splicetype_5),
			      &pairs,leftquerypos,/*leftgenomepos*/-1,rightpair,
			      chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			      cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			      queryaaseq_ptr,
#endif
			      queryseq_ptr,queryuc_ptr,
			      cdna_direction,watsonp,jump_late_p,pairpool,dynprogR,maxpeelback,
			      extramaterial_end,extraband_end,defect_rate,endalign);
  } else {
    /* Looks like we aren't calling this anymore */
    debug(printf("Running distalmedial_ending5\n"));
    gappairs = distalmedial_ending5(&(*knownsplicep),&(*chop_exon_p),&(*dynprogindex_minor),
				    &finalscore,&(*ambig_end_length_5),&(*ambig_splicetype_5),
				    &pairs,leftquerypos,/*leftgenomepos*/-1,rightpair,
				    chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
				    cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
				    queryaaseq_ptr,
#endif
				    queryseq_ptr,queryuc_ptr,
				    cdna_direction,watsonp,jump_late_p,pairpool,dynprogR,maxpeelback_distalmedial,
				    extramaterial_end,extraband_end,defect_rate);
  }

  debug(printf("Gappairs from build_pairs_end5:\n"));
  debug(Pair_dump_list(gappairs,true));

  pairs = Pairpool_transfer(pairs,gappairs);

  debug(printf("Final result of build_pairs_end5:\n"));
  debug(Pair_dump_list(pairs,true));
  debug(printf("\n"));

  return pairs;
}



static List_T
build_pairs_singles (int *dynprogindex, List_T path,
		     Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
		     char *queryaaseq_ptr,
#endif
		     char *queryseq_ptr, char *queryuc_ptr,
		     int cdna_direction, bool watsonp, bool jump_late_p, int maxpeelback, int nullgap,
		     int extraband_single, double defect_rate, int close_indels_mode,
		     Pairpool_T pairpool, Dynprog_T dynprogM) {
  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  bool filledp;

  debug(printf("\n** Starting build_pairs_singles\n"));
  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->gapp == false) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > nullgap) {
      /* Large gap.  Do nothing */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > pair->genomejump + EXTRAQUERYGAP) {
      /* cDNA insertion.  Do nothing */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->genomejump > pair->queryjump + SINGLESLEN) {
      /* Intron.  Do nothing */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (path == NULL || pairs == NULL) {
      fprintf(stderr,"Single gap at beginning or end of alignment\n");
      abort();

    } else {
      /* Guarantees: queryjump <= nullgap && genomejump < queryjump - EXTRAQUERYGAP &&
	 genomejump <= queryjump + MININTRONLEN, meaning that score matrix is nearly square */
      leftpair = path->first;
      rightpair = pairs->first;
	
      debug(printf("Stage 3 (dir %d): Traversing single gap: leftquerypos = %d, rightquerypos = %d, leftgenomepos = %d, rightgenomepos = %d\n",
		   cdna_direction,leftpair->querypos,rightpair->querypos,leftpair->genomepos,rightpair->genomepos));
      pairs = traverse_single_gap(&filledp,&(*dynprogindex),pairs,&path,leftpair,rightpair,
				  chroffset,chrhigh,
#ifdef PMAP
				  queryaaseq_ptr,
#endif
				  queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
				  jump_late_p,pairpool,dynprogM,maxpeelback,extraband_single,defect_rate,
				  close_indels_mode,/*forcep*/false);
      /* (old comment:) forcep needs to be true here to avoid subsequent anomalies in building dualintrons, e.g., XM_376610.2_mRNA on 7:127885572..127888991 */
      if (filledp == true) {
	/* Discard the gap */
	debug(printf("Discarding gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
      } else {
	/* Replace the gap */
	debug(printf("Replacing gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
#ifdef WASTE
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	pairs = List_push_existing(pairs,pairptr);
#endif

      }
    }
  }

  return pairs;
}


static List_T
build_pairs_dualintrons (int *dynprogindex, List_T path,
			 Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
			 char *queryaaseq_ptr,
#endif
			 char *queryseq_ptr, char *queryuc_ptr, int cdna_direction, bool watsonp,
			 bool jump_late_p, int maxpeelback, int nullgap,
			 int extramaterial_paired, int extraband_paired, double defect_rate,
			 Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogR) {
  List_T pairs = NULL, midexon_pairs = NULL, pairptr;
  Pair_T pair, leftpair, midleftpair, midpair, midrightpair, rightpair;
  int midquerypos, midgenomepos;
  bool left_end_intron_p, right_end_intron_p, exonp;

  debug(printf("\n** Starting build_pairs_dualintrons\n"));
  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    
    if (pair->gapp == false) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > nullgap) {
      /* Large gap.  Do nothing */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > pair->genomejump + EXTRAQUERYGAP) {
      /* cDNA insertion.  Do nothing */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->genomejump <= pair->queryjump + MININTRONLEN) {
      /* Single gap.  Do nothing */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else {
      midpair = path->first;
      if (midpair->shortexonp == false) {
	/* Long exon; do nothing */
	debug(printf("I see a long exon at %d...do nothing\n",midpair->querypos));
#ifdef WASTE
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	pairs = List_push_existing(pairs,pairptr);
#endif

      } else {
	/* Short exon */
	debug(printf("I see a short exon at %d...crossing\n",midpair->querypos));
	right_end_intron_p = pair->end_intron_p;
#ifdef WASTE
	path = Pairpool_pop(path,&midpair);
	midexon_pairs = Pairpool_push_existing(NULL,pairpool,midpair);
#else
	midpair = path->first;
	midexon_pairs = List_transfer_one(NULL,&path);
#endif
	midrightpair = midpair;

	exonp = true;
	while (path != NULL && exonp) {
#ifdef WASTE
	  path = Pairpool_pop(path,&midpair);
#else
	  midpair = path->first;
#endif
	  if (midpair->gapp == true) {
	    left_end_intron_p = midpair->end_intron_p;
	    exonp = false;
#ifdef WASTE
#else
	    path = path->rest;
#endif
	  } else {
#ifdef WASTE
	    midexon_pairs = Pairpool_push_existing(midexon_pairs,pairpool,midpair);
#else
	    midexon_pairs = List_transfer_one(midexon_pairs,&path);
#endif
	  }
	}
	debug(printf("Finished crossing a short exon\n"));

	if (path == NULL) {
	  /* Short exon is the first one.  Process it. */
	  debug(printf("path is NULL so Pairpool_push_existing\n"));
#ifdef WASTE
	  pairs = Pairpool_push_existing(pairs,pairpool,pair); /* initial gap */
#else
	  pairs = List_push_existing(pairs,pairptr);
#endif
	  pairs = Pairpool_transfer(pairs,List_reverse(midexon_pairs));

	} else {
	  /* Perform dual intron gap */
	  debug(printf("path is not NULL, so doing dual intron gap\n"));
	  midleftpair = midexon_pairs->first;
	  midgenomepos = (midleftpair->genomepos + midrightpair->genomepos)/2;
	  midquerypos = midrightpair->querypos - (midrightpair->genomepos - midgenomepos);
	  leftpair = path->first;
	  rightpair = pairs->first;
	  if (midquerypos <= leftpair->querypos || midquerypos >= rightpair->querypos) {
	    /* Skip */

	  } else {	  
	    debug(printf("Stage 3 (dir %d): Traversing dual intron gap: leftquerypos = %d, midquerypos = %d, rightquerypos = %d, leftgenomepos = %d, midgenomepos = %d, rightgenomepos = %d\n",
			 cdna_direction,leftpair->querypos,midquerypos,rightpair->querypos,
			 leftpair->genomepos,midgenomepos,rightpair->genomepos));
	  
	    pairs = traverse_dual_genome_gap(&(*dynprogindex),pairs,&path,leftpair,rightpair,
					     left_end_intron_p,right_end_intron_p,
					     chrnum,chroffset,chrhigh,
					     midquerypos,midgenomepos,
#ifdef PMAP
					     queryaaseq_ptr,
#endif
					     queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
					     jump_late_p,pairpool,dynprogL,dynprogR,
					     maxpeelback,nullgap,extramaterial_paired,extraband_paired,
					     defect_rate);
	  }
	}
      }
    }
  }

  return pairs;
}  


static List_T
build_pairs_introns (bool *shiftp, bool *incompletep, 
		     int *dynprogindex_minor, int *dynprogindex_major, List_T path,
		     Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef PMAP
		     char *queryaaseq_ptr,
#endif
		     char *queryseq_ptr, char *queryuc_ptr,
		     int cdna_direction, bool watsonp, bool jump_late_p,
		     int maxpeelback, int nullgap, int extramaterial_paired, 
		     int extraband_single, int extraband_paired, double defect_rate, int close_indels_mode,
		     Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		     bool finalp) {
  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  bool filledp;
  int minintronlen;

  debug(printf("\n** Starting build_pairs_introns\n"));
  debug(Pair_dump_list(path,true));

  if (finalp == true) {
    minintronlen = MININTRONLEN_FINAL;
  } else {
    minintronlen = MININTRONLEN;
  }

  *shiftp = *incompletep = false;
  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->gapp == false) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > nullgap) {
      debug(leftpair = path->first;
	    rightpair = pairs->first;
	    printf("Stage 3 (dir %d): Adding large gap: leftquerypos = %d, rightquerypos = %d, leftgenomepos = %d, rightgenomepos = %d\n",
		   cdna_direction,leftpair->querypos,rightpair->querypos,leftpair->genomepos,rightpair->genomepos)); 
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > pair->genomejump + EXTRAQUERYGAP) {
      leftpair = path->first;
      rightpair = pairs->first;
      debug(printf("Stage 3 (dir %d): Traversing cDNA gap: leftquerypos = %d, rightquerypos = %d, leftgenomepos = %d, rightgenomepos = %d\n",
		   cdna_direction,leftpair->querypos,rightpair->querypos,leftpair->genomepos,rightpair->genomepos));
      pairs = traverse_cdna_gap(&filledp,&(*incompletep),&(*dynprogindex_minor),&(*dynprogindex_major),
				pairs,&path,leftpair,rightpair,
				chroffset,chrhigh,
#ifdef PMAP
				queryaaseq_ptr,
#endif
				queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
				jump_late_p,pairpool,dynprogL,dynprogM,dynprogR,
				maxpeelback,extramaterial_paired,extraband_paired,extraband_single,
				defect_rate,close_indels_mode);

      if (filledp == true) {
	/* Discard gap */
	debug(printf("Discarding gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
      } else {
	/* Replace the gap */
	debug(printf("Replacing gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
#ifdef WASTE
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	pairs = List_push_existing(pairs,pairptr);
#endif
      }

    } else if (pair->genomejump > pair->queryjump + minintronlen) {
      /* Previously was 2*MININTRONLEN, and comment said needed space for two introns */
      /* We will make the score matrices nearly square */
      leftpair = path->first;
      rightpair = pairs->first;
      debug(printf("Stage 3 (dir %d): Traversing paired gap: leftquerypos = %d, rightquerypos = %d, leftgenomepos = %d, rightgenomepos = %d\n",
		   cdna_direction,leftpair->querypos,rightpair->querypos,leftpair->genomepos,rightpair->genomepos));
      pairs = traverse_genome_gap(&filledp,&(*shiftp),&(*dynprogindex_minor),&(*dynprogindex_major),
				  pairs,&path,leftpair,rightpair,chrnum,chroffset,chrhigh,
#ifdef PMAP
				  queryaaseq_ptr,
#endif
				  queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,jump_late_p,
				  pairpool,dynprogL,dynprogM,dynprogR,maxpeelback,extramaterial_paired,
				  extraband_paired,extraband_single,defect_rate,close_indels_mode,
				  finalp);
      /* Previously had forcep == true, because previously thought that adding large gap is not a good solution */

      if (filledp == true) {
	/* Discard the gap */
	debug(printf("Discarding gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
      } else {
	/* Replace the gap */
	debug(printf("Replacing gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
#ifdef WASTE
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	pairs = List_push_existing(pairs,pairptr);
#endif
      }

    } else if (pair->genomejump > pair->queryjump + SINGLESLEN) {
      /* Intron length shorter than MININTRONLEN_FINAL.  Just replace the gap */
      debug(printf("Short intron; not candidate for final calculation.  Replacing gap ");
	    Pair_dump_one(pair,true);
	    printf("\n"));
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else {
      /* Single gap; force fill */
      leftpair = path->first;
      rightpair = pairs->first;
      debug(printf("Stage 3 (dir %d): Traversing single gap: leftquerypos = %d, rightquerypos = %d, leftgenomepos = %d, rightgenomepos = %d.  queryjump = %d, genomejump = %d\n",
		   cdna_direction,leftpair->querypos,rightpair->querypos,leftpair->genomepos,rightpair->genomepos,
		   pair->queryjump,pair->genomejump));
      pairs = traverse_single_gap(&filledp,&(*dynprogindex_minor),pairs,&path,leftpair,rightpair,
				  chroffset,chrhigh,
#ifdef PMAP
				  queryaaseq_ptr,
#endif
				  queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
				  jump_late_p,pairpool,dynprogM,maxpeelback,extraband_single,defect_rate,
				  close_indels_mode,/*forcep*/false);

      if (filledp == true) {
	/* Discard the gap */
	debug(printf("Discarding gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
      } else {
	/* Replace the gap */
	debug(printf("Replacing gap ");
	      Pair_dump_one(pair,true);
	      printf("\n"));
#ifdef WASTE
	pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
	pairs = List_push_existing(pairs,pairptr);
#endif
      }
    }
  }

  return pairs;
}  


static int
score_alignment (int *nmatches, int *nmismatches, int *nindels,
#ifdef COMPLEX_DIRECTION
		 int *indel_alignment_score,
#endif
		 int *ncanonical, int *nsemicanonical, int *nnoncanonical,
		 List_T pairs, int cdna_direction) {
  int nunknowns, qopens, qindels, topens, tindels;
  double min_splice_prob;

  Pair_fracidentity(&(*nmatches),&nunknowns,&(*nmismatches),&qopens,&qindels,&topens,&tindels,
		    &(*ncanonical),&(*nsemicanonical),&(*nnoncanonical),&min_splice_prob,
		    pairs,cdna_direction);
  debug11(printf("%d matches, %d nmismatches, %d+%d qgaps, %d+%d tgaps => alignment_score is %d\n",
		 *nmatches,*nmismatches,qopens,qindels,topens,tindels,
		 MATCH*(*nmatches) + MISMATCH*(*nmismatches) + QOPEN*(qopens + qindels) + TOPEN*(topens + tindels)));

  debug(printf("%d matches, %d nmismatches, %d+%d qgaps, %d+%d tgaps => alignment_score is %d\n",
	       *nmatches,*nmismatches,qopens,qindels,topens,tindels,
	       MATCH*(*nmatches) + MISMATCH*(*nmismatches) + QOPEN*(qopens + qindels) + TOPEN*(topens + tindels)));

#ifdef COMPLEX_DIRECTION
  *indel_alignment_score = QOPEN*(qopens + qindels) + TOPEN*(topens + tindels);
#endif

  *nindels = qindels + tindels;
  return MATCH*(*nmatches) + MISMATCH*(*nmismatches) + QOPEN*(qopens + qindels) + TOPEN*(topens + tindels);
}  


static List_T
score_introns (double *max_intron_score, double *avg_donor_score, double *avg_acceptor_score, int *nbadintrons,
	       List_T path, int cdna_direction, bool watsonp,
	       Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
#ifdef WASTE	       
	       Pairpool_T pairpool,
#endif
	       int nullgap) {
  List_T pairs = NULL, pairptr;
  Pair_T pair, leftpair, rightpair;
  Univcoord_T splicesitepos;
  int minintronlen;
  double donor_score, acceptor_score;
  int nintrons = 0;
#if 0
  char gbuffer1[MAXENT_MAXLENGTH];
#endif

  debug11(printf("\n** Starting score_introns\n"));

  minintronlen = MININTRONLEN_FINAL;

  *max_intron_score = *avg_donor_score = *avg_acceptor_score = 0.0;
  *nbadintrons = 0;

  while (path != NULL) {
    pairptr = path;
    path = Pairpool_pop(path,&pair);
    if (pair->gapp == false) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > nullgap) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->queryjump > pair->genomejump + EXTRAQUERYGAP) {
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->genomejump > pair->queryjump + minintronlen) {
      leftpair = path->first;
      rightpair = pairs->first;

      debug11(printf("pair->comp = %c\n",pair->comp));

      if (cdna_direction == +1) {
	if (watsonp) {
	  splicesitepos = leftpair->genomepos + 1;
	  debug11(printf("1. looking up splicesites_iit for donor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/+1)) {
	    debug11(printf(" => known\n"));
	    donor_score = 1.0;
	  } else {
	    donor_score = Maxent_hr_donor_prob(chroffset + splicesitepos,chroffset);
	  }

	  splicesitepos = rightpair->genomepos;
	  debug11(printf("2. looking up splicesites_iit for acceptor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/+1)) {
	    debug11(printf(" => known\n"));
	    acceptor_score = 1.0;
	  } else {
	    acceptor_score = Maxent_hr_acceptor_prob(chroffset + splicesitepos,chroffset);
	  }

	} else {
	  splicesitepos = (chrhigh - chroffset) - leftpair->genomepos;
	  debug11(printf("3. looking up splicesites_iit for donor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/-1)) {
	    debug11(printf(" => known\n"));
	    donor_score = 1.0;
	  } else {
	    donor_score = Maxent_hr_antidonor_prob(chroffset + splicesitepos,chroffset);
	  }

	  splicesitepos = (chrhigh - chroffset) - rightpair->genomepos + 1;
	  debug11(printf("4. looking up splicesites_iit for acceptor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/-1)) {
	    debug11(printf(" => known\n"));
	    acceptor_score = 1.0;
	  } else {
	    acceptor_score = Maxent_hr_antiacceptor_prob(chroffset + splicesitepos,chroffset);
	  }
	}
	debug11(printf("donor score at %u is %f, watson %d, cdna_direction %d\n",
		       leftpair->genomepos,donor_score,watsonp,cdna_direction));
	debug11(printf("acceptor score at %u is %f, watson %d, cdna_direction %d\n",
		       rightpair->genomepos,acceptor_score,watsonp,cdna_direction));
	nintrons += 1;
	if (pair->knowngapp == true) {
	  /* Skip */
	} else if (pair->comp == FWD_CANONICAL_INTRON_COMP && (donor_score < 0.9 && acceptor_score < 0.9)) {
	  *nbadintrons = 1;
	}
	*avg_donor_score += donor_score;
	*avg_acceptor_score += acceptor_score;
	if (donor_score + acceptor_score > *max_intron_score) {
	  *max_intron_score = donor_score + acceptor_score;
	}

      } else if (cdna_direction == -1) {

#if 0
	make_complement_buffered(gbuffer1,&(genomicuc_ptr[leftpair->genomepos - ACCEPTOR_MODEL_RIGHT_MARGIN]),
				 ACCEPTOR_MODEL_LEFT_MARGIN+ACCEPTOR_MODEL_RIGHT_MARGIN+1);
	acceptor_score = Maxent_acceptor_prob(gbuffer1);
	make_complement_buffered(gbuffer1,&(genomicuc_ptr[rightpair->genomepos - DONOR_MODEL_RIGHT_MARGIN - 1]),
				 DONOR_MODEL_LEFT_MARGIN+DONOR_MODEL_RIGHT_MARGIN+1);
	donor_score = Maxent_donor_prob(gbuffer1);
#endif

	if (watsonp) {
	  splicesitepos = leftpair->genomepos + 1;
	  debug11(printf("5. looking up splicesites_iit for acceptor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/-1)) {
	    debug11(printf(" => known\n"));
	    acceptor_score = 1.0;
	  } else {
	    acceptor_score = Maxent_hr_antiacceptor_prob(chroffset + splicesitepos,chroffset);
	  }


	  splicesitepos = rightpair->genomepos;
	  debug11(printf("6. looking up splicesites_iit for donor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/-1)) {
	    debug11(printf(" => known\n"));
	    donor_score = 1.0;
	  } else {
	    donor_score = Maxent_hr_antidonor_prob(chroffset + splicesitepos,chroffset);
	  }

	} else {
	  splicesitepos = (chrhigh - chroffset) - leftpair->genomepos;
	  debug11(printf("7. looking up splicesites_iit for acceptor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,acceptor_typeint,/*sign*/+1)) {
	    debug11(printf(" => known\n"));
	    acceptor_score = 1.0;
	  } else {
	    acceptor_score = Maxent_hr_acceptor_prob(chroffset + splicesitepos,chroffset);
	  }

	  splicesitepos = (chrhigh - chroffset) - rightpair->genomepos + 1;
	  debug11(printf("8. looking up splicesites_iit for donor at #%d:%u..%u\n",chrnum,splicesitepos,splicesitepos+1));
	  if (splicesites_iit && IIT_exists_with_divno_typed_signed(splicesites_iit,splicesites_divint_crosstable[chrnum],
								    splicesitepos,splicesitepos+1U,donor_typeint,/*sign*/+1)) {
	    debug11(printf(" => known\n"));
	    donor_score = 1.0;
	  } else {
	    donor_score = Maxent_hr_donor_prob(chroffset + splicesitepos,chroffset);
	  }
	}
	debug11(printf("donor score at %u is %f, watson %d, cdna_direction %d\n",
		       leftpair->genomepos,donor_score,watsonp,cdna_direction));
	debug11(printf("acceptor score at %u is %f, watson %d, cdna_direction %d\n",
		       rightpair->genomepos,acceptor_score,watsonp,cdna_direction));
	nintrons += 1;
	if (pair->knowngapp == true) {
	  /* Skip */
	} else if (pair->comp == REV_CANONICAL_INTRON_COMP && (donor_score < 0.9 && acceptor_score < 0.9)) {
	  *nbadintrons += 1;
	}
	*avg_donor_score += donor_score;
	*avg_acceptor_score += acceptor_score;
	if (donor_score + acceptor_score > *max_intron_score) {
	  *max_intron_score = donor_score + acceptor_score;
	}

      }
      debug11(printf("\n"));

#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else if (pair->genomejump > pair->queryjump + SINGLESLEN) {
      /* Intron length shorter than MININTRONLEN_FINAL.  Just replace the gap */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif

    } else {
      /* Single gap; force fill */
#ifdef WASTE
      pairs = Pairpool_push_existing(pairs,pairpool,pair);
#else
      pairs = List_push_existing(pairs,pairptr);
#endif
    }
  }

  /* Want average scores */
  if (nintrons > 0) {
    *avg_donor_score /= (double) nintrons;
    *avg_acceptor_score /= (double) nintrons;
  }

  debug11(printf("max_intron_score = %f, avg_donor_score = %f, avg_acceptor_score = %f\n",
		 *max_intron_score,*avg_donor_score,*avg_acceptor_score));
  return pairs;
}  


static List_T
filter_goodness_hmm (bool *filterp, List_T pairs, double defect_rate) {
  Pair_T pair;
  List_T path, p;
  double prev_vprob_good = 0.0, prev_vprob_bad = 0.0, vprob_good, vprob_bad;
  double good_incr_prob, bad_incr_prob;
  double emission_prob;
  State_T state;

  if (defect_rate == 0.0) {
    defect_rate = 0.001;
  }

  debug5(printf("Beginning filter_goodness_hmm with defect rate %f\n",defect_rate));

  for (p = pairs; p != NULL; p = List_next(p)) {
    pair = (Pair_T) List_head(p);
    debug5(printf("hmm querypos %d (%c %c %c): ",pair->querypos,pair->genome,pair->comp,pair->cdna));

    /* state: GOOD */
    if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
      emission_prob = 1.0 - defect_rate;
    } else {
      emission_prob = defect_rate;
    }

    good_incr_prob = log(emission_prob) + log(/*transition_prob*/0.99); /* Prob(prev good state -> good state) */
    bad_incr_prob = log(emission_prob) + log(/*transition_prob*/0.10);  /* Prob(prev bad state -> good state) */

    debug5(printf("state GOOD: %f+%f %f+%f ",prev_vprob_good,good_incr_prob,prev_vprob_bad,bad_incr_prob));
    if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
      vprob_good = prev_vprob_good + good_incr_prob;
      pair->vstate_good = GOOD;
      debug5(printf(" =>GOOD.  "));
    } else {
      vprob_good = prev_vprob_bad + bad_incr_prob;
      pair->vstate_good = BAD;
      debug5(printf(" =>BAD.   "));
    }

    /* state: BAD */
    if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
      emission_prob = 0.25;
    } else {
      emission_prob = 0.75;
    }

    good_incr_prob = log(emission_prob) + log(/*transition_prob*/0.01);  /* Prob(prev good state -> bad state) */
    bad_incr_prob = log(emission_prob) + log(/*transition_prob*/0.90);  /* Prob(prev bad state -> bad state) */

    debug5(printf("state BAD: %f+%f %f+%f ",prev_vprob_good,good_incr_prob,prev_vprob_bad,bad_incr_prob));
    if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
      vprob_bad = prev_vprob_good + good_incr_prob;
      pair->vstate_bad = GOOD;
      debug5(printf(" =>GOOD.\n"));
    } else {
      vprob_bad = prev_vprob_bad + bad_incr_prob;
      pair->vstate_bad = BAD;
      debug5(printf(" =>BAD.\n"));
    }

    prev_vprob_good = vprob_good;
    prev_vprob_bad = vprob_bad;
  }

  if (prev_vprob_good > prev_vprob_bad) {
    state = GOOD;
  } else {
    state = BAD;
  }

  path = List_reverse(pairs);
  pairs = (List_T) NULL;

  *filterp = false;
  while (path != NULL) {
    pair = path->first;
    pair->state = state;

#ifdef DEBUG5
    Pair_dump_one(pair,/*zerobasedp*/false);
    printf("\n");
#endif

    if (state == GOOD) {
      pairs = List_transfer_one(pairs,&path);
      state = pair->vstate_good;
    } else {
      *filterp = true;
      path = path->rest;
      state = pair->vstate_bad;
    }
  }
  
  return pairs;
}


static List_T
filter_indels_hmm (bool *filterp, List_T pairs) {
  Pair_T pair;
  List_T path, p;
  double prev_vprob_good = 0.0, prev_vprob_bad = 0.0, vprob_good, vprob_bad;
  double good_incr_prob, bad_incr_prob;
  double emission_prob;
  State_T state;

  debug5(printf("Beginning filter_indels_hmm\n"));

  for (p = pairs; p != NULL; p = List_next(p)) {
    pair = (Pair_T) List_head(p);
    debug5(printf("indels querypos %d (%c %c %c): ",pair->querypos,pair->genome,pair->comp,pair->cdna));

    /* state: GOOD */
    /* These emission probs should add to 1.0 */
    if (pair->comp != INDEL_COMP) {
      emission_prob = 0.9999;	/* Prob(good state -> match/mismatch) */
    } else {
      emission_prob = 0.0001;	/* Prob(good state -> indel) */
    }

    /* These transition probs should complement those for state BAD */
    good_incr_prob = log(emission_prob) + log(/*transition_prob*/0.9999);  /* Prob(prev good state -> good state) */
    bad_incr_prob = log(emission_prob) + log(/*transition_prob*/0.25);   /* Prob(prev bad state -> good state) */

    debug5(printf("state GOOD: %f+%f %f+%f ",prev_vprob_good,good_incr_prob,prev_vprob_bad,bad_incr_prob));
    if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
      vprob_good = prev_vprob_good + good_incr_prob;
      pair->vstate_good = GOOD;
      debug5(printf(" =>GOOD.  "));
    } else {
      vprob_good = prev_vprob_bad + bad_incr_prob;
      pair->vstate_good = BAD;
      debug5(printf(" =>BAD.   "));
    }

    /* state: BAD */
    /* These emission probs should add to 1.0 */
    if (pair->comp != INDEL_COMP) {
      emission_prob = 0.5; 	/* Prob(bad state -> match/mismatch) */
    } else {
      emission_prob = 0.5;	/* Prob(bad state -> indel) */
    }

    good_incr_prob = log(emission_prob) + log(/*transition_prob*/0.0001);   /* Prob(prev good state -> bad state) */
    bad_incr_prob = log(emission_prob) + log(/*transition_prob*/0.75);  /* Prob(prev bad state -> bad state) */

    debug5(printf("state BAD: %f+%f %f+%f ",prev_vprob_good,good_incr_prob,prev_vprob_bad,bad_incr_prob));
    if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
      vprob_bad = prev_vprob_good + good_incr_prob;
      pair->vstate_bad = GOOD;
      debug5(printf(" =>GOOD.\n"));
    } else {
      vprob_bad = prev_vprob_bad + bad_incr_prob;
      pair->vstate_bad = BAD;
      debug5(printf(" =>BAD.\n"));
    }

    prev_vprob_good = vprob_good;
    prev_vprob_bad = vprob_bad;
  }

  if (prev_vprob_good > prev_vprob_bad) {
    state = GOOD;
  } else {
    state = BAD;
  }

  path = List_reverse(pairs);
  pairs = (List_T) NULL;

  *filterp = false;
  while (path != NULL) {
    pair = path->first;
    pair->state = state;

#ifdef DEBUG5
    Pair_dump_one(pair,/*zerobasedp*/false);
    printf("\n");
#endif

    if (state == GOOD) {
      pairs = List_transfer_one(pairs,&path);
      state = pair->vstate_good;
    } else {
      *filterp = true;
      path = path->rest;
      state = pair->vstate_bad;
    }
  }

  return pairs;
}



bool
Stage3_short_alignment_p (struct Pair_T *pairarray, int npairs, int querylength) {
  int querystart, queryend;

  if (npairs == 0) {
    return true;
  } else {
    querystart = pairarray[0].querypos;
    queryend = pairarray[npairs-1].querypos;
    if (queryend - querystart + 1 < querylength/3) {
      return true;
    } else {
      return false;
    }
  }
}


/* Uses hmm */
/* Modified from Substring_bad_stretch_p */
bool
Stage3_bad_stretch_p (struct Pair_T *pairarray, int npairs, int pos5, int pos3) {
  Pair_T pair;
  int i;
  double vprob_good, vprob_bad, prev_vprob_good, prev_vprob_bad, good_incr_prob, bad_incr_prob;
  bool indelp = false, mismatchp, matchp;

  /* Initialize priors */
#ifdef COMPUTE_LOG
  prev_vprob_good = log(0.99);
  prev_vprob_bad = log(0.01);
#else
  prev_vprob_good = LOG_99;
  prev_vprob_bad = LOG_01;
#endif

  for (i = 0; i < npairs; i++) {
    pair = &(pairarray[i]);
    if (pair->querypos < pos5) {
      /* Skip */
      matchp = mismatchp = false;
    } else if (pair->querypos >= pos3) {
      /* Skip */
      matchp = mismatchp = false;
    } else if (pair->gapp == true) {
      /* Skip */
      matchp = mismatchp = false;
      indelp = false;
    } else if (pair->comp == INDEL_COMP || pair->comp == SHORTGAP_COMP) {
      if (indelp == true) {
	/* Skip, because we count each indel just once  */
	matchp = mismatchp = false;
      } else {
	/* Count each gap as a mismatch */
	mismatchp = true;
	matchp = false;
	indelp = true;
      }
    } else if (pair->comp == MISMATCH_COMP) {
      mismatchp = true;
      matchp = false;
      indelp = false;
    } else {
      mismatchp = false;
      matchp = true;
      indelp = false;
    }
      
    if (mismatchp == true) {
      debug9(printf("querypos %d (mismatch): ",pair->querypos));

      /* state: GOOD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.001) + log(/*transition_prob*/0.999);
      bad_incr_prob = log(/*emission_prob*/0.001) + log(/*transition_prob*/0.001);
#else
      good_incr_prob = LOG_01_999;
      bad_incr_prob = LOG_01_001;
#endif

      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_good = prev_vprob_good + good_incr_prob;
      } else {
	/* vprob_good = prev_vprob_bad + bad_incr_prob; */
	return true;
      }

      /* state: BAD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.75) + log(/*transition_prob*/0.001);
      bad_incr_prob = log(/*emission_prob*/0.75) + log(/*transition_prob*/0.999);
#else
      good_incr_prob = LOG_75_001;
      bad_incr_prob = LOG_75_999;
#endif
      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_bad = prev_vprob_good + good_incr_prob;
      } else {
	vprob_bad = prev_vprob_bad + bad_incr_prob;
      }

      debug9(printf("vprob_good %f, vprob_bad %f",vprob_good,vprob_bad));
      prev_vprob_good = vprob_good;
      prev_vprob_bad = vprob_bad;
      debug9(printf("\n"));

    } else if (matchp == true) {
      debug9(printf("querypos %d (match): ",pair->querypos));

      /* state: GOOD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.999) + log(/*transition_prob*/0.999);
      bad_incr_prob = log(/*emission_prob*/0.999) + log(/*transition_prob*/0.001);
#else
      good_incr_prob = LOG_99_999;
      bad_incr_prob = LOG_99_001;
#endif

      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_good = prev_vprob_good + good_incr_prob;
      } else {
	/* vprob_good = prev_vprob_bad + bad_incr_prob; */
	return true;
      }

      /* state: BAD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.25) + log(/*transition_prob*/0.001);
      bad_incr_prob = log(/*emission_prob*/0.25) + log(/*transition_prob*/0.999);
#else
      good_incr_prob = LOG_25_001;
      bad_incr_prob = LOG_25_999;
#endif
      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_bad = prev_vprob_good + good_incr_prob;
      } else {
	vprob_bad = prev_vprob_bad + bad_incr_prob;
      }

      debug9(printf("vprob_good %f, vprob_bad %f",vprob_good,vprob_bad));
      prev_vprob_good = vprob_good;
      prev_vprob_bad = vprob_bad;
      debug9(printf("\n"));
    }
    
  }

  return false;
}


int
Stage3_good_part (struct Pair_T *pairarray, int npairs, int pos5, int pos3) {
  Pair_T pair;
  int i;
  double vprob_good, vprob_bad, prev_vprob_good, prev_vprob_bad, good_incr_prob, bad_incr_prob;
  bool indelp = false, mismatchp, matchp, stopp;
  int ngoodleft, ngoodright;

  /* Initialize priors */
#ifdef COMPUTE_LOG
  prev_vprob_good = log(0.99);
  prev_vprob_bad = log(0.01);
#else
  prev_vprob_good = LOG_99;
  prev_vprob_bad = LOG_01;
#endif

  ngoodleft = 0;
  stopp = false;
  for (i = 0; i < npairs && stopp == false; i++) {
    pair = &(pairarray[i]);
    if (pair->querypos < pos5) {
      /* Skip */
      matchp = mismatchp = false;
    } else if (pair->querypos >= pos3) {
      /* Skip */
      matchp = mismatchp = false;
    } else if (pair->gapp == true) {
      /* Skip */
      matchp = mismatchp = false;
      indelp = false;
    } else if (pair->comp == INDEL_COMP || pair->comp == SHORTGAP_COMP) {
      if (indelp == true) {
	/* Skip, because we count each indel just once  */
	matchp = mismatchp = false;
      } else {
	/* Count each gap as a mismatch */
	mismatchp = true;
	matchp = false;
	indelp = true;
      }
    } else if (pair->comp == MISMATCH_COMP) {
      mismatchp = true;
      matchp = false;
      indelp = false;
    } else {
      mismatchp = false;
      matchp = true;
      indelp = false;
    }
      
    if (mismatchp == true) {
      debug9(printf("querypos %d (mismatch): ",pair->querypos));

      /* state: GOOD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.001) + log(/*transition_prob*/0.999);
      bad_incr_prob = log(/*emission_prob*/0.001) + log(/*transition_prob*/0.001);
#else
      good_incr_prob = LOG_01_999;
      bad_incr_prob = LOG_01_001;
#endif

      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_good = prev_vprob_good + good_incr_prob;
	ngoodleft++;
      } else {
	/* vprob_good = prev_vprob_bad + bad_incr_prob; */
	stopp = true;
      }

      /* state: BAD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.75) + log(/*transition_prob*/0.001);
      bad_incr_prob = log(/*emission_prob*/0.75) + log(/*transition_prob*/0.999);
#else
      good_incr_prob = LOG_75_001;
      bad_incr_prob = LOG_75_999;
#endif
      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_bad = prev_vprob_good + good_incr_prob;
      } else {
	vprob_bad = prev_vprob_bad + bad_incr_prob;
      }

      debug9(printf("vprob_good %f, vprob_bad %f",vprob_good,vprob_bad));
      prev_vprob_good = vprob_good;
      prev_vprob_bad = vprob_bad;
      debug9(printf("\n"));

    } else if (matchp == true) {
      debug9(printf("querypos %d (match): ",pair->querypos));

      /* state: GOOD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.999) + log(/*transition_prob*/0.999);
      bad_incr_prob = log(/*emission_prob*/0.999) + log(/*transition_prob*/0.001);
#else
      good_incr_prob = LOG_99_999;
      bad_incr_prob = LOG_99_001;
#endif

      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_good = prev_vprob_good + good_incr_prob;
	ngoodleft++;
      } else {
	/* vprob_good = prev_vprob_bad + bad_incr_prob; */
	stopp = true;
      }

      /* state: BAD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.25) + log(/*transition_prob*/0.001);
      bad_incr_prob = log(/*emission_prob*/0.25) + log(/*transition_prob*/0.999);
#else
      good_incr_prob = LOG_25_001;
      bad_incr_prob = LOG_25_999;
#endif
      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_bad = prev_vprob_good + good_incr_prob;
      } else {
	vprob_bad = prev_vprob_bad + bad_incr_prob;
      }

      debug9(printf("vprob_good %f, vprob_bad %f",vprob_good,vprob_bad));
      prev_vprob_good = vprob_good;
      prev_vprob_bad = vprob_bad;
      debug9(printf("\n"));
    }
    
  }


  /* Initialize priors */
#ifdef COMPUTE_LOG
  prev_vprob_good = log(0.99);
  prev_vprob_bad = log(0.01);
#else
  prev_vprob_good = LOG_99;
  prev_vprob_bad = LOG_01;
#endif

  ngoodright = 0;
  stopp = false;
  for (i = npairs - 1; i >= 0 && stopp == false; i--) {
    pair = &(pairarray[i]);
    if (pair->querypos < pos5) {
      /* Skip */
      matchp = mismatchp = false;
    } else if (pair->querypos >= pos3) {
      /* Skip */
      matchp = mismatchp = false;
    } else if (pair->gapp == true) {
      /* Skip */
      matchp = mismatchp = false;
      indelp = false;
    } else if (pair->comp == INDEL_COMP || pair->comp == SHORTGAP_COMP) {
      if (indelp == true) {
	/* Skip, because we count each indel just once  */
	matchp = mismatchp = false;
      } else {
	/* Count each gap as a mismatch */
	mismatchp = true;
	matchp = false;
	indelp = true;
      }
    } else if (pair->comp == MISMATCH_COMP) {
      mismatchp = true;
      matchp = false;
      indelp = false;
    } else {
      mismatchp = false;
      matchp = true;
      indelp = false;
    }
      
    if (mismatchp == true) {
      debug9(printf("querypos %d (mismatch): ",pair->querypos));

      /* state: GOOD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.001) + log(/*transition_prob*/0.999);
      bad_incr_prob = log(/*emission_prob*/0.001) + log(/*transition_prob*/0.001);
#else
      good_incr_prob = LOG_01_999;
      bad_incr_prob = LOG_01_001;
#endif

      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_good = prev_vprob_good + good_incr_prob;
	ngoodright++;
      } else {
	/* vprob_good = prev_vprob_bad + bad_incr_prob; */
	stopp = true;
      }

      /* state: BAD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.75) + log(/*transition_prob*/0.001);
      bad_incr_prob = log(/*emission_prob*/0.75) + log(/*transition_prob*/0.999);
#else
      good_incr_prob = LOG_75_001;
      bad_incr_prob = LOG_75_999;
#endif
      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_bad = prev_vprob_good + good_incr_prob;
      } else {
	vprob_bad = prev_vprob_bad + bad_incr_prob;
      }

      debug9(printf("vprob_good %f, vprob_bad %f",vprob_good,vprob_bad));
      prev_vprob_good = vprob_good;
      prev_vprob_bad = vprob_bad;
      debug9(printf("\n"));

    } else if (matchp == true) {
      debug9(printf("querypos %d (match): ",pair->querypos));

      /* state: GOOD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.999) + log(/*transition_prob*/0.999);
      bad_incr_prob = log(/*emission_prob*/0.999) + log(/*transition_prob*/0.001);
#else
      good_incr_prob = LOG_99_999;
      bad_incr_prob = LOG_99_001;
#endif

      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_good = prev_vprob_good + good_incr_prob;
	ngoodright++;
      } else {
	/* vprob_good = prev_vprob_bad + bad_incr_prob; */
	stopp = true;
      }

      /* state: BAD */
#ifdef COMPUTE_LOG
      good_incr_prob = log(/*emission_prob*/0.25) + log(/*transition_prob*/0.001);
      bad_incr_prob = log(/*emission_prob*/0.25) + log(/*transition_prob*/0.999);
#else
      good_incr_prob = LOG_25_001;
      bad_incr_prob = LOG_25_999;
#endif
      if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
	vprob_bad = prev_vprob_good + good_incr_prob;
      } else {
	vprob_bad = prev_vprob_bad + bad_incr_prob;
      }

      debug9(printf("vprob_good %f, vprob_bad %f",vprob_good,vprob_bad));
      prev_vprob_good = vprob_good;
      prev_vprob_bad = vprob_bad;
      debug9(printf("\n"));
    }
    
  }

  debug9(printf("ngoodleft = %d, ngoodright = %d\n",ngoodleft,ngoodright));

  if (ngoodleft > ngoodright) {
    return ngoodleft;
  } else {
    return ngoodright;
  }
}




#if 0
static int
score_nconsecutive (List_T pairs) {
  int score = 0, nconsecutive;
  Pair_T pair;
  List_T path, p;
  double prev_vprob_good = 0.0, prev_vprob_bad = 0.0, vprob_good, vprob_bad;
  double defect_rate = 0.001, good_incr_prob, bad_incr_prob;
  double emission_prob;
  State_T state;


  debug(printf("Beginning score_nconsecutive\n"));

  for (p = pairs; p != NULL; p = List_next(p)) {
    pair = (Pair_T) List_head(p);
    debug5(printf("hmm querypos %d (%c %c %c): ",pair->querypos,pair->genome,pair->comp,pair->cdna));

    /* state: GOOD */
    if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
      emission_prob = 1.0 - defect_rate;
    } else {
      emission_prob = defect_rate;
    }

    good_incr_prob = log(emission_prob) + log(/*transition_prob*/0.99); /* Prob(prev good state -> good state) */
    bad_incr_prob = log(emission_prob) + log(/*transition_prob*/0.10);  /* Prob(prev bad state -> good state) */

    debug5(printf("state GOOD: %f+%f %f+%f ",prev_vprob_good,good_incr_prob,prev_vprob_bad,bad_incr_prob));
    if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
      vprob_good = prev_vprob_good + good_incr_prob;
      pair->vstate_good = GOOD;
      debug5(printf(" =>GOOD.  "));
    } else {
      vprob_good = prev_vprob_bad + bad_incr_prob;
      pair->vstate_good = BAD;
      debug5(printf(" =>BAD.   "));
    }

    /* state: BAD */
    if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
      emission_prob = 0.25;
    } else {
      emission_prob = 0.75;
    }

    good_incr_prob = log(emission_prob) + log(/*transition_prob*/0.01);  /* Prob(prev good state -> bad state) */
    bad_incr_prob = log(emission_prob) + log(/*transition_prob*/0.90);  /* Prob(prev bad state -> bad state) */

    debug5(printf("state BAD: %f+%f %f+%f ",prev_vprob_good,good_incr_prob,prev_vprob_bad,bad_incr_prob));
    if (prev_vprob_good + good_incr_prob > prev_vprob_bad + bad_incr_prob) {
      vprob_bad = prev_vprob_good + good_incr_prob;
      pair->vstate_bad = GOOD;
      debug5(printf(" =>GOOD.\n"));
    } else {
      vprob_bad = prev_vprob_bad + bad_incr_prob;
      pair->vstate_bad = BAD;
      debug5(printf(" =>BAD.\n"));
    }

    prev_vprob_good = vprob_good;
    prev_vprob_bad = vprob_bad;
  }

  if (prev_vprob_good > prev_vprob_bad) {
    state = GOOD;
  } else {
    state = BAD;
  }

  nconsecutive = 0;
  path = List_reverse(pairs);

  for (p = path; p != NULL; p = p->rest) {
    pair = p->first;
    debug5(printf("hmm querypos %d (%c %c %c): ",pair->querypos,pair->genome,pair->comp,pair->cdna));
    if (state == GOOD) {
      nconsecutive++;
      debug5(printf("state is GOOD, nconsecutive %d\n",nconsecutive));
      state = pair->vstate_good;
    } else {
      if (nconsecutive > score) {
	score = nconsecutive;
      }
      nconsecutive = 0;
      debug5(printf("state is BAD, score %d\n",score));
      state = pair->vstate_bad;
    }
  }
  
  if (nconsecutive > score) {
    score = nconsecutive;
  }
  debug5(printf("At end, score %d\n",score));

  pairs = List_reverse(path);

  return score;
}
#endif



static List_T
path_compute (double *defect_rate, int *ambig_end_length_5, int *ambig_end_length_3,
	      Splicetype_T *ambig_splicetype_5, Splicetype_T *ambig_splicetype_3,
	      List_T path, int cdna_direction, bool watsonp, int genestrand, bool jump_late_p,
	      int querylength,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
	      int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
	      char *queryaaseq_ptr, char *genomicuc_ptr,
#endif
	      char *queryseq_ptr, char *queryuc_ptr,
	      Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
	      Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
	      int maxpeelback, int maxpeelback_distalmedial, int nullgap,
	      int extramaterial_end, int extraband_end,
	      int extramaterial_paired, int extraband_single, int extraband_paired,
	      Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
	      Stage3debug_T stage3debug, bool do_final_p,
	      Oligoindex_T *oligoindices_minor, int noligoindices_minor, Diagpool_T diagpool, int sufflookback, int nsufflookback,
	      int maxintronlen_bound, int close_indels_mode, int paired_favor_mode, int zero_offset) {
  List_T pairs = NULL;
  int iter0, iter1, iter2;
  bool shiftp, incompletep, shortp, deletep;
  int dynprogindex_minor = DYNPROGINDEX_MINOR, dynprogindex_major = DYNPROGINDEX_MAJOR;
  int matches, unknowns, mismatches, qopens, qindels, topens, tindels,
    ncanonical, nsemicanonical, nnoncanonical;
  double min_splice_prob;
  bool filterp = true, dual_break_p = true;
  int distance5, distance3, totaljump5, totaljump3, npairs5, npairs3, donep;
  bool knownsplice5p, knownsplice3p, ignorep, chop_exon_p;
#ifdef DEBUG
  Pair_T firstpair, lastpair;
#endif
  bool trim5p, trim3p, adjacent_indels_p;


  *ambig_end_length_5 = *ambig_end_length_3 = 0;
  if (path == NULL) {
    return NULL;
  }

  if (stage3debug == POST_STAGE2) {
    pairs = List_reverse(path);
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
    pairs = List_reverse(path);
    return pairs;
  }

  iter0 = 0;
  while ((filterp == true /*|| dual_break_p == true */) && iter0 < MAXITER_CYCLES) {
    /* Pass 0: Insert gaps.  pairs --> pairs */
    debug(printf("\n*** Pass 0 (dir %d): Solve single nucleotide gaps.  Iteration0 %d\n",
		 cdna_direction,iter0));

    pairs = List_reverse(path);
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
    pairs = List_reverse(path);

    /* Pass 1: Initial smoothing.  pairs --> path */
    debug(printf("\n*** Pass 1 (dir %d): Initial smoothing by net gap.  Iteration0 %d\n",
		 cdna_direction,iter0));
    pairs = Smooth_pairs_by_netgap(&deletep,pairs,pairpool);
    if (deletep == true) {
      path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
    } else {
      path = List_reverse(pairs);
    }

    if (stage3debug == POST_SMOOTHING) {
      pairs = List_reverse(path);
      return pairs;
    }

#ifdef PMAP
    /* Pass 1b: undefine nucleotides around gaps.  path --> path */
    pairs = undefine_nucleotides(queryseq_ptr,querylength,path,pairpool,/*width*/6);
    path = List_reverse(pairs);
#endif

    /* Pass 2A: solve straight gaps.  path --> pairs (for defect rate) */
    debug(printf("\n*** Pass 2A (dir %d): Solve straight gaps.  Iteration0 %d\n",
		 cdna_direction,iter0));
    pairs = build_pairs_singles(&dynprogindex_minor,path,
				chroffset,chrhigh,
#ifdef PMAP
				queryaaseq_ptr,
#endif
				queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
				jump_late_p,maxpeelback,nullgap,extraband_single,
				/*defect_rate*/0.0,close_indels_mode,pairpool,dynprogM);


#if 1
    /* Pass 2B: fix adjacent indels */
#if 0
    /* gapholders shouldn't be necessary before fix_adjacent_indels,
       but is necessary afterward for build_pairs_singles */
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
    pairs = List_reverse(path);
#endif

    debug(printf("\n*** Pass 2B (dir %d): Fix adjacent indels.  Iteration0 %d\n",
		 cdna_direction,iter0));
    path = fix_adjacent_indels(pairs);
    pairs = List_reverse(path);
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);


    /* Pass 2C: solve straight gaps again.  path --> pairs (for defect rate) */
    debug(printf("\n*** Pass 2C (dir %d): Solve straight gaps again.  Iteration0 %d\n",
		 cdna_direction,iter0));
    pairs = build_pairs_singles(&dynprogindex_minor,path,
				chroffset,chrhigh,
#ifdef PMAP
				queryaaseq_ptr,
#endif
				queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
				jump_late_p,maxpeelback,nullgap,extraband_single,
				/*defect_rate*/0.0,close_indels_mode,pairpool,dynprogM);

#endif

    if (stage3debug == POST_SINGLES) {
      path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
      pairs = List_reverse(path);
      return pairs;
    }


    /* Compute defect rate here */
    Pair_fracidentity(&matches,&unknowns,&mismatches,&qopens,&qindels,&topens,&tindels,
		      &ncanonical,&nsemicanonical,&nnoncanonical,&min_splice_prob,
		      pairs,/*cdna_direction*/0);
    if (matches + mismatches == 0) {
      *defect_rate = 0.0;
    } else {
      *defect_rate = (double) mismatches/(double) (matches + mismatches);
    }
    debug(printf("defect_rate = %f (%d matches, %d mismatches)\n",*defect_rate,matches,mismatches));
    

    /* Pass 3: introns.  pairs --> pairs */
    debug(printf("\n*** Pass 3: Smooth and solve dual introns iteratively.  Iteration0 %d\n",iter0));
    iter1 = 0;
    shortp = true;
    while (shortp == true && iter1 < MAXITER_SMOOTH_BY_SIZE) {
      /* Pass 3a: smoothing.  pairs --> pairs */
      debug(printf("*** Pass 3a: Smoothing by size.  Iteration0 %d, iteration1 %d\n",iter0,iter1));
      path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
      pairs = List_reverse(path);
      pairs = Smooth_pairs_by_size(&shortp,&deletep,pairs,pairpool,/*stage2_indexsize*/6);
      debug(printf("  => Result of Pass 3a (smoothing): shortp is %d, deletep is %d\n",shortp,deletep));
      debug(Pair_dump_list(pairs,/*zerobasedp*/true));
      
      /* Pass 3b: dual introns.  pairs --> pairs */
      debug(printf("*** Pass 3b: Solve dual introns.  Iteration0 %d, Iteration1 %d\n",iter0,iter1));
      if (shortp == false && deletep == false) {
	debug(printf("  no shortp or deletep, so do nothing\n"));
      } else {
	debug(printf("  shortp or deletep is true, so running build_pairs_dualintrons\n"));
	path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
	/* XX */
	/* pairs = assign_gap_types(path,cdna_direction,watsonp,queryseq_ptr,
	   chrnum,chroffset,chrhigh,pairpool); */

	pairs = remove_indel_gaps(path);
	path = List_reverse(pairs);
#if 0
	/* Smooth_pairs_by_size should mark shortexons correctly */
	pairs = Smooth_mark_short_exons(pairs,pairpool,/*stage2_indexsize*/6);
#endif
	pairs = build_pairs_dualintrons(&dynprogindex_major,path,chrnum,chroffset,chrhigh,
#ifdef PMAP
					queryaaseq_ptr,
#endif
					queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,jump_late_p,
					maxpeelback,nullgap,extramaterial_paired,extraband_paired,
					*defect_rate,pairpool,dynprogL,dynprogR);
	debug(printf("  => Result of Pass 3b (dual introns):\n"));
	debug(Pair_dump_list(pairs,/*zerobasedp*/true));
      }

      /* Pass 3c: single introns.  pairs --> pairs */
      iter2 = 0;
      shiftp = incompletep = true;
      debug(printf("\n*** Pass 3c: Solve introns iteratively\n"));
      while ((shiftp == true || incompletep == true) && iter2 < MAXITER_INTRONS) {
	debug(printf("*** Pass 3c: Solve introns.  Iteration0 %d, iteration1 %d, iteration2 %d\n",
		     iter0,iter1,iter2));
	path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
	/* XX */
	/* pairs = assign_gap_types(path,cdna_direction,watsonp,queryseq_ptr,
	   chrnum,chroffset,chrhigh,pairpool); */
	pairs = remove_indel_gaps(path);
	path = List_reverse(pairs);
	pairs = build_pairs_introns(&shiftp,&incompletep,
				    &dynprogindex_minor,&dynprogindex_major,path,
				    chrnum,chroffset,chrhigh,
#ifdef PMAP
				    queryaaseq_ptr,
#endif
				    queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,jump_late_p,
				    maxpeelback,nullgap,extramaterial_paired,extraband_single,extraband_paired,
				    *defect_rate,close_indels_mode,pairpool,dynprogL,dynprogM,dynprogR,/*finalp*/false);
	debug(printf("  => Result of Pass 3c (introns):\n"));
	debug(Pair_dump_list(pairs,/*zerobasedp*/true));

	iter2++;
      }

      iter1++;
    }

    if (stage3debug == POST_INTRONS) {
      path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
      pairs = List_reverse(path);
      return pairs;
    }

    /* Pass 3b: trim end exons: pairs -> pairs */
    debug(printf("\n*** Pass 3b: Trim end exons\n"));
#ifdef WASTE
    pairs = chop_ends_by_changepoint(pairs,pairpool);
#else
    pairs = chop_ends_by_changepoint(pairs);
#endif
    debug(Pair_dump_list(pairs,/*zerobasedp*/true));


    /* Pass 4: Remove bad sections */
    debug(printf("\n*** Pass 4: Remove bad sections.  Iteration0 %d\n",iter0));
    Pair_fracidentity(&matches,&unknowns,&mismatches,&qopens,&qindels,&topens,&tindels,
		      &ncanonical,&nsemicanonical,&nnoncanonical,&min_splice_prob,
		      pairs,/*cdna_direction*/0);
    if (matches + mismatches == 0) {
      *defect_rate = 0.0;
    } else {
      *defect_rate = (double) mismatches/(double) (matches + mismatches);
    }
    pairs = filter_goodness_hmm(&filterp,pairs,*defect_rate);
    pairs = filter_indels_hmm(&filterp,pairs);
    filterp = false;		/* Do not iterate */
    debug(Pair_dump_list(pairs,/*zerobasedp*/true));

    if (stage3debug == POST_HMM) {
      path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
      pairs = List_reverse(path);
      return pairs;
    }

    /* Pass 5: Fix dual breaks */
    debug(printf("\n*** Pass 5: Fix dual breaks.  Iteration0 %d\n",iter0));
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);

#if 0
    /* Can no longer assign_gap_types here; just remove indel gaps  */
    pairs = assign_gap_types(&npairs,&ngaps,&ncanonical,path,pairpool,queryseq_ptr,genomicuc_ptr,cdna_direction);
#elif defined(WASTE)
    pairs = remove_indel_gaps(path,pairpool);
#else
    pairs = remove_indel_gaps(path);
#endif
    path = List_reverse(pairs);

    pairs = build_dual_breaks(&dual_break_p,&dynprogindex_minor,&dynprogindex_major,path,
			      chrnum,chroffset,chrhigh,
#ifdef PMAP
			      queryaaseq_ptr,genomicuc_ptr,
#endif
			      queryseq_ptr,queryuc_ptr,
			      cdna_direction,watsonp,genestrand,jump_late_p,pairpool,
			      dynprogL,dynprogM,dynprogR,maxpeelback,
			      oligoindices_minor,noligoindices_minor,diagpool,sufflookback,nsufflookback,
			      maxintronlen_bound,extramaterial_paired,extraband_paired,extraband_single,
			      *defect_rate,close_indels_mode);
    /* Must end with path to start loop */
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);

    if (stage3debug == POST_DUAL_BREAKS) {
      pairs = List_reverse(path);
      return pairs;
    }

    iter0++;
  }
  /* End of iter0 on pass 1-5 */

  if (stage3debug == POST_CYCLES) {
    pairs = List_reverse(path);
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
    pairs = List_reverse(path);
    return pairs;
  }


  /* Pass 6: Final pass to solve for introns with higher rewards for canonical introns: path --> pairs */
  pairs = List_reverse(path);
  if (do_final_p == true) {
    debug(printf("\n*** Pass 6: Final pass to find canonical introns\n"));
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
    /* XX */
    /* pairs = assign_gap_types(path,cdna_direction,watsonp,queryseq_ptr,
       chrnum,chroffset,chrhigh,pairpool); */
    pairs = remove_indel_gaps(path);
    path = List_reverse(pairs);
    pairs = build_pairs_introns(&shiftp,&incompletep,
				&dynprogindex_minor,&dynprogindex_major,path,
				chrnum,chroffset,chrhigh,
#ifdef PMAP
				queryaaseq_ptr,
#endif
				queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,jump_late_p,
				maxpeelback,nullgap,extramaterial_paired,extraband_single,extraband_paired,
				*defect_rate,close_indels_mode,pairpool,dynprogL,dynprogM,dynprogR,/*finalp*/true);
  }

  if (stage3debug == POST_CANONICAL) {
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
    pairs = List_reverse(path);
    return pairs;
  }


  /* Pass 7: Remove dual breaks at ends: pairs -> pairs */
  debug(printf("\n*** Pass 7: Remove dual breaks.\n"));
  path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
  donep = false;
  while (donep == false) {
    if (path == NULL) {
      donep = true;
    } else if (dualbreak_p(path) == false) {
      donep = true;
    } else {
      distance3 = dualbreak_distance_from_end(&npairs3,&totaljump3,path);
      pairs = List_reverse(path);
      distance5 = dualbreak_distance_from_end(&npairs5,&totaljump5,pairs);
      debug(printf("distance5 = %d, totaljump5 = %d, npairs5 %d\n",distance5,totaljump5,npairs5));
      debug(printf("distance3 = %d, totaljump3 = %d, npairs3 %d\n",distance3,totaljump3,npairs3));
      if (totaljump5 < distance5 && totaljump3 < distance3) {
	/* Keep dual break(s) => already have pairs */
	donep = true;
      } else if (totaljump5 > distance5 && totaljump3 > distance3) {
	if (distance5 < distance3) {
	  debug(printf("trimming shorter dual break on 5' end, %d pairs\n",npairs5));
	  pairs = trim_npairs(pairs,npairs5);
	  path = List_reverse(pairs);
	} else {
	  debug(printf("trimming shorter dual break on 3' end, %d pairs\n",npairs3));
	  path = List_reverse(pairs);
	  path = trim_npairs(path,npairs3);
	}
      } else if (totaljump5 > distance5) {
	debug(printf("trimming only dual break on 5' end, %d pairs\n",npairs5));
	pairs = trim_npairs(pairs,npairs5);
	path = List_reverse(pairs);
      } else {
	debug(printf("trimming only dual break on 3' end, %d pairs\n",npairs3));
	path = List_reverse(pairs);
	path = trim_npairs(path,npairs3);
      }
    }
  }
  pairs = List_reverse(path);


  /* Pass 7b: Fix adjacent insertion/deletion events: pairs -> pairs */
  debug(printf("\n*** Pass 7b: Fix adjacent indels again\n"));
  path = remove_adjacent_ins_del(&adjacent_indels_p,pairs);
  pairs = List_reverse(path);

  if (adjacent_indels_p == true) {
    path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);

    /* Pass 7c: solve straight gaps again.  path --> pairs (for defect rate) */
    debug(printf("\n*** Pass 7C (dir %d): Solve straight gaps again.  Iteration0 %d\n",
		 cdna_direction,iter0));
    pairs = build_pairs_singles(&dynprogindex_minor,path,
				chroffset,chrhigh,
#ifdef PMAP
				queryaaseq_ptr,
#endif
				queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
				jump_late_p,maxpeelback,nullgap,extraband_single,
				/*defect_rate*/0.0,close_indels_mode,pairpool,dynprogM);
  }


  path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
#ifdef WASTE
  /* pairs = remove_indel_gaps(path,pairpool); */
  pairs = List_reverse(path);
#else
  /* Don't want to remove end indels already found.  extend_ending5 and extend_ending3 will peelback and quit at gaps */
  /* pairs = remove_indel_gaps(path); */
  pairs = List_reverse(path);
#endif
    

  /* Testing for GSNAP shows that we want pass 8 with QUERYEND_NOGAPS
     and pass 9 with QUERYEND_INDELS.  Do not want to extend to ends
     for GMAP. */

  /* Extend to ends: pairs --> pairs */
  debug(printf("\n*** Pass 8: Extend to ends and determine distalmedial\n"));

  /* Extend to query end, so we get an accurate count of matches and mismatches */
  pairs = clean_pairs_end5_gap_indels(pairs);
  pairs = build_pairs_end5(&knownsplice5p,&(*ambig_end_length_5),&(*ambig_splicetype_5),
			   &chop_exon_p,&dynprogindex_minor,pairs,
			   chroffset,chrhigh,
			   knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			   cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			   queryaaseq_ptr,
#endif
			   queryseq_ptr,queryuc_ptr,
			   cdna_direction,watsonp,jump_late_p,
			   maxpeelback,maxpeelback_distalmedial,
			   nullgap,extramaterial_end,extraband_end,
			   *defect_rate,pairpool,dynprogR,
			   /*extendp*/true,/*endalign*/QUERYEND_GAP);

  /* Extend to query end, so we get an accurate count of matches and mismatches */
  path = List_reverse(pairs);
  path = clean_path_end3_gap_indels(path);

  debug(printf("Calling build_path_end3 in pass 8\n"));
  path = build_path_end3(&knownsplice3p,&(*ambig_end_length_3),&(*ambig_splicetype_3),
			 &chop_exon_p,&dynprogindex_minor,path,
			 chroffset,chrhigh,querylength,
			 knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			 cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			 queryaaseq_ptr,
#endif
			 queryseq_ptr,queryuc_ptr,
			 cdna_direction,watsonp,jump_late_p,
			 maxpeelback,maxpeelback_distalmedial,
			 nullgap,extramaterial_end,extraband_end,
			 *defect_rate,pairpool,dynprogL,
			 /*extendp*/true,/*endalign*/QUERYEND_GAP);

#if 0 /* Skip pass 9.  Rely on trimming non-canonical instead. */

  /* Pass 9 was once distalmedial (extendp = false), but then switched
     to QUERYEND_INDELS, but that appears to cause too many false
     positive indels at the ends.  Now using QUERYEND_NOGAPS to get
     accurate mismatch counts for trim_noncanonical_ends. */
  /* Compare distal exons with medial extensions */
#if 0
  debug(printf("\n*** Pass 9: Compare distal exons with medial extensions\n"));
#else
  debug(printf("\n*** Pass 9: Extend ends for noncanonical end exon trimming\n"));
#endif

  /* Need to assign gap types because traversal of ends depends on
     knowing if end introns are canonical.  Need to insert gapholders
     before assign gap types */
  pairs = List_reverse(path);
  path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);

  /* 9a. solve 5' end trying both distal and medial: pairs --> pairs */
  debug(printf("\n*** Pass 9a: Solve 5' end\n"));
  /* Need to assign gap types before distalmedial */
  pairs = assign_gap_types(path,cdna_direction,watsonp,queryseq_ptr,
			   chrnum,chroffset,chrhigh,pairpool);

  debug(printf("knownsplice5p = %d\n",knownsplice5p));
  /* Previously did build_pairs only if knownsplice5p was false */
  /* Don't wipe out information about knownsplice5p */
  pairs = build_pairs_end5(/*knownsplice5p*/&ignorep,&(*ambig_end_length_5),&(*ambig_splicetype_5),
			   &chop_exon_p,&dynprogindex_minor,pairs,
			   chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			   cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			   queryaaseq_ptr,
#endif
			   queryseq_ptr,queryuc_ptr,
			   cdna_direction,watsonp,jump_late_p,
			   /*maxpeelback*/0,maxpeelback_distalmedial,
			   nullgap,extramaterial_end,extraband_end,
			   *defect_rate,pairpool,dynprogR,
			   /*extendp*/true,
#ifdef GSNAP
			   /*endalign*/QUERYEND_NOGAPS
#else
			   /*endalign*/BEST_LOCAL
#endif
			   );
  /* endalign above must be BEST_LOCAL, at least for GMAP, otherwise
     it will truncate all ends of alignments.  But for GSNAP,
     QUERYEND_NOGAPS is needed for trimming procedure to work. */
  /* Previously extendp was false */

  debug(
	if (pairs) {
	  firstpair = pairs->first;
	  printf("5' starts at %d\n",firstpair->querypos);
	});


  /* 9b: solve 3' end trying both distal and medial: pairs --> pairs */
  debug(printf("\n*** Pass 9b: Solve 3' end\n"));

#if 0
  /* Need to assign gap types before distalmedial.  Already assigned previously for 5' end. */
  pairs = assign_gap_types(path,cdna_direction,watsonp,queryseq_ptr,
			   chrnum,chroffset,chrhigh,pairpool);
#endif

  debug(printf("Calling build_path_end3 in pass 9b\n"));
  path = List_reverse(pairs);

  debug(printf("knownsplice3p = %d\n",knownsplice3p));
  /* Previously did build_path only if knownsplice3p was false */
  /* Don't wipe out information about knownsplice3p */
  path = build_path_end3(/*knownsplice3p*/&ignorep,&(*ambig_end_length_3),&(*ambig_splicetype_3),
			 &chop_exon_p,&dynprogindex_minor,path,
			 chroffset,chrhigh,querylength,
			 knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			 cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			 queryaaseq_ptr,
#endif
			 queryseq_ptr,queryuc_ptr,
			 cdna_direction,watsonp,jump_late_p,
			 /*maxpeelback*/0,maxpeelback_distalmedial,
			 nullgap,extramaterial_end,extraband_end,
			 *defect_rate,pairpool,dynprogL,
			 /*extendp*/true,
#ifdef GSNAP
			 /*endalign*/QUERYEND_NOGAPS
#else
			 /*endalign*/BEST_LOCAL
#endif
			 );
  /* endalign above must be BEST_LOCAL, at least for GMAP, otherwise
     it will truncate all ends of alignments.  But for GSNAP,
     QUERYEND_NOGAPS is needed for trimming procedure to work. */
  /* Previously extendp was false */

  debug(
	if (path) {
	  lastpair = path->first;
	  printf("3' ends at %d\n",lastpair->querypos);
	});

#endif /* Skip pass 9 */

  /* Necessary to insert gaps and assign gap types (fills in cDNA
     insertions, so they don't get trimmed), in case an insertion was
     introduced at ends */
  pairs = List_reverse(path);
  path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
  pairs = assign_gap_types(path,cdna_direction,watsonp,queryseq_ptr,
			   chrnum,chroffset,chrhigh,pairpool);

  /* Pass 10: Remove noncanonical end exons: pairs -> pairs */
  debug(printf("\n*** Pass 10: Remove noncanonical end exons\n"));

  /* Don't trim at known splice sites */
  if (knownsplice5p == true) {
    trim5p = false;
  } else {
    trim5p = true;
  }

  if (knownsplice3p == true) {
    trim3p = false;
  } else {
    trim3p = true;
  }

  debug(printf("trim5p = %d, trim3p = %d\n",trim5p,trim3p));

  /* Using iter1 to avoid the possibility of an infinite loop */
  iter1 = 0;
  while (iter1 < 5 && (trim5p == true || trim3p == true)) {
    if (trim5p == false) {
      path = List_reverse(pairs);
    } else {
#ifdef WASTE
      path = trim_noncanonical_end5_exons(&trim5p,&(*ambig_end_length_5),&(*ambig_splicetype_5),
					  pairs,paired_favor_mode,zero_offset,querylength,
					  watsonp,cdna_direction,maxintronlen_bound,pairpool);
#else
      path = trim_noncanonical_end5_exons(&trim5p,&(*ambig_end_length_5),&(*ambig_splicetype_5),
					  pairs,paired_favor_mode,zero_offset,querylength,
					  watsonp,cdna_direction,maxintronlen_bound);
#endif
    }

    if (trim3p == false) {
      pairs = List_reverse(path);
    } else {
#ifdef WASTE
      pairs = trim_noncanonical_end3_exons(&trim3p,&(*ambig_end_length_3),&(*ambig_splicetype_3),
					   path,paired_favor_mode,zero_offset,querylength,
					   watsonp,cdna_direction,maxintronlen_bound,pairpool);
#else
      pairs = trim_noncanonical_end3_exons(&trim3p,&(*ambig_end_length_3),&(*ambig_splicetype_3),
					   path,paired_favor_mode,zero_offset,querylength,
					   watsonp,cdna_direction,maxintronlen_bound);
#endif
    }

    debug(printf("trim5p = %d, trim3p = %d\n",trim5p,trim3p));
    debug(Pair_dump_list(pairs,true));

    if (trim5p == true) {
      pairs = build_pairs_end5(&knownsplice5p,&(*ambig_end_length_5),&(*ambig_splicetype_5),
			       &chop_exon_p,&dynprogindex_minor,pairs,
			       chroffset,chrhigh,
			       knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			       cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			       queryaaseq_ptr,
#endif
			       queryseq_ptr,queryuc_ptr,
			       cdna_direction,watsonp,jump_late_p,
			       maxpeelback,maxpeelback_distalmedial,
			       nullgap,extramaterial_end,extraband_end,
			       *defect_rate,pairpool,dynprogR,
			       /*extendp*/true,/*endalign*/QUERYEND_INDELS);
    }

    if (trim3p == true) {
      debug(printf("Calling build_path_end3 in pass 10\n"));
      path = List_reverse(pairs);
      path = build_path_end3(&knownsplice3p,&(*ambig_end_length_3),&(*ambig_splicetype_3),
			     &chop_exon_p,&dynprogindex_minor,path,
			     chroffset,chrhigh,querylength,
			     knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			     cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			     queryaaseq_ptr,
#endif
			     queryseq_ptr,queryuc_ptr,
			     cdna_direction,watsonp,jump_late_p,
			     maxpeelback,maxpeelback_distalmedial,
			     nullgap,extramaterial_end,extraband_end,
			     *defect_rate,pairpool,dynprogL,
			     /*extendp*/true,/*endalign*/QUERYEND_INDELS);
      pairs = List_reverse(path);
    }

    /* Stop trimming at known splice sites */
    if (knownsplice5p == true) {
      trim5p = false;
    }

    if (knownsplice3p == true) {
      trim3p = false;
    }

    iter1++;
  }

  /* Need this because pass10 can set trim5p or trim3p false based on
     good medial_prob, while still trimming */

  debug(printf("Pass 11: Final extension, end5\n"));
  /* Perform final extension so we can compare fwd and rev properly */
  pairs = build_pairs_end5(&knownsplice5p,&(*ambig_end_length_5),&(*ambig_splicetype_5),
			   &chop_exon_p,&dynprogindex_minor,pairs,
			   chroffset,chrhigh,
			   knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			   cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			   queryaaseq_ptr,
#endif
			   queryseq_ptr,queryuc_ptr,
			   cdna_direction,watsonp,jump_late_p,
			   maxpeelback,maxpeelback_distalmedial,
			   nullgap,extramaterial_end,extraband_end,
			   *defect_rate,pairpool,dynprogR,
			   /*extendp*/true,/*endalign*/QUERYEND_NOGAPS);

  debug(printf("Pass 11: Final extension, end3\n"));
  path = List_reverse(pairs);
  path = build_path_end3(&knownsplice3p,&(*ambig_end_length_3),&(*ambig_splicetype_3),
			 &chop_exon_p,&dynprogindex_minor,path,
			 chroffset,chrhigh,querylength,
			 knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			 cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			 queryaaseq_ptr,
#endif
			 queryseq_ptr,queryuc_ptr,
			 cdna_direction,watsonp,jump_late_p,
			 maxpeelback,maxpeelback_distalmedial,
			 nullgap,extramaterial_end,extraband_end,
			 *defect_rate,pairpool,dynprogL,
			 /*extendp*/true,/*endalign*/QUERYEND_NOGAPS);
  pairs = List_reverse(path);

  /* *nmatches_pretrim = Pair_nmatches(pairs); */

  debug(Pair_dump_list(pairs,true));
  debug(printf("End of path_compute\n"));
  return pairs;
}


static List_T
trim_novel_spliceends (List_T pairs, 
		       int *ambig_end_length_5, int *ambig_end_length_3,
		       Splicetype_T *ambig_splicetype_5, Splicetype_T *ambig_splicetype_3,
		       int *cdna_direction, int *sensedir, bool watsonp, int querylength,
		       Univcoord_T chroffset, Univcoord_T chrhigh,
		       bool knownsplice5p, bool knownsplice3p) {
  List_T path, p;
  int i;

  Pair_T pair;
  Univcoord_T genomicpos, start_genomicpos, end_genomicpos, splice_genomepos_5, splice_genomepos_3;
  Univcoord_T start, end;
  double donor_prob, acceptor_prob, max_prob_5 = 0.0, max_prob_3 = 0.0;
  Splicetype_T splicetype5, splicetype3;
  int splice_cdna_direction_5, splice_sensedir_5, splice_cdna_direction_3, splice_sensedir_3;
  bool mismatchp;


  debug13(printf("\nEntered trim_novel_spliceends with cdna_direction %d and sensedir %d\n",
		 *cdna_direction,*sensedir));

  path = List_reverse(pairs);
  if (path != NULL && knownsplice3p == false && *ambig_end_length_3 == 0 &&
      exon_length_3(path) >= END_SPLICESITE_EXON_LENGTH) {
    /* See if there is a good splice site at the 3' end */
    debug13(Pair_dump_list(path,true));

    pair = (Pair_T) List_head(p = path);
    start = end = pair->genomepos;

    i = 0;
    mismatchp = false;
    while (i <= END_SPLICESITE_SEARCH) {
      if ((p = List_next(p)) == NULL) {
	break;
      } else {
	pair = (Pair_T) List_head(p);
	if (pair->gapp == true) {
	  break;
	} else if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
	  end = pair->genomepos;
	  i++;
	} else {
	  mismatchp = true;
	  i = 0;
	}
      }
    }


    if (mismatchp == false) {
      /* Allow perfect overhangs into intron */
      debug13(printf("Allowing perfect overhang into potential intron\n"));

    } else if (*sensedir == SENSE_FORWARD) {
      if (watsonp) {
	splicetype3 = DONOR;

	start_genomicpos = start + 1;
	end_genomicpos = end + 1;

	assert(start_genomicpos >= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos >= end_genomicpos; genomicpos--) {
	  donor_prob = Maxent_hr_donor_prob(chroffset+genomicpos,chroffset); /* Case 1 */
	  debug13(printf("3', watson, sense anti %u %f\n",genomicpos,donor_prob));
	  if (donor_prob > max_prob_3) {
	    max_prob_3 = donor_prob;
	    splice_genomepos_3 = genomicpos - 1;
	  }
	}

      } else {
	splicetype3 = ANTIDONOR;

	start_genomicpos = (chrhigh - chroffset) - start;
	end_genomicpos = (chrhigh - chroffset) - end;

	assert(start_genomicpos <= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos <= end_genomicpos; genomicpos++) {
	  donor_prob = Maxent_hr_antidonor_prob(chroffset+genomicpos,chroffset); /* Case 3 */
	  debug13(printf("3', crick, sense forward %u %f\n",genomicpos,donor_prob));
	  if (donor_prob > max_prob_3) {
	    max_prob_3 = donor_prob;
	    splice_genomepos_3 = (chrhigh - chroffset) - genomicpos;
	  }
	}
      }

    } else if (*sensedir == SENSE_ANTI) {
      if (watsonp) {
	splicetype3 = ANTIACCEPTOR;

	start_genomicpos = start + 1;
	end_genomicpos = end + 1;

	assert(start_genomicpos >= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos >= end_genomicpos; genomicpos--) {
	  acceptor_prob = Maxent_hr_antiacceptor_prob(chroffset+genomicpos,chroffset); /* Case 5 */
	  debug13(printf("3', watson, sense forward %u %f\n",genomicpos,acceptor_prob));
	  if (acceptor_prob > max_prob_3) {
	    max_prob_3 = acceptor_prob;
	    splice_genomepos_3 = genomicpos - 1;
	  }
	}

      } else {
	splicetype3 = ACCEPTOR;

	start_genomicpos = (chrhigh - chroffset) - start;
	end_genomicpos = (chrhigh - chroffset) - end;

	assert(start_genomicpos <= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos <= end_genomicpos; genomicpos++) {
	  acceptor_prob = Maxent_hr_acceptor_prob(chroffset+genomicpos,chroffset); /* Case 7 */
	  debug13(printf("3', crick, sense anti %u %f\n",genomicpos,acceptor_prob));
	  if (acceptor_prob > max_prob_3) {
	    max_prob_3 = acceptor_prob;
	    splice_genomepos_3 = (chrhigh - chroffset) - genomicpos;
	  }
	}
      }
      
    } else {
      if (watsonp) {
	start_genomicpos = start + 1;
	end_genomicpos = end + 1;

	assert(start_genomicpos >= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos >= end_genomicpos; genomicpos--) {
	  donor_prob = Maxent_hr_donor_prob(chroffset+genomicpos,chroffset); /* Case 1 */
	  acceptor_prob = Maxent_hr_antiacceptor_prob(chroffset+genomicpos,chroffset); /* Case 5 */
	  debug13(printf("3', watson, sense null %u %f %f\n",genomicpos,donor_prob,acceptor_prob));
	  if (donor_prob > max_prob_3) {
	    max_prob_3 = donor_prob;
	    splice_genomepos_3 = genomicpos - 1;
	    splice_cdna_direction_3 = +1;
	    splice_sensedir_3 = SENSE_FORWARD;
	    splicetype3 = DONOR;
	  }
	  if (acceptor_prob > max_prob_3) {
	    max_prob_3 = acceptor_prob;
	    splice_genomepos_3 = genomicpos - 1;
	    splice_cdna_direction_3 = -1;
	    splice_sensedir_3 = SENSE_ANTI;
	    splicetype3 = ANTIACCEPTOR;
	  }
	}

      } else {
	start_genomicpos = (chrhigh - chroffset) - start;
	end_genomicpos = (chrhigh - chroffset) - end;

	assert(start_genomicpos <= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos <= end_genomicpos; genomicpos++) {
	  donor_prob = Maxent_hr_antidonor_prob(chroffset+genomicpos,chroffset); /* Case 3 */
	  acceptor_prob = Maxent_hr_acceptor_prob(chroffset+genomicpos,chroffset); /* Case 7 */
	  debug13(printf("3', crick, sense null %u %f %f\n",genomicpos,donor_prob,acceptor_prob));
	  if (donor_prob > max_prob_3) {
	    max_prob_3 = donor_prob;
	    splice_genomepos_3 = (chrhigh - chroffset) - genomicpos;
	    splice_cdna_direction_3 = +1;
	    splice_sensedir_3 = SENSE_FORWARD;
	    splicetype3 = ANTIDONOR;
	  }
	  if (acceptor_prob > max_prob_3) {
	    max_prob_3 = acceptor_prob;
	    splice_genomepos_3 = (chrhigh - chroffset) - genomicpos;
	    splice_cdna_direction_3 = -1;
	    splice_sensedir_3 = SENSE_ANTI;
	    splicetype3 = ACCEPTOR;
	  }
	}
      }
    }

    if (*sensedir != SENSE_NULL) {
      if (max_prob_3 > END_SPLICESITE_PROB) {
	debug13(printf("Found good splice %s on 3' end at %u with probability %f\n",
		       Splicetype_string(splicetype3),splice_genomepos_3,max_prob_3));
	while (path != NULL && ((Pair_T) path->first)->genomepos > splice_genomepos_3) {
	  path = Pairpool_pop(path,&pair);
	}
	path = clean_path_end3(path);
	if (path != NULL) {
	  *ambig_end_length_3 = (querylength - 1) - ((Pair_T) path->first)->querypos;
	  *ambig_splicetype_3 = splicetype3;
	  debug13(printf("Set ambig_end_length_3 to be %d\n",*ambig_end_length_3));
	}
      }
    }
  }

  pairs = List_reverse(path);
  if (pairs != NULL && knownsplice5p == false && *ambig_end_length_5 == 0 &&
      exon_length_5(pairs) >= END_SPLICESITE_EXON_LENGTH) {
    /* See if there is a good splice site at the 5' end */
    debug13(Pair_dump_list(pairs,true));

    pair = (Pair_T) List_head(p = pairs);
    start = end = pair->genomepos;

    i = 0;
    mismatchp = false;
    while (i <= END_SPLICESITE_SEARCH) {
      if ((p = List_next(p)) == NULL) {
	break;
      } else {
	pair = (Pair_T) List_head(p);
	if (pair->gapp == true) {
	  break;
	} else if (pair->comp == MATCH_COMP || pair->comp == DYNPROG_MATCH_COMP || pair->comp == AMBIGUOUS_COMP) {
	  end = pair->genomepos;
	  i++;
	} else {
	  mismatchp = true;
	  i = 0;
	}
      }
    }

    if (mismatchp == false) {
      /* Allow perfect overhangs into intron */
      debug13(printf("Allowing perfect overhang into potential intron\n"));

    } else if (*sensedir == SENSE_FORWARD) {
      if (watsonp) {
	splicetype5 = ACCEPTOR;

	start_genomicpos = start;
	end_genomicpos = end;

	assert(start_genomicpos <= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos <= end_genomicpos; genomicpos++) {
	  acceptor_prob = Maxent_hr_acceptor_prob(chroffset+genomicpos,chroffset); /* Case 2 */
	  debug13(printf("5', watson, sense forward %u %f\n",genomicpos,acceptor_prob));
	  if (acceptor_prob > max_prob_5) {
	    max_prob_5 = acceptor_prob;
	    splice_genomepos_5 = genomicpos;
	  }
	}

      } else {
	splicetype5 = ANTIACCEPTOR;

	start_genomicpos = (chrhigh - chroffset) - start + 1;
	end_genomicpos = (chrhigh - chroffset) - end + 1;

	assert(start_genomicpos >= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos >= end_genomicpos; genomicpos--) {
	  acceptor_prob = Maxent_hr_antiacceptor_prob(chroffset+genomicpos,chroffset); /* Case 4 */
	  debug13(printf("5', crick, sense anti %u %f\n",genomicpos,acceptor_prob));
	  if (acceptor_prob > max_prob_5) {
	    max_prob_5 = acceptor_prob;
	    splice_genomepos_5 = (chrhigh - chroffset) - genomicpos + 1;
	  }
	}
      }

    } else if (*sensedir == SENSE_ANTI) {
      if (watsonp) {
	splicetype5 = ANTIDONOR;

	start_genomicpos = start;
	end_genomicpos = end;
	
	assert(start_genomicpos <= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos <= end_genomicpos; genomicpos++) {
	  donor_prob = Maxent_hr_antidonor_prob(chroffset+genomicpos,chroffset); /* Case 6 */
	  debug13(printf("5', watson, sense anti %u %f\n",genomicpos,donor_prob));
	  if (donor_prob > max_prob_5) {
	    max_prob_5 = donor_prob;
	    splice_genomepos_5 = genomicpos;
	  }
	}

      } else {
	splicetype5 = DONOR;

	start_genomicpos = (chrhigh - chroffset) - start + 1;
	end_genomicpos = (chrhigh - chroffset) - end + 1;

	assert(start_genomicpos >= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos >= end_genomicpos; genomicpos--) {
	  donor_prob = Maxent_hr_donor_prob(chroffset+genomicpos,chroffset); /* Case 8 */
	  debug13(printf("5', crick, sense forward %u %f\n",genomicpos,donor_prob));
	  if (donor_prob > max_prob_5) {
	    max_prob_5 = donor_prob;
	    splice_genomepos_5 = (chrhigh - chroffset) - genomicpos + 1;
	  }
	}
      }
      
    } else {
      if (watsonp) {
	start_genomicpos = start;
	end_genomicpos = end;

	assert(start_genomicpos <= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos <= end_genomicpos; genomicpos++) {
	  acceptor_prob = Maxent_hr_acceptor_prob(chroffset+genomicpos,chroffset); /* Case 2 */
	  donor_prob = Maxent_hr_antidonor_prob(chroffset+genomicpos,chroffset); /* Case 6 */
	  debug13(printf("5', watson, sense null %u %f %f\n",genomicpos,donor_prob,acceptor_prob));
	  if (acceptor_prob > max_prob_5) {
	    max_prob_5 = acceptor_prob;
	    splice_genomepos_5 = genomicpos;
	    splice_cdna_direction_5 = +1;
	    splice_sensedir_5 = SENSE_FORWARD;
	    splicetype5 = ACCEPTOR;
	  }
	  if (donor_prob > max_prob_5) {
	    max_prob_5 = donor_prob;
	    splice_genomepos_5 = genomicpos;
	    splice_cdna_direction_5 = -1;
	    splice_sensedir_5 = SENSE_ANTI;
	    splicetype5 = ANTIDONOR;
	  }
	}

      } else {
	start_genomicpos = (chrhigh - chroffset) - start + 1;
	end_genomicpos = (chrhigh - chroffset) - end + 1;

	assert(start_genomicpos >= end_genomicpos);
	for (genomicpos = start_genomicpos; genomicpos >= end_genomicpos; genomicpos--) {
	  acceptor_prob = Maxent_hr_antiacceptor_prob(chroffset+genomicpos,chroffset); /* Case 4 */
	  donor_prob = Maxent_hr_donor_prob(chroffset+genomicpos,chroffset); /* Case 8 */
	  debug13(printf("5', crick, sense null %u %f %f\n",genomicpos,donor_prob,acceptor_prob));
	  if (acceptor_prob > max_prob_5) {
	    max_prob_5 = acceptor_prob;
	    splice_genomepos_5 = (chrhigh - chroffset) - genomicpos + 1;
	    splice_cdna_direction_5 = +1;
	    splice_sensedir_5 = SENSE_FORWARD;
	    splicetype5 = ANTIACCEPTOR;
	  }
	  if (donor_prob > max_prob_5) {
	    max_prob_5 = donor_prob;
	    splice_genomepos_5 = (chrhigh - chroffset) - genomicpos + 1;
	    splice_cdna_direction_5 = -1;
	    splice_sensedir_5 = SENSE_ANTI;
	    splicetype5 = DONOR;
	  }
	}
      }
    }

    if (*sensedir != SENSE_NULL) {
      if (max_prob_5 > END_SPLICESITE_PROB) {
	debug13(printf("Found good splice %s on 5' end at %u with probability %f\n",
		       Splicetype_string(splicetype5),splice_genomepos_5,max_prob_5));
	while (pairs != NULL && ((Pair_T) pairs->first)->genomepos < splice_genomepos_5) {
	  pairs = Pairpool_pop(pairs,&pair);
	}
	pairs = clean_pairs_end5(pairs);
	if (pairs != NULL) {
	  *ambig_end_length_5 = ((Pair_T) pairs->first)->querypos;
	  *ambig_splicetype_5 = splicetype5;
	  debug13(printf("Set ambig_end_length_5 to be %d\n",*ambig_end_length_5));
	}
      }
    }
  }

  if (*sensedir == SENSE_NULL) {
    if (max_prob_3 > max_prob_5) {
      if (max_prob_3 > END_SPLICESITE_PROB) {
	debug13(printf("Found good splice %s on 3' end at %u with probability %f\n",
		       Splicetype_string(splicetype3),splice_genomepos_3,max_prob_3));
	path = List_reverse(pairs);
	while (path != NULL && ((Pair_T) path->first)->genomepos > splice_genomepos_3) {
	  path = Pairpool_pop(path,&pair);
	}
	path = clean_path_end3(path);
	if (path != NULL) {
	  *ambig_end_length_3 = (querylength - 1) - ((Pair_T) path->first)->querypos;
	  *ambig_splicetype_3 = splicetype3;
	  *cdna_direction = splice_cdna_direction_3;
	  *sensedir = splice_sensedir_3;
	  debug13(printf("Set ambig_end_length_3 to be %d\n",*ambig_end_length_3));
	}
	pairs = List_reverse(path);
      }
    } else {
      if (max_prob_5 > END_SPLICESITE_PROB) {
	debug13(printf("Found good splice %s on 5' end at %u with probability %f\n",
		       Splicetype_string(splicetype5),splice_genomepos_5,max_prob_5));
	while (pairs != NULL && ((Pair_T) pairs->first)->genomepos < splice_genomepos_5) {
	  pairs = Pairpool_pop(pairs,&pair);
	}
	pairs = clean_pairs_end5(pairs);
	if (pairs != NULL) {
	  *ambig_end_length_5 = ((Pair_T) pairs->first)->querypos;
	  *ambig_splicetype_5 = splicetype5;
	  *cdna_direction = splice_cdna_direction_5;
	  *sensedir = splice_sensedir_5;
	  debug13(printf("Set ambig_end_length_5 to be %d\n",*ambig_end_length_5));
	}
      }
    }
  }

  return pairs;
}



static List_T
path_trim (double defect_rate, int *ambig_end_length_5, int *ambig_end_length_3,
	   Splicetype_T *ambig_splicetype_5, Splicetype_T *ambig_splicetype_3,
	   List_T pairs, int *cdna_direction, int *sensedir, bool watsonp, bool jump_late_p,
	   int querylength,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
	   int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
	   char *queryaaseq_ptr,
#endif
	   char *queryseq_ptr, char *queryuc_ptr,
	   Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
	   Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
	   int maxpeelback, int maxpeelback_distalmedial, int nullgap,
	   int extramaterial_end, int extraband_end,
	   Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogR, Stage3debug_T stage3debug) {
  List_T path = NULL;
  int dynprogindex_minor = DYNPROGINDEX_MINOR;
  int maxpeelback5, maxpeelback3;
  bool chop_exon_p;
  bool knownsplice5p = false, knownsplice3p = false;
  bool trim5p, trim3p;

  maxpeelback5 = maxpeelback3 = maxpeelback;


  debug3(printf("Entering path_trim with cdna_direction %d and sensedir %d\n",*cdna_direction,*sensedir));

#if 0
  /* Remove known splices here */
  path = insert_gapholders(pairs,queryseq_ptr,queryuc_ptr,chroffset,chrhigh,watsonp,pairpool);
  pairs = assign_gap_types(path,*cdna_direction,watsonp,queryseq_ptr,
			   chrnum,chroffset,chrhigh,pairpool);
#endif

#if 0
  /* Now doing this at the end of path_compute */

  debug3(printf("Before full extension:\n"));
  debug3(Pair_dump_list(pairs,true));

  /* Final: Full extension: path -> pairs */
  pairs = clean_pairs_end5_gap_indels(pairs);
  pairs = build_pairs_end5(&knownsplice5p,&(*ambig_end_length_5),&(*ambig_splicetype_5),
			   &chop_exon_p,&dynprogindex_minor,pairs,
			   chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			   cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			   queryaaseq_ptr,
#endif
			   queryseq_ptr,queryuc_ptr,
			   *cdna_direction,watsonp,jump_late_p,
			   maxpeelback5,maxpeelback_distalmedial,
			   nullgap,extramaterial_end,extraband_end,
			   defect_rate,pairpool,dynprogR,
			   /*extendp*/true,/*endalign -- was QUERYEND_INDELS*/QUERYEND_NOGAPS);

  path = List_reverse(pairs);
  path = clean_path_end3_gap_indels(path);
  
  debug(printf("Calling build_path_end3 in trim final\n"));
  path = build_path_end3(&knownsplice3p,&(*ambig_end_length_3),&(*ambig_splicetype_3),
			 &chop_exon_p,&dynprogindex_minor,path,
			 chroffset,chrhigh,querylength,
			 knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			 cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			 queryaaseq_ptr,
#endif
			 queryseq_ptr,queryuc_ptr,
			 *cdna_direction,watsonp,jump_late_p,
			 maxpeelback3,maxpeelback_distalmedial,
			 nullgap,extramaterial_end,extraband_end,
			 defect_rate,pairpool,dynprogL,
			 /*extendp*/true,/*endalign -- was QUERYEND_INDELS*/QUERYEND_NOGAPS);

  pairs = List_reverse(path);

  debug3(printf("After full extension and before trim ends:\n"));
  debug3(Pair_dump_list(pairs,true));
#endif


  debug13(printf("Before trim_novel_spliceends\n"));
  debug13(Pair_dump_list(pairs,true));

#ifdef GSNAP
  if (novelsplicingp == true) {
    pairs = trim_novel_spliceends(pairs,&(*ambig_end_length_5),&(*ambig_end_length_3),
				  &(*ambig_splicetype_5),&(*ambig_splicetype_3),
				  &(*cdna_direction),&(*sensedir),watsonp,querylength,
				  chroffset,chrhigh,knownsplice5p,knownsplice3p);
  }
#endif
  debug13(printf("After trim_novel_spliceends\n"));
  debug13(Pair_dump_list(pairs,true));


  if (pairs == NULL) {
    return (List_T) NULL;
  } else {
    knownsplice5p = knownsplice3p = false;
    pairs = Pair_trim_ends(&trim5p,&trim3p,pairs);
    
    debug3(printf("After Pair_trim_ends: trim5p %d, trim3p %d\n",trim5p,trim3p));
    debug3(Pair_dump_list(pairs,true));
    debug3(printf("\n"));

    /* Extend with BEST_LOCAL to get right local (not global) answer,
       and with maxpeelback == 0 to ensure we perform no peelback */
    if (trim5p == true) {
      debug3(printf("Extending at 5'\n"));
      pairs = build_pairs_end5(&knownsplice5p,&(*ambig_end_length_5),&(*ambig_splicetype_5),
			       &chop_exon_p,&dynprogindex_minor,pairs,
			       chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			       cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			       queryaaseq_ptr,
#endif
			       queryseq_ptr,queryuc_ptr,
			       *cdna_direction,watsonp,jump_late_p,
			       /*maxpeelback*/0,/*maxpeelback_distalmedial*/0,
			       nullgap,extramaterial_end,extraband_end,
			       defect_rate,pairpool,dynprogR,
			       /*extendp*/true,/*endalign*/BEST_LOCAL);
    }
    if (trim3p == true) {
      debug3(printf("Extending at 3'\n"));
      path = List_reverse(pairs);
      path = build_path_end3(&knownsplice3p,&(*ambig_end_length_3),&(*ambig_splicetype_3),
			     &chop_exon_p,&dynprogindex_minor,path,
			     chroffset,chrhigh,querylength,
			     knownsplice_limit_low,knownsplice_limit_high,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			     cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			     queryaaseq_ptr,
#endif
			     queryseq_ptr,queryuc_ptr,
			     *cdna_direction,watsonp,jump_late_p,
			     /*maxpeelback*/0,/*maxpeelback_distalmedial*/0,
			     nullgap,extramaterial_end,extraband_end,
			     defect_rate,pairpool,dynprogL,
			     /*extendp*/true,/*endalign*/BEST_LOCAL);
      pairs = List_reverse(path);
    }

    debug3(printf("After trim ends:\n"));
    debug3(Pair_dump_list(pairs,true));
  }


  debug3(printf("Final result of path_trim: chroffset = %u, cdna_direction %d, sensedir %d\n",
		chroffset,*cdna_direction,*sensedir));
  debug3(Pair_dump_list(pairs,true));
  debug3(printf("\n"));

  return pairs;
}


struct Pair_T *
Stage3_compute (List_T *pairs, int *npairs, int *cdna_direction, int *sensedir, int *matches,
		int *nmatches_posttrim, int *max_match_length,
		int *ambig_end_length_5, int *ambig_end_length_3,
		Splicetype_T *ambig_splicetype_5, Splicetype_T *ambig_splicetype_3,
		int *unknowns, int *mismatches, int *qopens, int *qindels, int *topens, int *tindels,
		int *ncanonical, int *nsemicanonical, int *nnoncanonical, double *min_splice_prob,
		double *defect_rate, List_T path,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		int cutoff_level, char *queryptr, Compress_T query_compress,
#endif
#endif
#ifdef PMAP
		char *queryaaseq_ptr, char *genomicuc_ptr,
#endif
		char *queryseq_ptr, char *queryuc_ptr, int querylength,
		int skiplength, int query_subseq_offset,
		Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
		Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
		bool watsonp, int genestrand, bool jump_late_p,
		int maxpeelback, int maxpeelback_distalmedial, int nullgap,
		int extramaterial_end, int extramaterial_paired,
		int extraband_single, int extraband_end, int extraband_paired, int minendexon,
		Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		int ngap, Stage3debug_T stage3debug, bool diagnosticp, bool checkp,
		bool do_final_p, int sense_try, int sense_filter,
		Oligoindex_T *oligoindices_minor, int noligoindices_minor, Diagpool_T diagpool,
		int sufflookback, int nsufflookback, int maxintronlen, int close_indels_mode,
		int paired_favor_mode, int zero_offset) {
  struct Pair_T *pairarray;
  double fwd_defect_rate, rev_defect_rate;
  List_T pairs_pretrim, pairs_fwd, pairs_rev, path_fwd, path_rev;
  int ncanonical_fwd, nsemicanonical_fwd, nnoncanonical_fwd,
    ncanonical_rev, nsemicanonical_rev, nnoncanonical_rev;
  int nbadintrons_fwd, nbadintrons_rev;
  double max_intron_score_fwd = 0.0, max_intron_score_rev = 0.0,
    avg_donor_score_fwd = 0.0, avg_acceptor_score_fwd = 0.0,
    avg_donor_score_rev = 0.0, avg_acceptor_score_rev = 0.0;
  int alignment_score_fwd, alignment_score_rev;
  int nmatches_fwd, nmismatches_fwd, nmatches_rev, nmismatches_rev, nindels_fwd, nindels_rev;
  int fwd_ambig_end_length_5, fwd_ambig_end_length_3, rev_ambig_end_length_5, rev_ambig_end_length_3;
  Splicetype_T fwd_ambig_splicetype_5, fwd_ambig_splicetype_3, rev_ambig_splicetype_5, rev_ambig_splicetype_3;

#ifdef COMPLEX_DIRECTION
  int indel_alignment_score_fwd, indel_alignment_score_rev;
#endif


  debug(printf("Stage 3: *** Starting stage 3 at chrnum #%d)\n",chrnum));

#ifdef PMAP
  path_fwd = path;
  path_rev = (List_T) NULL;
  pairs_rev = (List_T) NULL;
  /* do_final_p = true; */
#else
  if (splicingp == false) {
    path_fwd = path;
    path_rev = (List_T) NULL;
  } else if (sense_try == 0) {
    /* Should try both even if no introns (cf, AA011563) */
    path_fwd = path;
    path_rev = Pairpool_copy(path,pairpool);
  } else if (sense_try > 0) {
    path_fwd = path;
    path_rev = (List_T) NULL;
  } else if (sense_try < 0) {
    path_fwd = (List_T) NULL;
    path_rev = path;
  }
#endif


  if (path_fwd != NULL) {
    pairs_fwd = path_compute(&fwd_defect_rate,&fwd_ambig_end_length_5,&fwd_ambig_end_length_3,
			     &fwd_ambig_splicetype_5,&fwd_ambig_splicetype_3,
			     path_fwd,/*cdna_direction*/+1,watsonp,genestrand,
			     jump_late_p,querylength,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			     cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
			     queryaaseq_ptr,genomicuc_ptr,
#endif
			     queryseq_ptr,queryuc_ptr,chrnum,chroffset,chrhigh,
			     knownsplice_limit_low,knownsplice_limit_high,
			     maxpeelback,maxpeelback_distalmedial,nullgap,
			     extramaterial_end,extraband_end,
			     extramaterial_paired,extraband_single,extraband_paired,
			     pairpool,dynprogL,dynprogM,dynprogR,stage3debug,do_final_p,
			     oligoindices_minor,noligoindices_minor,diagpool,
			     sufflookback,nsufflookback,maxintronlen,close_indels_mode,
			     paired_favor_mode,zero_offset);
    
    if (splicingp) {
      path_fwd = List_reverse(pairs_fwd);
      pairs_fwd = score_introns(&max_intron_score_fwd,&avg_donor_score_fwd,&avg_acceptor_score_fwd,&nbadintrons_fwd,
				path_fwd,/*cdna_direction*/+1,watsonp,
				chrnum,chroffset,chrhigh,
#ifdef WASTE
				pairpool,
#endif
				nullgap);
      alignment_score_fwd = score_alignment(&nmatches_fwd,&nmismatches_fwd,&nindels_fwd,
#ifdef COMPLEX_DIRECTION
					    &indel_alignment_score_fwd,
#endif
					    &ncanonical_fwd,&nsemicanonical_fwd,&nnoncanonical_fwd,
					    pairs_fwd,/*cdna_direction*/+1);
    }

  } else {
    pairs_fwd = NULL;
  }


#ifndef PMAP
  if (path_rev != NULL) {
    pairs_rev = path_compute(&rev_defect_rate,&rev_ambig_end_length_5,&rev_ambig_end_length_3,
			     &rev_ambig_splicetype_5,&rev_ambig_splicetype_3,
			     path_rev,/*cdna_direction*/-1,watsonp,genestrand,
			     jump_late_p,querylength,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
			     cutoff_level,queryptr,query_compress,
#endif
#endif
			     queryseq_ptr,queryuc_ptr,chrnum,chroffset,chrhigh,
			     knownsplice_limit_low,knownsplice_limit_high,
			     maxpeelback,maxpeelback_distalmedial,nullgap,
			     extramaterial_end,extraband_end,
			     extramaterial_paired,extraband_single,extraband_paired,
			     pairpool,dynprogL,dynprogM,dynprogR,stage3debug,do_final_p,
			     oligoindices_minor,noligoindices_minor,diagpool,
			     sufflookback,nsufflookback,maxintronlen,close_indels_mode,
			     paired_favor_mode,zero_offset);


    path_rev = List_reverse(pairs_rev);
    pairs_rev = score_introns(&max_intron_score_rev,&avg_donor_score_rev,&avg_acceptor_score_rev,&nbadintrons_rev,
			      path_rev,/*cdna_direction*/-1,watsonp,
			      chrnum,chroffset,chrhigh,
#ifdef WASTE
			      pairpool,
#endif			      
			      nullgap);
    alignment_score_rev = score_alignment(&nmatches_rev,&nmismatches_rev,&nindels_rev,
#ifdef COMPLEX_DIRECTION
					  &indel_alignment_score_rev,
#endif
					  &ncanonical_rev,&nsemicanonical_rev,&nnoncanonical_rev,
					  pairs_rev,/*cdna_direction*/-1);


  } else {
    pairs_rev = NULL;
  }
#endif

  debug(printf("Forward:\n"));
  debug(Pair_dump_list(pairs_fwd,true));
  debug(printf("\n"));

  debug(printf("Reverse:\n"));
  debug(Pair_dump_list(pairs_rev,true));
  debug(printf("\n"));

  if (diagnosticp == true) {
    if (pairs_fwd != NULL) {
      path_fwd = check_gaps(pairs_fwd,pairpool);
      pairs_fwd = List_reverse(path_fwd);
    }
    if (pairs_rev != NULL) {
      path_rev = check_gaps(pairs_rev,pairpool);
      pairs_rev = List_reverse(path_rev);
    }
  }


  debug(printf("Intronscores: %f,%f fwd, %f,%f rev\n",
	       avg_donor_score_fwd,avg_acceptor_score_fwd,avg_donor_score_rev,avg_acceptor_score_rev));
  if (splicingp == false) {
    pairs_pretrim = pairs_fwd;
    *defect_rate = fwd_defect_rate;
    *cdna_direction = +1;
    *sensedir = SENSE_NULL;
  } else {
    pairs_pretrim = pick_cdna_direction(&(*cdna_direction),&(*sensedir),pairs_fwd,pairs_rev,
					ncanonical_fwd,nsemicanonical_fwd,nnoncanonical_fwd,nbadintrons_fwd,
					ncanonical_rev,nsemicanonical_rev,nnoncanonical_rev,nbadintrons_rev,
					max_intron_score_fwd,avg_donor_score_fwd,avg_acceptor_score_fwd,
					max_intron_score_rev,avg_donor_score_rev,avg_acceptor_score_rev,
#ifdef COMPLEX_DIRECTION
					nmatches_fwd,nmismatches_fwd,nmatches_rev,nmismatches_rev,nindels_fwd,nindels_rev,
					indel_alignment_score_fwd,indel_alignment_score_rev,
#endif
					alignment_score_fwd,alignment_score_rev,sense_filter);
  }

  if (pairs_pretrim == NULL) {
    *npairs = 0;
    *defect_rate = 0.0;
    *nmatches_posttrim = 0;
    *ambig_end_length_5 = *ambig_end_length_3 = 0;
    return (struct Pair_T *) NULL;
  } else {
    if (*cdna_direction >= 0) {
      *defect_rate = fwd_defect_rate;
      *ambig_end_length_5 = fwd_ambig_end_length_5;
      *ambig_end_length_3 = fwd_ambig_end_length_3;
      *ambig_splicetype_5 = fwd_ambig_splicetype_5;
      *ambig_splicetype_3 = fwd_ambig_splicetype_3;
    } else {
      *defect_rate = rev_defect_rate;
      *ambig_end_length_5 = rev_ambig_end_length_5;
      *ambig_end_length_3 = rev_ambig_end_length_3;
      *ambig_splicetype_5 = rev_ambig_splicetype_5;
      *ambig_splicetype_3 = rev_ambig_splicetype_3;
    }


    *pairs = path_trim(*defect_rate,&(*ambig_end_length_5),&(*ambig_end_length_3),
		       &(*ambig_splicetype_5),&(*ambig_splicetype_3),
		       pairs_pretrim,&(*cdna_direction),&(*sensedir),watsonp,
		       jump_late_p,querylength,
#ifdef GSNAP
#ifdef END_KNOWNSPLICING_SHORTCUT
		       cutoff_level,queryptr,query_compress,
#endif
#endif
#ifdef PMAP
		       queryaaseq_ptr,
#endif
		       queryseq_ptr,queryuc_ptr,
		       chrnum,chroffset,chrhigh,knownsplice_limit_low,knownsplice_limit_high,
		       maxpeelback,maxpeelback_distalmedial,nullgap,
		       extramaterial_end,extraband_end,
		       pairpool,dynprogL,dynprogR,stage3debug);

    *nmatches_posttrim = Pair_nmatches_posttrim(&(*max_match_length),*pairs,/*pos5*/*ambig_end_length_5,
						/*pos3*/querylength - (*ambig_end_length_3));

    /* printf("ambig_end_length = %d, %d\n",*ambig_end_length_5,*ambig_end_length_3); */

    Pair_fracidentity(&(*matches),&(*unknowns),&(*mismatches),
		      &(*qopens),&(*qindels),&(*topens),&(*tindels),
		      &(*ncanonical),&(*nsemicanonical),&(*nnoncanonical),
		      &(*min_splice_prob),*pairs,*cdna_direction);

    pairarray = make_pairarray(&(*npairs),&(*pairs),*cdna_direction,*sensedir,watsonp,
			       pairpool,queryseq_ptr,
#ifdef PMAP
			       queryaaseq_ptr,
#endif
			       chroffset,chrhigh,
			       ngap,query_subseq_offset,skiplength,diagnosticp);

    if (checkp == true && stage3debug == NO_STAGE3DEBUG && 
	Pair_check_array(pairarray,*npairs) == true) {
      Pair_dump_array(pairarray,*npairs,/*zerobasedp*/true);
#ifndef DEBUG
      Except_raise(&coordinate_error,__FILE__,__LINE__);
#endif
    }
#ifdef DEBUG
    Pair_dump_array(pairarray,*npairs,/*zerobasedp*/true);
#endif

    return pairarray;
  }
}


/************************************************************************
 *  Merging
 ************************************************************************/

bool
Stage3_mergeable (bool *singlep, bool *dualbreakp, int *cdna_direction,
		  int *queryjump, int *genomejump,
		  Stage3_T firstpart, Stage3_T secondpart,
		  int breakpoint, int queryntlength,
		  double donor_prob, double acceptor_prob) {
  Pair_T end1, start2;
  bool watsonp, connectablep = false;
  Chrpos_T endchrpos1, startchrpos2;
  int npairs_left, npairs_right, nstart;
  int jumpdiff;
  int cdna_direction_1, cdna_direction_2;

  assert(firstpart->pairs != NULL);
  assert(secondpart->pairs != NULL);

  debug10(printf("Stage3_mergeable called with watsonp %d and %d and cdna_directions %d and %d\n",
		 firstpart->watsonp,secondpart->watsonp,firstpart->cdna_direction,secondpart->cdna_direction));
  debug10(Stage3_print_ends(firstpart));
  debug10(Stage3_print_ends(secondpart));

  if (firstpart->chrnum != secondpart->chrnum) {
    debug10(printf("not mergeable: chrnum %d != chrnum %d\n",firstpart->chrnum,secondpart->chrnum));
    return false;

  } else if (firstpart->watsonp != secondpart->watsonp) {
    debug10(printf("not mergeable: watsonp %d != watsonp %d\n",firstpart->watsonp,secondpart->watsonp));
    return false;

#if 0
  } else if (firstpart->sensedir != secondpart->sensedir &&
	     firstpart->sensedir != SENSE_NULL && secondpart->sensedir != SENSE_NULL) {
    /* Could be mergeable if an intron is trimmed during the merge */
    debug10(printf("not mergeable: sensedir %d != sensedir %d\n",
		   firstpart->sensedir,secondpart->sensedir));
    return false;
#endif

  } else {
    /* Determine cdna directions in the two halves */
    end1 = Pair_end_bound(&cdna_direction_1,firstpart->pairs,breakpoint);
    start2 = Pair_start_bound(&cdna_direction_2,secondpart->pairs,breakpoint+1);
    assert(end1 != NULL);
    assert(start2 != NULL);

    if (cdna_direction_1 != cdna_direction_2 &&
	cdna_direction_1 != 0 && cdna_direction_2 != 0) {
      debug10(printf("not mergeable: cdna_direction %d != cdna_direction %d\n",
		     cdna_direction_1,cdna_direction_2));
      return false;

    } else {
      if (cdna_direction_1 == 0) {
	*cdna_direction = cdna_direction_2;
      } else if (cdna_direction_2 == 0) {
	*cdna_direction = cdna_direction_1;
      } else {
	*cdna_direction = cdna_direction_1;
      }
      debug10(printf("cdna_direction is %d\n",*cdna_direction));
    }

    if ((watsonp = firstpart->watsonp) == true) {
      endchrpos1 = end1->genomepos;
      startchrpos2 = start2->genomepos;

      debug10(printf("? connectable, watson: endchrpos1 %d at querypos %d versus startchrpos2 %d and querypos %d\n",
		     endchrpos1,end1->querypos,startchrpos2,start2->querypos));
      if (endchrpos1 < startchrpos2) {
	/* Deletion */
	*genomejump = startchrpos2 - endchrpos1 - 1;
	debug10(printf("endchrpos1 < startchrpos2, so deletion of length %u\n",*genomejump));
	debug10(printf("donor_prob %f, acceptor_prob %f\n",donor_prob,acceptor_prob));
	if (startchrpos2 < endchrpos1 + MERGELENGTH) {
	  connectablep = true;
	} else if (/* donor_prob > DONOR_THRESHOLD && acceptor_prob >= ACCEPTOR_THRESHOLD && */
		   startchrpos2 < endchrpos1 + LONG_MERGELENGTH) {
	  connectablep = true;
	}

      } else if (startchrpos2 + (end1->querypos - start2->querypos) + 20 >= endchrpos1) {
	/* Insertion */
	debug10(printf("startchrpos2 + (%d - %d) + %d >= endchrpos2, so insertion\n",end1->querypos,start2->querypos,20));
	*genomejump = 0;
	connectablep = true;
      }


    } else {
      /* These are genomicpos, not really chrpos.  If go to chrpos, need to rewrite logic. */
      endchrpos1 = firstpart->chrhigh - end1->genomepos;
      startchrpos2 = secondpart->chrhigh - start2->genomepos;

      debug10(printf("? connectable, crick: startchrpos2 %u versus endchrpos1 %u\n",startchrpos2,endchrpos1));
      if (startchrpos2 < endchrpos1) {
	/* Deletion */
	*genomejump = endchrpos1 - startchrpos2 - 1;
	debug10(printf("startchrpos2 < endchrpos1, so deletion of length %u\n",*genomejump));
	debug10(printf("donor_prob %f, acceptor_prob %f\n",donor_prob,acceptor_prob));
	if (endchrpos1 < startchrpos2 + MERGELENGTH) {
	  connectablep = true;
	} else if (/* donor_prob > DONOR_THRESHOLD && acceptor_prob >= ACCEPTOR_THRESHOLD && */
		   endchrpos1 < startchrpos2 + LONG_MERGELENGTH) {
	  connectablep = true;
	}
      } else if (endchrpos1 + (end1->querypos - start2->querypos) + 20 >= startchrpos2) {
	/* Insertion */
	debug10(printf("endchrpos1 + (%d - %d) + %d >= endchrpos1, so insertion\n",end1->querypos,start2->querypos,20));
	*genomejump = 0;
	connectablep = true;
      }
    }

    if (connectablep == false) {
      debug10(printf("result: not mergeable\n\n"));
      return false;
    } else {
      npairs_left = Pairpool_count_bounded(&nstart,firstpart->pairs,0,breakpoint);
      npairs_right = Pairpool_count_bounded(&nstart,secondpart->pairs,breakpoint,queryntlength);
      debug10(printf("Predicted after splicing: npairs_left %d, npairs_right %d\n",npairs_left,npairs_right));
      if (npairs_left == 0 || npairs_right == 0) {
	return false;
      }

      *queryjump = start2->querypos - end1->querypos - 1;
      debug10(printf("result: mergeable: queryjump = %d - %d - 1 = %d, genomejump = %d\n\n",
		     start2->querypos,end1->querypos,*queryjump,*genomejump));

      if (*queryjump > *genomejump) {
	jumpdiff = *queryjump - *genomejump;
      } else {
	jumpdiff = *genomejump - *queryjump;
      }

      /* Follows logic of assign_gap_types somewhat */
      *singlep = *dualbreakp = false;
      if (jumpdiff < MININTRONLEN) {
	debug10(printf("  Gap is a single gap\n"));
	*singlep = true;
      } else if (*queryjump > 9) {
	*dualbreakp = true;
      }

      return true;
    }
  }
}


bool
Stage3_merge_chimera (T this_left, T this_right,
		      int minpos1, int maxpos1, int minpos2, int maxpos2,
#ifdef PMAP
		      char *queryaaseq_ptr,
#endif
		      Sequence_T queryseq, Sequence_T queryuc, Pairpool_T pairpool, 
		      Dynprog_T dynprogL, Dynprog_T dynprogR,
		      int maxpeelback, int maxpeelback_distalmedial,
		      int nullgap, int extramaterial_end, int extraband_end, int ngap) {
  List_T path;
  bool knownsplicep, chop_exon_p;
  int ambig_end_length_5 = 0, ambig_end_length_3 = 0;	/* Need to be set for build_pairs_end5 and build_path_end3 */
  int dynprogindex_minor = 0;
  Splicetype_T ambig_splicetype;
  char *queryseq_ptr, *queryuc_ptr;


  this_left->pairs = Pairpool_clip_bounded(this_left->pairs,minpos1,maxpos1);
  this_right->pairs = Pairpool_clip_bounded(this_right->pairs,minpos2,maxpos2);

  if (this_left->pairs == NULL && this_right->pairs == NULL) {
    Stage3_free_pairarray(&this_left);
    Stage3_free_pairarray(&this_right);
    this_left->pairarray = (struct Pair_T *) NULL;
    this_right->pairarray = (struct Pair_T *) NULL;
    this_left->pairarray_freeable_p = false;
    this_right->pairarray_freeable_p = false;
    return false;

  } else {
    queryseq_ptr = Sequence_fullpointer(queryseq);
    queryuc_ptr = Sequence_fullpointer(queryuc);

    path = List_reverse(this_left->pairs);

    /* Chimera might not have a single breakpoint */
    /* path = clean_path_end3_gap_indels (path); */

    path = build_path_end3(&knownsplicep,&ambig_end_length_3,&ambig_splicetype,
			   &chop_exon_p,&dynprogindex_minor,path,
			   this_left->chroffset,this_left->chrhigh,/*querylength*/maxpos1+1,
			   /*knownsplice_limit_low*/-1U,/*knownsplice_limit_high*/0,
#ifdef PMAP
			   queryaaseq_ptr,
#endif
			   queryseq_ptr,queryuc_ptr,
			   this_left->cdna_direction,this_left->watsonp,
			   /*jump_late_p*/this_left->watsonp ? false : true,
			   maxpeelback,maxpeelback_distalmedial,
			   nullgap,extramaterial_end,extraband_end,
			   /*defect_rate*/0.0,pairpool,dynprogL,
			   /*extendp*/true,/*endalign*/BEST_LOCAL);

    this_left->pairs = List_reverse(path);
    this_left->pairs = Pairpool_clip_bounded(this_left->pairs,minpos1,maxpos1);


    /* Chimera might not have a single breakpoint */
    /* this_right->pairs = clean_pairs_end5_gap_indels(this_right->pairs); */

    this_right->pairs = build_pairs_end5(&knownsplicep,&ambig_end_length_5,&ambig_splicetype,
					 &chop_exon_p,&dynprogindex_minor,this_right->pairs,
					 this_right->chroffset,this_right->chrhigh,
					 /*knownsplice_limit_low*/-1U,/*knownsplice_limit_high*/0,
#ifdef PMAP
					 queryaaseq_ptr,
#endif
					 queryseq_ptr,queryuc_ptr,
					 this_right->cdna_direction,this_right->watsonp,
					 /*jump_late_p*/this_right->watsonp ? false : true,
					 maxpeelback,maxpeelback_distalmedial,
					 nullgap,extramaterial_end,extraband_end,
					 /*defect_rate*/0.0,pairpool,dynprogR,
					 /*extendp*/true,/*endalign*/BEST_LOCAL);
    this_right->pairs = Pairpool_clip_bounded(this_right->pairs,minpos2,maxpos2);

    if (this_left->pairs == NULL || this_right->pairs == NULL) {
      return false;
    } else {
      make_pairarrays_chimera(this_left,this_right,
#ifdef PMAP
			      queryaaseq_ptr,
#endif
			      queryseq,pairpool,/*gaplength*/0,ngap);

      this_left->chimera_right_p = true;
      this_right->chimera_left_p = true;
      return true;
    }
  }
}



void
Stage3_extend_right (T this, int goal, int querylength,
#ifdef PMAP
		     char *queryaaseq_ptr,
#endif
		     char *queryseq_ptr, char *queryuc_ptr,
		     bool max_extend_p, Pairpool_T pairpool,
		     int ngap, int maxpeelback) {
  List_T path, peeled_path;
  Pair_T leftpair, gappair;

  int nconsecutive_mismatches;
  int querypos, querydp5, genomedp5;
  int genomepos;
  char c, c_upper, g, g_alt, comp;
  bool mismatchp;

  debug10(printf("LEFT BEFORE FILL\n"));
  debug10(Pair_dump_list(this->pairs,true));
  debug10(printf("END_LEFT BEFORE FILL\n"));


  path = List_reverse(this->pairs);
  leftpair = (Pair_T) path->first;

  debug(printf("\nEXTEND_RIGHT\n"));
  querydp5 = leftpair->querypos + 1;
  genomedp5 = leftpair->genomepos + 1;
  if (leftpair->cdna == ' ') querydp5--;
  if (leftpair->genome == ' ') genomedp5--;

  path = peel_leftward(&mismatchp,&peeled_path,path,&querydp5,&genomedp5,
#ifdef WASTE
		       pairpool,
#endif
		       maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
		       /*endgappairs*/NULL,&gappair,
		       /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

  path = clean_path_end3_gap_indels(path);
  leftpair = (Pair_T) path->first;
  querypos = leftpair->querypos;
  genomepos = leftpair->genomepos;

  if (this->watsonp == true) {
    /* pos = this->chroffset + genomepos; */
    debug10(printf("watsonp on left is true.  pos is %u.  goal querypos is %d\n",genomepos,goal));

    querypos++;
    genomepos++;
    /* pos++; */
    while (querypos < goal /* && pos <= this->chrhigh */) {
      c = queryseq_ptr[querypos];
      c_upper = queryuc_ptr[querypos];
      /* g = Genome_get_char(genome,pos); */
      g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/true);
      if (g != '*') {
	if (c_upper == g) {
	  comp = MATCH_COMP;
	} else {
	  comp = MISMATCH_COMP;
	}
	debug10(printf("At querypos %d and pos %u on left, have %c and %c\n",
		       querypos,genomepos,c,g));
	path = Pairpool_push(path,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
      }
      querypos++;
      genomepos++;
      /* pos++; */
    }

    if (max_extend_p == true) {
      debug10(printf("\nGoal achieved.  Now looking for consecutive mismatches\n"));
      nconsecutive_mismatches = 0;
      while (querypos < querylength /* && pos <= this->chrhigh */ && nconsecutive_mismatches < 3) {
	c = queryseq_ptr[querypos];
	c_upper = queryuc_ptr[querypos];
	/* g = Genome_get_char(genome,pos); */
	g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/true);
	if (g != '*') {
	  if (c_upper == g) {
	    comp = MATCH_COMP;
	    nconsecutive_mismatches = 0;
	  } else {
	    comp = MISMATCH_COMP;
	    nconsecutive_mismatches += 1;
	  }
	  debug10(printf("At querypos %d and pos %u on left, have %c and %c\n",
			 querypos,genomepos,c,g));
	  path = Pairpool_push(path,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
	}
	querypos++;
	genomepos++;
	/* pos++; */
      }
    }

  } else {
    /* pos = this->chrhigh - genomepos; */
    debug10(printf("watsonp on left is false.  pos is %u.  querypos is %d.  want to go up to goal querypos %d\n",
		   genomepos,querypos,goal));

    querypos++;
    genomepos++;
    /* pos--; */
    while (querypos < goal /* && pos != this->chroffset - 1U */) {
      c = queryseq_ptr[querypos];
      c_upper = queryuc_ptr[querypos];
      /* g = complCode[(int) Genome_get_char(genome,pos)]; */
      g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/false);
      if (g != '*') {
	if (c_upper == g) {
	  comp = MATCH_COMP;
	} else {
	  comp = MISMATCH_COMP;
	}
	debug10(printf("At querypos %d and pos %u on left, have %c and %c\n",
		       querypos,genomepos,c,g));
	path = Pairpool_push(path,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
      }
      querypos++;
      genomepos++;
      /* pos--; */
    }

    if (max_extend_p == true) {
      debug10(printf("\nGoal achieved.  Now looking for consecutive mismatches\n"));
      nconsecutive_mismatches = 0;
      while (querypos < querylength /* && pos != this->chroffset - 1U */ && nconsecutive_mismatches < 3) {
	c = queryseq_ptr[querypos];
	c_upper = queryuc_ptr[querypos];
	/* g = complCode[(int) Genome_get_char(genome,pos)]; */
	g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/false);
	if (g != '*') {
	  if (c_upper == g) {
	    comp = MATCH_COMP;
	    nconsecutive_mismatches = 0;
	  } else {
	    comp = MISMATCH_COMP;
	    nconsecutive_mismatches += 1;
	  }
	  debug10(printf("At querypos %d and pos %u on left, have %c and %c\n",
			 querypos,genomepos,c,g));
	  path = Pairpool_push(path,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
	}
	querypos++;
	genomepos++;
	/* pos--; */
      }
    }
  }

  this->pairs = List_reverse(path);

  debug10(printf("LEFT AFTER FILL\n"));
  debug10(Pair_dump_list(this->pairs,true));
  debug10(printf("END_LEFT AFTER FILL\n"));

  Stage3_free_pairarray(&this);
  this->pairarray = make_pairarray(&this->npairs,&this->pairs,this->cdna_direction,this->sensedir,
				   this->watsonp,pairpool,queryseq_ptr,
#ifdef PMAP
				   queryaaseq_ptr,
#endif
				   this->chroffset,this->chrhigh,ngap,/*subseq_offset*/0,/*skiplength*/0,
				   /*diagnosticp*/false);
  if (this->pairarray == NULL) {
    this->pairarray_freeable_p = false;
  } else {
    this->pairarray_freeable_p = true;
  }

  return;
}


void
Stage3_extend_left (T this, int goal,
#ifdef PMAP
		    char *queryaaseq_ptr,
#endif
		    char *queryseq_ptr, char *queryuc_ptr,
		    bool max_extend_p, Pairpool_T pairpool,
		    int ngap, int maxpeelback) {
  List_T pairs, peeled_pairs;
  Pair_T rightpair, gappair;

  int nconsecutive_mismatches;
  int querypos, querydp3, genomedp3;
  int genomepos;
  char c, c_upper, g, g_alt, comp;
  bool mismatchp;

  debug10(printf("RIGHT BEFORE FILL\n"));
  debug10(Pair_dump_list(this->pairs,true));
  debug10(printf("END_RIGHT BEFORE FILL\n"));


  /* Do not call insert_gapholders */
  pairs = this->pairs;
  rightpair = (Pair_T) pairs->first;

  debug(printf("\nEXTEND_LEFT\n"));
  querydp3 = rightpair->querypos - 1;
  genomedp3 = rightpair->genomepos - 1;

  pairs = peel_rightward(&mismatchp,&peeled_pairs,pairs,&querydp3,&genomedp3,
#ifdef WASTE
			 pairpool,
#endif
			 maxpeelback,/*throughmismatchp*/true,/*quit_on_gap_p*/false,
			 /*endgappairs*/NULL,&gappair,
			 /*querydp_medialgap*/NULL,/*genomedp_medialgap*/NULL);

  pairs = clean_pairs_end5_gap_indels(pairs);
  rightpair = (Pair_T) pairs->first;
  querypos = rightpair->querypos;
  genomepos = rightpair->genomepos;
  
  if (this->watsonp == true) {
    /* pos = this->chroffset + genomepos; */
    debug10(printf("watsonp on right is true.  pos is %u.  goal querypos is %d\n",genomepos,goal));

    querypos--;
    genomepos--;
    /* pos--; */
    while (querypos >= goal /* && pos != this->chroffset - 1U */) {
      c = queryseq_ptr[querypos];
      c_upper = queryuc_ptr[querypos];
      /* g = Genome_get_char(genome,pos); */
      g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/true);
      if (g != '*') {
	if (c_upper == g) {
	  comp = MATCH_COMP;
	} else {
	  comp = MISMATCH_COMP;
	}
	debug10(printf("At querypos %d and pos %u on right, have %c and %c\n",
		       querypos,genomepos,c,g));
	pairs = Pairpool_push(pairs,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
      }
      querypos--;
      genomepos--;
      /* pos--; */
    }

    if (max_extend_p == true) {
      debug10(printf("\nGoal achieved.  Now looking for consecutive mismatches\n"));
      nconsecutive_mismatches = 0;
      while (querypos >= 0 /* && pos != this->chroffset - 1U */ && nconsecutive_mismatches < 3) {
	c = queryseq_ptr[querypos];
	c_upper = queryuc_ptr[querypos];
	/* g = Genome_get_char(genome,pos); */
	g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/true);
	if (g != '*') {
	  if (c_upper == g) {
	    comp = MATCH_COMP;
	    nconsecutive_mismatches = 0;
	  } else {
	    comp = MISMATCH_COMP;
	    nconsecutive_mismatches += 1;
	  }
	  debug10(printf("At querypos %d and pos %u on right, have %c and %c\n",
			 querypos,genomepos,c,g));
	  pairs = Pairpool_push(pairs,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
	}
	querypos--;
	genomepos--;
	/* pos--; */
      }
    }

  } else {
    /* pos = this->chrhigh - genomepos; */
    debug10(printf("watsonp on right is false.  pos is %u.  goal querypos is %d\n",genomepos,goal));

    querypos--;
    genomepos--;
    /* pos++; */
    while (querypos >= goal /* && pos <= this->chrhigh */) {
      c = queryseq_ptr[querypos];
      c_upper = queryuc_ptr[querypos];
      /* g = complCode[(int) Genome_get_char(genome,pos)]; */
      g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/false);
      if (g != '*') {
	if (c_upper == g) {
	  comp = MATCH_COMP;
	} else {
	  comp = MISMATCH_COMP;
	}
	debug10(printf("At querypos %d and pos %u on right, have %c and %c\n",
		       querypos,genomepos,c,g));
	pairs = Pairpool_push(pairs,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
      }
      querypos--;
      genomepos--;
      /* pos++; */
    }

    if (max_extend_p == true) {
      debug10(printf("\nGoal achieved.  Now looking for consecutive mismatches\n"));
      nconsecutive_mismatches = 0;
      while (querypos >= 0 /* && pos <= this->chrhigh */ && nconsecutive_mismatches > 3) {
	c = queryseq_ptr[querypos];
	c_upper = queryuc_ptr[querypos];
	/* g = complCode[(int) Genome_get_char(genome,pos)]; */
	g = get_genomic_nt(&g_alt,genomepos,this->chroffset,this->chrhigh,/*watsonp*/false);
	if (g != '*') {
	  if (c_upper == g) {
	    comp = MATCH_COMP;
	    nconsecutive_mismatches = 0;
	  } else {
	    comp = MISMATCH_COMP;
	    nconsecutive_mismatches += 1;
	  }
	  debug10(printf("At querypos %d and pos %u on right, have %c and %c\n",
			 querypos,genomepos,c,g));
	  pairs = Pairpool_push(pairs,pairpool,querypos,genomepos,c,comp,g,g_alt,/*dynprogindex*/0);
	}
	querypos--;
	genomepos--;
	/* pos++; */
      }
    }
  }

  this->pairs = pairs;

  debug10(printf("RIGHT AFTER FILL\n"));
  debug10(Pair_dump_list(this->pairs,true));
  debug10(printf("END_RIGHT AFTER FILL\n"));

  Stage3_free_pairarray(&this);
  this->pairarray = make_pairarray(&this->npairs,&this->pairs,this->cdna_direction,this->sensedir,
				   this->watsonp,pairpool,queryseq_ptr,
#ifdef PMAP
				   queryaaseq_ptr,
#endif
				   this->chroffset,this->chrhigh,ngap,/*subseq_offset*/0,/*skiplength*/0,
				   /*diagnosticp*/false);
  if (this->pairarray == NULL) {
    this->pairarray_freeable_p = false;
  } else {
    this->pairarray_freeable_p = true;
  }

  return;
}


#if 0
static void
adjust_genomepos (T this, int delta) {
  Pair_T pair;
  List_T p;

  for (p = this->pairs; p != NULL; p = List_next(p)) {
    pair = (Pair_T) List_head(p);
    pair->genomepos += delta;
  }

  return;
}
#endif


void
Stage3_merge_local_single (T this_left, T this_right,
			   int minpos1, int maxpos1, int minpos2, int maxpos2, int cdna_direction,
#ifdef PMAP
			   char *queryaaseq_ptr,
#endif
			   Sequence_T queryseq, char *queryseq_ptr, char *queryuc_ptr,
			   Pairpool_T pairpool, Dynprog_T dynprogM,
			   int maxpeelback, int extraband_single, int ngap) {
  Pair_T firstpair, lastpair, leftpair, rightpair;
  List_T path;
  bool watsonp, filledp;

#ifdef EXTRACT_GENOMICSEG
  char *genomicseg_ptr = NULL;
#endif
  Univcoord_T left;
  int firstpos, lastpos;
  int dynprogindex_minor = 0;


  this_left->pairs = Pairpool_clip_bounded(this_left->pairs,minpos1,maxpos1);
  this_right->pairs = Pairpool_clip_bounded(this_right->pairs,minpos2,maxpos2);

  Stage3_free_pairarray(&this_left);
  Stage3_free_pairarray(&this_right);

  if (this_left->pairs == NULL && this_right->pairs == NULL) {
    this_left->pairarray = (struct Pair_T *) NULL;
    this_right->pairarray = (struct Pair_T *) NULL;
    this_left->pairarray_freeable_p = false;
    this_right->pairarray_freeable_p = false;
    return;

  } else if ((watsonp = this_left->watsonp) == true) {
    debug10(printf("watsonp %d\n",watsonp));

#if 0
    /* Has no effect on plus strand */
    Pair_set_genomepos_list(this_left->pairs,chroffset,chrhigh,/*watsonp*/true);
    Pair_set_genomepos_list(this_right->pairs,chroffset,chrhigh,/*watsonp*/true);
#endif

    debug10(printf("LEFT\n"));
    debug10(Pair_dump_list(this_left->pairs,true));
    debug10(printf("END LEFT\n"));

    debug10(printf("RIGHT\n"));
    debug10(Pair_dump_list(this_right->pairs,true));
    debug10(printf("END RIGHT\n"));

    firstpair = (Pair_T) List_head(this_left->pairs);
    lastpair = (Pair_T) List_last_value(this_right->pairs);
    firstpos = firstpair->genomepos;
    lastpos = lastpair->genomepos;

    left = this_left->chroffset + firstpos;

#ifdef EXTRACT_GENOMICSEG
    genomicseg = Genome_get_segment(genome,left,genomiclength,/*chromosome_iit*/NULL,/*revcomp*/false);
    genomicseg_ptr = genomicuc_ptr = Sequence_fullpointer(genomicseg);
#endif

#if 0
    /* Has no effect on plus strand */
    Pair_set_genomepos_list(this_left->pairs,chroffset,chrhigh,/*watsonp*/true);
    Pair_set_genomepos_list(this_right->pairs,chroffset,chrhigh,/*watsonp*/true);
#endif

    debug10(printf("LEFT\n"));
    debug10(Pair_dump_list(this_left->pairs,true));
    debug10(printf("END LEFT\n"));

    debug10(printf("RIGHT\n"));
    debug10(Pair_dump_list(this_right->pairs,true));
    debug10(printf("END RIGHT\n"));

    path = List_reverse(this_left->pairs);

    leftpair = (Pair_T) path->first;
    rightpair = (Pair_T) this_right->pairs->first;

    this_right->pairs = traverse_single_gap(&filledp,&dynprogindex_minor,this_right->pairs,&path,leftpair,rightpair,
					    this_right->chroffset,this_right->chrhigh,
#ifdef PMAP
					    queryaaseq_ptr,
#endif
					    queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
					    /*jump_late_p*/watsonp ? false : true,pairpool,dynprogM,
					    maxpeelback,extraband_single,/*defect_rate*/0,
					    /*close_indels_mode*/+1,/*forcep*/true);
    this_left->pairs = List_reverse(path);

  } else {
    debug10(printf("watsonp %d\n",watsonp));

#if 0
    /* Do not change list, just pairarray */
    Pair_set_genomepos_list(this_left->pairs,chroffset,chrhigh,/*watsonp*/false);
    Pair_set_genomepos_list(this_right->pairs,chroffset,chrhigh,/*watsonp*/false);
#endif

    debug10(printf("LEFT\n"));
    debug10(Pair_dump_list(this_left->pairs,true));
    debug10(printf("END LEFT\n"));

    debug10(printf("RIGHT\n"));
    debug10(Pair_dump_list(this_right->pairs,true));
    debug10(printf("END RIGHT\n"));

    firstpair = (Pair_T) List_head(this_left->pairs);
    lastpair = (Pair_T) List_last_value(this_right->pairs);
    firstpos = firstpair->genomepos;
    lastpos = lastpair->genomepos;

    left = this_right->chroffset + lastpos;

#ifdef EXTRACT_GENOMICSEG
    genomicseg = Genome_get_segment(genome,left,genomiclength,/*chromosome_iit*/NULL,/*revcomp*/true);
    genomicseg_ptr = genomicuc_ptr = Sequence_fullpointer(genomicseg);
#endif

#if 0
    /* Do not change list, just pairarray */
    Pair_set_genomepos_list(this_left->pairs,chroffset,chrhigh,/*watsonp*/false);
    Pair_set_genomepos_list(this_right->pairs,chroffset,chrhigh,/*watsonp*/false);
#endif

    debug10(printf("LEFT\n"));
    debug10(Pair_dump_list(this_left->pairs,true));
    debug10(printf("END LEFT\n"));

    debug10(printf("RIGHT\n"));
    debug10(Pair_dump_list(this_right->pairs,true));
    debug10(printf("END RIGHT\n"));

    path = List_reverse(this_left->pairs);

    leftpair = (Pair_T) path->first;
    rightpair = (Pair_T) this_right->pairs->first;

    this_right->pairs = traverse_single_gap(&filledp,&dynprogindex_minor,this_right->pairs,&path,leftpair,rightpair,
					    this_right->chroffset,this_right->chrhigh,
#ifdef PMAP
					    queryaaseq_ptr,
#endif
					    queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
					    /*jump_late_p*/watsonp ? false : true,pairpool,dynprogM,
					    maxpeelback,extraband_single,/*defect_rate*/0,
					    /*close_indels_mode*/+1,/*forcep*/true);
    this_left->pairs = List_reverse(path);
  }

  make_pairarray_merge(this_left,this_right,this_left->cdna_direction,this_left->sensedir,
		       this_left->watsonp,pairpool,queryseq_ptr,
#ifdef PMAP
		       queryaaseq_ptr,
#endif
		       this_left->chroffset,this_left->chrhigh,ngap,/*subseq_offset*/0,/*skiplength*/0,
		       /*diagnosticp*/false,/*new_gap_p*/false);

  return;
}


void
Stage3_merge_local_splice (T this_left, T this_right, char comp,
			   int minpos1, int maxpos1, int minpos2, int maxpos2,
			   int queryjump, int genomejump, int cdna_direction,
#ifdef PMAP
			   char *queryaaseq_ptr,
#endif
			   Sequence_T queryseq, char *queryseq_ptr, char *queryuc_ptr,
			   Pairpool_T pairpool, Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
			   Genome_T genome, Genome_T genomealt,
			   int maxpeelback, int extramaterial_paired, int extraband_paired,
			   int extraband_single, int ngap) {
  Pair_T end1, start2, newpair = NULL, oldpair, pair, firstpair, lastpair, leftpair, rightpair;
  List_T pairs = NULL, path;
  bool watsonp, filledp, shiftp;
  int genomicpos, querypos, i;
  int gaplength;
  char *genomicseg_ptr, *genomicsegalt_ptr;
  Univcoord_T left;
  int firstpos, lastpos;
  Sequence_T genomicseg, genomicsegalt;

  int intronlength;
  bool intronp;

  int dynprogindex_minor = 0, dynprogindex_major = 0;


  this_left->pairs = Pairpool_clip_bounded(this_left->pairs,minpos1,maxpos1);
  this_right->pairs = Pairpool_clip_bounded(this_right->pairs,minpos2,maxpos2);

  Stage3_free_pairarray(&this_left);
  Stage3_free_pairarray(&this_right);

  if (this_left->pairs == NULL && this_right->pairs == NULL) {
    this_left->pairarray = (struct Pair_T *) NULL;
    this_right->pairarray = (struct Pair_T *) NULL;
    this_left->pairarray_freeable_p = false;
    this_right->pairarray_freeable_p = false;
    return;
  }

  watsonp = this_left->watsonp;
  if (watsonp == true) {
    debug10(printf("watsonp true\n"));
    
    firstpair = (Pair_T) List_head(this_left->pairs);
    lastpair = (Pair_T) List_last_value(this_right->pairs);
    firstpos = firstpair->genomepos;
    lastpos = lastpair->genomepos;

    left = this_left->chroffset + firstpos;

  } else {
    debug10(printf("watsonp false\n"));
    
    firstpair = (Pair_T) List_head(this_left->pairs);
    lastpair = (Pair_T) List_last_value(this_right->pairs);
    firstpos = firstpair->genomepos;
    lastpos = lastpair->genomepos;
    
    left = this_right->chroffset + lastpos;

  }

  /* Determine if the gap is an intron or not */
  end1 = (Pair_T) List_last_value(this_left->pairs);
  start2 = (Pair_T) List_head(this_right->pairs);
  intronlength = start2->genomepos - end1->genomepos - 1;

  if (splicingp == false) {
    debug10(printf("splicingp is false, so not an intron\n"));
    intronp = false;

  } else if (intronlength < min_intronlength) {
    debug10(printf("intronlength %d < min_intronlength %d, so not an intron\n",
		   intronlength,min_intronlength));
    intronp = false;

  } else {
    intronp = true;
  }

  if (intronp == false) {
    debug10(printf("Before\n"));
    debug10(Pair_dump_list(this_left->pairs,true));
    debug10(Pair_dump_list(this_right->pairs,true));

    end1 = (Pair_T) List_last_value(this_left->pairs);
    start2 = (Pair_T) List_head(this_right->pairs);

    Stage3_merge_local_single(this_left,this_right,
			      minpos1,/*maxpos1*/end1->querypos,
			      /*minpos2*/start2->querypos,maxpos2,cdna_direction,
#ifdef PMAP
			      queryaaseq_ptr,
#endif
			      queryseq,queryseq_ptr,queryuc_ptr,
			      pairpool,dynprogM,maxpeelback,extraband_single,ngap);

    debug10(printf("After\n"));
    debug10(Pair_dump_list(this_left->pairs,true));

  } else {
    /* Intron */
    path = List_reverse(this_left->pairs);
    leftpair = (Pair_T) path->first;
    rightpair = (Pair_T) this_right->pairs->first;

    debug10(printf("traverse_genome_gap..."));
    this_right->pairs = traverse_genome_gap(&filledp,&shiftp,&dynprogindex_minor,&dynprogindex_major,
					    this_right->pairs,&path,leftpair,rightpair,
					    this_left->chrnum,this_left->chroffset,this_left->chrhigh,
#ifdef PMAP
					    queryaaseq_ptr,
#endif
					    queryseq_ptr,queryuc_ptr,cdna_direction,watsonp,
					    /*jump_late_p*/watsonp ? false : true,pairpool,
					    dynprogL,dynprogM,dynprogR,
					    maxpeelback,extramaterial_paired,extraband_paired,extraband_single,
					    /*defect_rate*/0,/*close_indels_mode*/+1,/*finalp*/true);
    debug10(printf("done"));

    if (filledp == false) {
      this_right->pairs = Pairpool_push_gapholder(this_right->pairs,pairpool,queryjump,genomejump,/*knownp*/false);
    }

    this_left->pairs = List_reverse(path);

    make_pairarray_merge(this_left,this_right,cdna_direction,this_left->sensedir,
			 this_left->watsonp,pairpool,queryseq_ptr,
#ifdef PMAP
			 queryaaseq_ptr,
#endif
			 this_left->chroffset,this_left->chrhigh,ngap,/*subseq_offset*/0,/*skiplength*/0,
			 /*diagnosticp*/false,/*new_gap_p*/true);
  }
      
  this_left->cdna_direction = cdna_direction;

  return;
}

#ifndef PMAP
void
Stage3_guess_cdna_direction (T this) {
  this->cdna_direction = Pair_guess_cdna_direction_array(&this->sensedir,this->pairarray,this->npairs,
							 /*invertedp*/false,this->chroffset,this->watsonp);
  Pair_fix_cdna_direction_array(this->pairarray,this->npairs,this->cdna_direction);
  return;
}
#endif

