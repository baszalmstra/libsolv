/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * Generic policy interface for SAT solver
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "solver.h"
#include "solver_private.h"
#include "evr.h"
#include "policy.h"
#include "poolvendor.h"
#include "poolarch.h"
#include "linkedpkg.h"
#include "cplxdeps.h"



/*-----------------------------------------------------------------*/

/*
 * prep for prune_best_version
 *   sort by name
 */

static int
prune_to_best_version_sortcmp(const void *ap, const void *bp, void *dp)
{
  Pool *pool = dp;
  int r;
  Id a = *(Id *)ap;
  Id b = *(Id *)bp;
  Solvable *sa, *sb;

  sa = pool->solvables + a;
  sb = pool->solvables + b;
  r = sa->name - sb->name;
  if (r)
    {
      const char *na, *nb;
      /* different names. We use real strcmp here so that the result
       * is not depending on some random solvable order */
      na = pool_id2str(pool, sa->name);
      nb = pool_id2str(pool, sb->name);
      return strcmp(na, nb);
    }
  if (sa->arch != sb->arch)
    {
      unsigned int aa, ab;
      aa = pool_arch2score(pool, sa->arch);
      ab = pool_arch2score(pool, sb->arch);
      if (aa != ab && aa > 1 && ab > 1)
	return aa < ab ? -1 : 1;	/* lowest score first */
    }

  /* the same name, bring installed solvables to the front */
  if (pool->installed)
    {
      if (sa->repo == pool->installed)
	{
	  if (sb->repo != pool->installed)
	    return -1;
	}
      else if (sb->repo == pool->installed)
	return 1;	
    }
  /* sort by repository sub-prio (installed repo handled above) */
  r = (sb->repo ? sb->repo->subpriority : 0) - (sa->repo ? sa->repo->subpriority : 0);
  if (r)
    return r;
  /* no idea about the order, sort by id */
  return a - b;
}


/*
 * prune to repository with highest priority.
 * does not prune installed solvables.
 */

static void
prune_to_highest_prio(Pool *pool, Queue *plist)
{
  int i, j;
  Solvable *s;
  int bestprio = 0, bestprioset = 0;

  /* prune to highest priority */
  for (i = 0; i < plist->count; i++)  /* find highest prio in queue */
    {
      s = pool->solvables + plist->elements[i];
      if (pool->installed && s->repo == pool->installed)
	continue;
      if (!bestprioset || s->repo->priority > bestprio)
	{
	  bestprio = s->repo->priority;
	  bestprioset = 1;
	}
    }
  if (!bestprioset)
    return;
  for (i = j = 0; i < plist->count; i++) /* remove all with lower prio */
    {
      s = pool->solvables + plist->elements[i];
      if (s->repo->priority == bestprio || (pool->installed && s->repo == pool->installed))
	plist->elements[j++] = plist->elements[i];
    }
  plist->count = j;
}


/* installed packages involed in a dup operation can only be kept
 * if they are identical to a non-installed one */
static void
solver_prune_installed_dup_packages(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, j, bestprio = 0;

  /* find bestprio (again) */
  for (i = 0; i < plist->count; i++)
    {
      Solvable *s = pool->solvables + plist->elements[i];
      if (s->repo != pool->installed)
	{
	  bestprio = s->repo->priority;
	  break;
	}
    }
  if (i == plist->count)
    return;	/* only installed packages, could not find prio */
  for (i = j = 0; i < plist->count; i++)
    {
      Id p = plist->elements[i];
      Solvable *s = pool->solvables + p;
      if (s->repo != pool->installed && s->repo->priority < bestprio)
	continue;
      if (s->repo == pool->installed && (solv->dupinvolvedmap_all || (solv->dupinvolvedmap.size && MAPTST(&solv->dupinvolvedmap, p))))
	{
	  Id p2, pp2;
	  int keepit = 0;
	  FOR_PROVIDES(p2, pp2, s->name)
	    {
	      Solvable *s2 = pool->solvables + p2;
	      if (s2->repo == pool->installed || s2->evr != s->evr || s2->repo->priority < bestprio)
		continue;
	      if (!solvable_identical(s, s2))
		continue;
	      keepit = 1;
	      if (s2->repo->priority > bestprio)
		{
		  /* new max prio! */
		  bestprio = s2->repo->priority;
		  j = 0;
		}
	    }
	  if (!keepit)
	    continue;	/* no identical package found, ignore installed package */
	}
      plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;
}

/*
 * like prune_to_highest_prio, but calls solver prune_installed_dup_packages
 * when there are dup packages
 */
static inline void
solver_prune_to_highest_prio(Solver *solv, Queue *plist)
{
  prune_to_highest_prio(solv->pool, plist);
  if (plist->count > 1 && solv->pool->installed && (solv->dupinvolvedmap_all || solv->dupinvolvedmap.size))
    solver_prune_installed_dup_packages(solv, plist);
}


static void
solver_prune_to_highest_prio_per_name(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  Queue pq;
  int i, j, k;
  Id name;

  queue_init(&pq);
  solv_sort(plist->elements, plist->count, sizeof(Id), prune_to_best_version_sortcmp, pool);
  queue_push(&pq, plist->elements[0]);
  name = pool->solvables[pq.elements[0]].name;
  for (i = 1, j = 0; i < plist->count; i++)
    {
      if (pool->solvables[plist->elements[i]].name != name)
	{
	  name = pool->solvables[plist->elements[i]].name;
	  if (pq.count > 2)
	    solver_prune_to_highest_prio(solv, &pq);
	  for (k = 0; k < pq.count; k++)
	    plist->elements[j++] = pq.elements[k];
	  queue_empty(&pq);
	}
      queue_push(&pq, plist->elements[i]);
    }
  if (pq.count > 2)
    solver_prune_to_highest_prio(solv, &pq);
  for (k = 0; k < pq.count; k++)
    plist->elements[j++] = pq.elements[k];
  queue_free(&pq);
  plist->count = j;
}


#ifdef ENABLE_COMPLEX_DEPS

/* simple fixed-size hash for package ids */
#define CPLXDEPHASH_EMPTY(elements) (memset(elements, 0, sizeof(Id) * 256))
#define CPLXDEPHASH_SET(elements, p) (elements[(p) & 255] |= (1 << ((p) >> 8 & 31)))
#define CPLXDEPHASH_TST(elements, p) (elements[(p) & 255] && (elements[(p) & 255] & (1 << ((p) >> 8 & 31))))

static void
check_complex_dep(Solver *solv, Id dep, Map *m, Queue **cqp)
{
  Pool *pool = solv->pool;
  Queue q;
  Id p;
  int i, qcnt;

#if 0
  printf("check_complex_dep %s\n", pool_dep2str(pool, dep));
#endif
  queue_init(&q);
  i = pool_normalize_complex_dep(pool, dep, &q, CPLXDEPS_EXPAND);
  if (i == 0 || i == 1)
    {
      queue_free(&q);
      return;
    }
  qcnt = q.count;
  for (i = 0; i < qcnt; i++)
    {
      /* we rely on the fact that blocks are ordered here.
       * if we reach a positive element, we know that we
       * saw all negative ones */
      for (; (p = q.elements[i]) < 0; i++)
	{
	  if (solv->decisionmap[-p] < 0)
	    break;
	  if (solv->decisionmap[-p] == 0)
	    queue_push(&q, -p);		/* undecided negative literal */
	}
      if (p <= 0)
	{
#if 0
	  printf("complex dep block cannot be true or no pos literals\n");
#endif
	  while (q.elements[i])
	    i++;
	  if (qcnt != q.count)
	    queue_truncate(&q, qcnt);
	  continue;
	}
      if (qcnt == q.count)
	{
	  /* all negative literals installed, add positive literals to map */
	  for (; (p = q.elements[i]) != 0; i++)
	    MAPSET(m, p);
	}
      else
	{
	  /* at least one undecided negative literal, postpone */
	  int j, k;
	  Queue *cq;
#if 0
	  printf("add new complex dep block\n");
	  for (j = qcnt; j < q.count; j++)
	    printf("  - %s\n", pool_solvid2str(pool, q.elements[j]));
#endif
	  while (q.elements[i])
	    i++;
	  if (!(cq = *cqp))
	    {
	      cq = solv_calloc(1, sizeof(Queue));
	      queue_init(cq);
	      queue_insertn(cq, 0, 256, 0);	/* allocate hash area */
	      *cqp = cq;
	    }
	  for (j = qcnt; j < q.count; j++)
	    {
	      p = q.elements[j];
	      /* check if we already have this (dep, p) entry */
	      for (k = 256; k < cq->count; k += 2)
		if (cq->elements[k + 1] == dep && cq->elements[k] == p)
		  break;
	      if (k == cq->count)
		{
		  /* a new one. add to cq and hash */
	          queue_push2(cq, p, dep);
		  CPLXDEPHASH_SET(cq->elements, p);
		}
	    }
	  queue_truncate(&q, qcnt);
	}
    }
  queue_free(&q);
}

static void
recheck_complex_deps(Solver *solv, Id p, Map *m, Queue **cqp)
{
  Queue *cq = *cqp;
  Id pp;
  int i;
#if 0
  printf("recheck_complex_deps for package %s\n", pool_solvid2str(solv->pool, p));
#endif
  /* make sure that we don't have a false hit */
  for (i = 256; i < cq->count; i += 2)
    if (cq->elements[i] == p)
      break;
  if (i == cq->count)
    return;	/* false alert */
  if (solv->decisionmap[p] <= 0)
    return;	/* just in case... */

  /* rebuild the hash, call check_complex_dep for our package */
  CPLXDEPHASH_EMPTY(cq->elements);
  for (i = 256; i < cq->count; i += 2)
    if ((pp = cq->elements[i]) == p)
      {
	Id dep = cq->elements[i + 1];
	queue_deleten(cq, i, 2);
	i -= 2;
        check_complex_dep(solv, dep, m, &cq);
      }
    else
      CPLXDEPHASH_SET(cq->elements, pp);
}

#endif


void
policy_update_recommendsmap(Solver *solv)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Id p, pp, rec, *recp, sug, *sugp;

  if (solv->recommends_index < 0)
    {
      MAPZERO(&solv->recommendsmap);
      MAPZERO(&solv->suggestsmap);
#ifdef ENABLE_COMPLEX_DEPS
      if (solv->recommendscplxq)
	{
	  queue_free(solv->recommendscplxq);
	  solv->recommendscplxq = solv_free(solv->recommendscplxq);
	}
      if (solv->suggestscplxq)
	{
	  queue_free(solv->suggestscplxq);
	  solv->suggestscplxq = solv_free(solv->suggestscplxq);
	}
#endif
      solv->recommends_index = 0;
    }
  while (solv->recommends_index < solv->decisionq.count)
    {
      p = solv->decisionq.elements[solv->recommends_index++];
      if (p < 0)
	continue;
      s = pool->solvables + p;
#ifdef ENABLE_COMPLEX_DEPS
      /* re-check postponed complex blocks */
      if (solv->recommendscplxq && CPLXDEPHASH_TST(solv->recommendscplxq->elements, p))
        recheck_complex_deps(solv, p, &solv->recommendsmap, &solv->recommendscplxq);
      if (solv->suggestscplxq && CPLXDEPHASH_TST(solv->suggestscplxq->elements, p))
        recheck_complex_deps(solv, p, &solv->suggestsmap, &solv->suggestscplxq);
#endif
      if (s->recommends)
	{
	  recp = s->repo->idarraydata + s->recommends;
          while ((rec = *recp++) != 0)
	    {
#ifdef ENABLE_COMPLEX_DEPS
	      if (pool_is_complex_dep(pool, rec))
		{
		  check_complex_dep(solv, rec, &solv->recommendsmap, &solv->recommendscplxq);
		  continue;
		}
#endif
	      FOR_PROVIDES(p, pp, rec)
	        MAPSET(&solv->recommendsmap, p);
	    }
	}
      if (s->suggests)
	{
	  sugp = s->repo->idarraydata + s->suggests;
          while ((sug = *sugp++) != 0)
	    {
#ifdef ENABLE_COMPLEX_DEPS
	      if (pool_is_complex_dep(pool, sug))
		{
		  check_complex_dep(solv, sug, &solv->suggestsmap, &solv->suggestscplxq);
		  continue;
		}
#endif
	      FOR_PROVIDES(p, pp, sug)
	        MAPSET(&solv->suggestsmap, p);
	    }
	}
    }
}

/* bring suggested/enhanced packages to front
 * installed packages count as suggested */
static void
prefer_suggested(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, count;

  /* update our recommendsmap/suggestsmap */
  if (solv->recommends_index < solv->decisionq.count)
    policy_update_recommendsmap(solv);

  for (i = 0, count = plist->count; i < count; i++)
    {
      Id p = plist->elements[i];
      Solvable *s = pool->solvables + p;
      if ((pool->installed && s->repo == pool->installed) ||
          MAPTST(&solv->suggestsmap, p) ||
          solver_is_enhancing(solv, s))
	continue;	/* good package */
      /* bring to back */
     if (i < plist->count - 1)
	{
	  memmove(plist->elements + i, plist->elements + i + 1, (plist->count - 1 - i) * sizeof(Id));
	  plist->elements[plist->count - 1] = p;
	}
      i--;
      count--;
    }
}

static int
sort_by_favor_cmp(const void *ap, const void *bp, void *dp)
{
  const Id *a = ap, *b = bp, *d = dp;
  return d[b[0]] - d[a[0]];
}

/* bring favored packages to front and disfavored packages to back */
void
policy_prefer_favored(Solver *solv, Queue *plist)
{
  if (solv->favormap && plist->count > 1)
    solv_sort(plist->elements, plist->count, sizeof(Id), sort_by_favor_cmp, solv->favormap);
}

/*
 * prune to recommended/suggested packages.
 * does not prune installed packages (they are also somewhat recommended).
 */
static void
prune_to_recommended(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, j, k, ninst;
  Solvable *s;
  Id p;

  ninst = 0;
  if (pool->installed)
    {
      for (i = 0; i < plist->count; i++)
	{
	  p = plist->elements[i];
	  s = pool->solvables + p;
	  if (pool->installed && s->repo == pool->installed)
	    ninst++;
	}
    }
  if (plist->count - ninst < 2)
    return;

  /* update our recommendsmap/suggestsmap */
  if (solv->recommends_index < solv->decisionq.count)
    policy_update_recommendsmap(solv);

  /* prune to recommended/supplemented */
  ninst = 0;
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      s = pool->solvables + p;
      if (pool->installed && s->repo == pool->installed)
	{
	  ninst++;
	  if (j)
	    plist->elements[j++] = p;
	  continue;
	}
      if (!MAPTST(&solv->recommendsmap, p))
	if (!solver_is_supplementing(solv, s))
	  continue;
      if (!j && ninst)
	{
	  for (k = 0; j < ninst; k++)
	    {
	      s = pool->solvables + plist->elements[k];
	      if (pool->installed && s->repo == pool->installed)
	        plist->elements[j++] = plist->elements[k];
	    }
	}
      plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;

#if 0
  /* anything left to prune? */
  if (plist->count - ninst < 2)
    return;

  /* prune to suggested/enhanced */
  ninst = 0;
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      s = pool->solvables + p;
      if (pool->installed && s->repo == pool->installed)
	{
	  ninst++;
	  if (j)
	    plist->elements[j++] = p;
	  continue;
	}
      if (!MAPTST(&solv->suggestsmap, p))
        if (!solver_is_enhancing(solv, s))
	  continue;
      if (!j && ninst)
	{
	  for (k = 0; j < ninst; k++)
	    {
	      s = pool->solvables + plist->elements[k];
	      if (pool->installed && s->repo == pool->installed)
	        plist->elements[j++] = plist->elements[k];
	    }
	}
      plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;
#endif
}

static void
prune_to_best_arch(const Pool *pool, Queue *plist)
{
  Id a, bestscore;
  Solvable *s;
  int i, j;

  if (!pool->id2arch || plist->count < 2)
    return;
  bestscore = 0;
  for (i = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      a = pool_arch2score(pool, s->arch);
      if (a && a != 1 && (!bestscore || a < bestscore))
	bestscore = a;
    }
  if (!bestscore)
    return;
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      a = pool_arch2score(pool, s->arch);
      if (!a)
	continue;
      /* a == 1 -> noarch */
      if (a != 1 && ((a ^ bestscore) & 0xffff0000) != 0)
	continue;
      plist->elements[j++] = plist->elements[i];
    }
  if (j)
    plist->count = j;
}


struct trj_data {
  Pool *pool;
  Queue *plist;
  Id *stack;
  Id nstack;
  Id *low;
  Id firstidx;
  Id idx;
};

/* This is Tarjan's SCC algorithm, slightly modified */
static void
trj_visit(struct trj_data *trj, Id node)
{
  Id *low = trj->low;
  Pool *pool = trj->pool;
  Queue *plist = trj->plist;
  Id myidx, stackstart;
  Solvable *s;
  int i;
  Id p, pp, obs, *obsp;

  low[node] = myidx = trj->idx++;
  trj->stack[(stackstart = trj->nstack++)] = node;

  s = pool->solvables + plist->elements[node];
  if (s->obsoletes)
    {
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      Solvable *ps = pool->solvables + p;
	      if (ps->name == s->name)
		continue;
	      if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
		continue;
	      if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
		continue;
	      /* hmm, expensive. should use hash if plist is big */
	      for (i = 0; i < plist->count; i++)
		{
		  if (node != i && plist->elements[i] == p)
		    {
		      Id l = low[i];
		      if (!l)
			{
			  if (!ps->obsoletes)
			    {
			      /* don't bother */
			      trj->idx++;
			      low[i] = -1;
			      continue;
			    }
			  trj_visit(trj, i);
			  l = low[i];
			}
		      if (l < 0)
			continue;
		      if (l < trj->firstidx)
			{
			  int k;
			  /* this means we have reached an old SCC found earlier.
			   * delete it as we obsolete it */
			  for (k = l; ; k++)
			    {
			      if (low[trj->stack[k]] == l)
				low[trj->stack[k]] = -1;
			      else
				break;
			    }
			}
		      else if (l < low[node])
			low[node] = l;
		    }
		}
	    }
	}
    }
  if (low[node] == myidx)	/* found a SCC? */
    {
      /* we're only interested in SCCs that contain the first node,
       * as all others are "obsoleted" */
      if (myidx != trj->firstidx)
	myidx = -1;
      for (i = stackstart; i < trj->nstack; i++)
	low[trj->stack[i]] = myidx;
      trj->nstack = stackstart;	/* empty stack */
    }
}

/*
 * remove entries from plist that are obsoleted by other entries
 * with different name.
 */
static void
prune_obsoleted(Pool *pool, Queue *plist)
{
  Id data_buf[2 * 16], *data;
  struct trj_data trj;
  int i, j;
  Solvable *s;

  if (plist->count <= 16)
    {
      memset(data_buf, 0, sizeof(data_buf));
      data = data_buf;
    }
  else
    data = solv_calloc(plist->count, 2 * sizeof(Id));
  trj.pool = pool;
  trj.plist = plist;
  trj.low = data;
  trj.idx = 1;
  trj.stack = data + plist->count - 1;	/* -1 so we can index with idx (which starts with 1) */
  for (i = 0; i < plist->count; i++)
    {
      if (trj.low[i])
	continue;
      s = pool->solvables + plist->elements[i];
      if (s->obsoletes)
	{
	  trj.firstidx = trj.nstack = trj.idx;
          trj_visit(&trj, i);
	}
      else
        {
          Id myidx = trj.idx++;
          trj.low[i] = myidx;
          trj.stack[myidx] = i;
        }
    }
  for (i = j = 0; i < plist->count; i++)
    if (trj.low[i] >= 0)
      plist->elements[j++] = plist->elements[i];
  plist->count = j;
  if (data != data_buf)
    solv_free(data);
}

/* this is prune_obsoleted special-cased for two elements */
static void
prune_obsoleted_2(Pool *pool, Queue *plist)
{
  int i;
  Solvable *s;
  Id p, pp, obs, *obsp;
  Id other;
  int obmap = 0;

  for (i = 0; i < 2; i++)
    {
      s = pool->solvables + plist->elements[i];
      other = plist->elements[1 - i];
      if (s->obsoletes)
	{
	  obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, obs)
		{
		  Solvable *ps;
		  if (p != other)
		    continue;
		  ps = pool->solvables + p;
		  if (ps->name == s->name)
		    continue;
		  if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
		    continue;
		  if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
		    continue;
		  obmap |= 1 << i;
		  break;
		}
	      if (p)
		break;
	    }
	}
    }
  if (obmap == 0 || obmap == 3)
    return;
  if (obmap == 2)
    plist->elements[0] = plist->elements[1];
  plist->count = 1;
}

/*
 * bring those elements to the front of the queue that
 * have a installed solvable with the same name
 */
static void
move_installed_to_front(Pool *pool, Queue *plist)
{
  int i, j;
  Solvable *s;
  Id p, pp;

  if (!pool->installed)
    return;
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      if (s->repo != pool->installed)
        {
          FOR_PROVIDES(p, pp, s->name)
	    {
	      Solvable *ps = pool->solvables + p;
	      if (s->name == ps->name && ps->repo == pool->installed)
		{
		  s = ps;
		  break;
		}
	    }
        }
      if (s->repo == pool->installed)
	{
	  if (i != j)
	    {
	      p = plist->elements[i];
              if (i - j == 1)
		plist->elements[i] = plist->elements[j];
	      else
	        memmove(plist->elements + j + 1, plist->elements + j, (i - j) * sizeof(Id));
	      plist->elements[j] = p;
	    }
	  else if (j + 2 == plist->count)
	    break;	/* no need to check last element if all prev ones are installed */
	  j++;
	}
    }
}

/*
 * prune_to_best_version
 *
 * sort list of packages (given through plist) by name and evr
 * return result through plist
 */
void
prune_to_best_version(Pool *pool, Queue *plist)
{
#ifdef ENABLE_CONDA
  if (pool->disttype == DISTTYPE_CONDA)
     return prune_to_best_version_conda(pool, plist);
#endif

  int i, j, r;
  Solvable *s, *best;

  if (plist->count < 2)		/* no need to prune for a single entry */
    return;
  POOL_DEBUG(SOLV_DEBUG_POLICY, "prune_to_best_version %d\n", plist->count);

  /* sort by name first, prefer installed */
  solv_sort(plist->elements, plist->count, sizeof(Id), prune_to_best_version_sortcmp, pool);

  /* now find best 'per name' */
  best = 0;
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];

      POOL_DEBUG(SOLV_DEBUG_POLICY, "- %s [%d]%s\n",
		 pool_solvable2str(pool, s), plist->elements[i], 
		 (pool->installed && s->repo == pool->installed) ? "I" : "");

      if (!best)		/* if no best yet, the current is best */
        {
          best = s;
          continue;
        }

      /* name switch: finish group, re-init */
      if (best->name != s->name)   /* new name */
        {
          plist->elements[j++] = best - pool->solvables; /* move old best to front */
          best = s;		/* take current as new best */
          continue;
        }
     
      r = 0; 
      if (r == 0)
        r = best->evr != s->evr ? pool_evrcmp(pool, best->evr, s->evr, EVRCMP_COMPARE) : 0;
#ifdef ENABLE_LINKED_PKGS
      if (r == 0 && has_package_link(pool, s))
        r = pool_link_evrcmp(pool, best, s);
#endif
      if (r < 0)
	best = s;
    }

  plist->elements[j++] = best - pool->solvables;	/* finish last group */
  plist->count = j;

  /* we reduced the list to one package per name, now look at
   * package obsoletes */
  if (plist->count > 1)
    {
      if (plist->count == 2)
        prune_obsoleted_2(pool, plist);
      else
        prune_obsoleted(pool, plist);
    }
}

#ifdef ENABLE_CONDA
static int
pool_featurecountcmp(Pool *pool, Solvable *s1, Solvable *s2)
{
  unsigned int cnt1, cnt2;
  cnt1 = solvable_lookup_count(s1, SOLVABLE_TRACK_FEATURES);
  cnt2 = solvable_lookup_count(s2, SOLVABLE_TRACK_FEATURES);
  return cnt1 == cnt2 ? 0 : cnt1 > cnt2 ? -1 : 1;
}

static int
pool_buildversioncmp(Pool *pool, Solvable *s1, Solvable *s2)
{
  const char *bv1, *bv2;
  bv1 = solvable_lookup_str(s1, SOLVABLE_BUILDVERSION);
  bv2 = solvable_lookup_str(s2, SOLVABLE_BUILDVERSION);
  if (!bv1 && !bv2)
    return 0;
  return pool_evrcmp_str(pool, bv1 ? bv1 : "" , bv2 ? bv2 : "", EVRCMP_COMPARE);
}

static int
pool_buildflavorcmp(Pool *pool, Solvable *s1, Solvable *s2)
{
  const char *f1 = solvable_lookup_str(s1, SOLVABLE_BUILDFLAVOR);
  const char *f2 = solvable_lookup_str(s2, SOLVABLE_BUILDFLAVOR);
  if (!f1 && !f2)
    return 0;
  return pool_evrcmp_str(pool, f1 ? f1 : "" , f2 ? f2 : "", EVRCMP_COMPARE);
}

void intersect_selection(Pool* pool, Id dep, Queue* prev)
{
  Queue tmp;
  int i = 0, j = 0, isectidx = 0;

  queue_init(&tmp);

  Id* pp, p;
  pp = pool_whatprovides_ptr(pool, dep);
  while ((p = *pp++) != 0)
    queue_push(&tmp, p);

  // set intersection, assuming sorted arrays
  while (i < prev->count && j < tmp.count) 
    if (prev->elements[i] < tmp.elements[j])
      i++;
    else if (tmp.elements[j] < prev->elements[i])
      j++;
    else
      {
        if (isectidx != i)
          prev->elements[isectidx] = prev->elements[i];
        i++, j++, isectidx++;
      }

  prev->count = isectidx;
  queue_free(&tmp);
}

int check_deps_unequal(Pool* pool, Queue* q1, Queue* q2, Id name)
{
  Id dep;
  int i, j;
  int found = 0;
  for (i = 0; i < q1->count; ++i)
  {
    dep = q1->elements[i];
    if (ISRELDEP(dep) && GETRELDEP(pool, dep)->name == name)
    {
      for (j = 0; j < q2->count; ++j)
      {
        if (q2->elements[j] == dep)
        {
          found = 1;
          break;
        }
      }
      if (!found)
        return 1;

      found = 0;
    }
  }
  return 0;
}

Id best_matching(Pool* pool, Queue* q, Id name, int* all_have_trackfeatures)
{
  int first = 1;
  Id dep, p, *pp;

  Queue selection;
  queue_init(&selection);

  for (int i = 0; i < q->count; ++i)
  {
    dep = q->elements[i];
    if (!ISRELDEP(dep) || GETRELDEP(pool, dep)->name != name) continue;

    if (first)
    {
      pp = pool_whatprovides_ptr(pool, dep);
      while ((p = *pp++) != 0)
        queue_push(&selection, p);
      first = 0;
    }
    else
      intersect_selection(pool, dep, &selection);
  }

  if (selection.count == 0)
    return 0;

  Solvable *stmp, *best = pool_id2solvable(pool, selection.elements[0]);
  int cmp;

  *all_have_trackfeatures = 1;
  for (int i = 0; i < selection.count; ++i)
    if (solvable_lookup_count(pool_id2solvable(pool, selection.elements[i]),
                              SOLVABLE_TRACK_FEATURES) == 0)
      {
        *all_have_trackfeatures = 0;
        break;
      }
  
  for (int i = 0; i < selection.count; ++i)
  {
    stmp = pool_id2solvable(pool, selection.elements[i]);
    cmp = pool_evrcmp(pool, best->evr, stmp->evr, 0);
    if (cmp < 0) best = stmp;
  }

  return best->evr;
}

int conda_compare_dependencies(Pool *pool, Solvable *s1, Solvable *s2)
{
  int i, j, has_seen;
  Queue q1, q2, seen;

  queue_init(&q1);
  queue_init(&q2);
  queue_init(&seen);

  solvable_lookup_deparray(s1, SOLVABLE_REQUIRES, &q1, -1);
  solvable_lookup_deparray(s2, SOLVABLE_REQUIRES, &q2, -1);

  int comparison_result = 0;

  for (i = 0; i < q1.count; ++i)
  {
    Id x1 = q1.elements[i];
    has_seen = 0;

    if (!ISRELDEP(x1))
      continue;

    Reldep* rd1 = GETRELDEP(pool, x1);
    for (j = 0; j < seen.count && has_seen == 0; ++j)
      if (seen.elements[j] == rd1->name)
        has_seen = 1;

    if (has_seen)
      continue;

    // first make sure that deps are different between a & b
    int deps_unequal = check_deps_unequal(pool, &q1, &q2, rd1->name);
    if (!deps_unequal)
      {
        queue_push(&seen, rd1->name);
        continue;
      }

    int aht_1, aht_2; // all have track features check
    Id b1 = best_matching(pool, &q1, rd1->name, &aht_1);
    Id b2 = best_matching(pool, &q2, rd1->name, &aht_2);

    // one of both or both is not solvable...
    // ignoring this case for now
    if (b1 == 0 || b2 == 0)
      continue;

    // if one has deps with track features, and the other does not, 
    // downweight the one with track features
    if (aht_1 != aht_2)
      comparison_result += (aht_1 - aht_2) * 100;

    comparison_result += pool_evrcmp(pool, b2, b1, 0);
  }

  queue_free(&q1);
  queue_free(&q2);
  queue_free(&seen);

  return comparison_result;
}

static int
sort_by_best_dependencies(const void *ap, const void *bp, void *dp)
{
  Pool* pool = (Pool*) dp;

  Id a = *(Id *)ap;
  Id b = *(Id *)bp;
  Solvable *sa, *sb;

  sa = pool->solvables + a;
  sb = pool->solvables + b;

  int res = conda_compare_dependencies(pool, sa, sb);
  if (res == 0)
  {
    // no differences, select later build
    Repodata* ra = repo_last_repodata(sa->repo);
    Repodata* rb = repo_last_repodata(sb->repo);

    unsigned long long bta = repodata_lookup_num(ra, a, SOLVABLE_BUILDTIME, 0ull);
    unsigned long long btb = repodata_lookup_num(rb, b, SOLVABLE_BUILDTIME, 0ull);

    res = (btb > bta) ? 1 : -1;
    POOL_DEBUG(SOLV_DEBUG_POLICY, "Fallback to timestamp comparison: %llu vs %llu: [%d]\n", bta, btb, res);
  }

  POOL_DEBUG(SOLV_DEBUG_POLICY, "Selecting variant [%c] of (a) %s vs (b) %s (score: %d)\n",
             (res < 0 ? 'a' : 'b'), pool_solvable2str(pool, sa), pool_solvable2str(pool, sb), res);

  return res; 
}

/*
 * prune_to_best_version_conda
 *
 * sort list of packages (given through plist) by name and evr
 * return result through plist
 */
void
prune_to_best_version_conda(Pool *pool, Queue *plist)
{
  int i, j, r;
  Solvable *s, *best;

  if (plist->count < 2)         /* no need to prune for a single entry */
    return;
  POOL_DEBUG(SOLV_DEBUG_POLICY, "prune_to_best_version_conda %d\n", plist->count);

  /* sort by name first, prefer installed */
  solv_sort(plist->elements, plist->count, sizeof(Id), prune_to_best_version_sortcmp, pool);

  /* now find best 'per name' */
  best = 0;
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];

      POOL_DEBUG(SOLV_DEBUG_POLICY, "- %s [%d]%s\n",
                 pool_solvable2str(pool, s), plist->elements[i], 
                 (pool->installed && s->repo == pool->installed) ? "I" : "");

      if (!best)                /* if no best yet, the current is best */
        {
          best = s;
          continue;
        }

      /* name switch: finish group, re-init */
      if (best->name != s->name)   /* new name */
        {
          plist->elements[j++] = best - pool->solvables; /* move old best to front */
          best = s;             /* take current as new best */
          continue;
        }
     
      r = 0; 
      r = pool_featurecountcmp(pool, best, s);
      if (r == 0)
        r = best->evr != s->evr ? pool_evrcmp(pool, best->evr, s->evr, EVRCMP_COMPARE) : 0;
      if (r == 0)
        r = (best->repo ? best->repo->subpriority : 0) - (s->repo ? s->repo->subpriority : 0);
      if (r == 0)
        r = pool_buildversioncmp(pool, best, s);
      // this can be removed as this comparison doesn't effect anything
      if (r == 0)
        r = pool_buildflavorcmp(pool, best, s);
      if (r < 0)
        best = s;
    }

  Queue q;
  queue_init(&q);
  for (i = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      r = pool_featurecountcmp(pool, best, s);
      if (r == 0)
        r = best->evr != s->evr ? pool_evrcmp(pool, best->evr, s->evr, EVRCMP_COMPARE) : 0;
      if (r == 0)
        r = (best->repo ? best->repo->subpriority : 0) - (s->repo ? s->repo->subpriority : 0);
      if (r == 0)
        r = pool_buildversioncmp(pool, best, s);
      if (r == 0)
        queue_push(&q, s - pool->solvables);
    }

  if (q.count > 1)
    {
      // order by first-level deps
      solv_sort(q.elements, q.count, sizeof(Id), sort_by_best_dependencies, pool);
    }

  for (i = 0; i < q.count; ++i)
    plist->elements[i] = q.elements[i];
  plist->count = q.count;

  queue_free(&q);
}
#endif  // ENABLE_CONDA

static int
sort_by_name_evr_sortcmp(const void *ap, const void *bp, void *dp)
{
  Pool *pool = dp;
  Id a, *aa = (Id *)ap;
  Id b, *bb = (Id *)bp;
  Id r = aa[1] - bb[1];
  if (r)
    return r < 0 ? -1 : 1;
  if (aa[2] == bb[2])
    return 0;
  a = aa[2] < 0 ? -aa[2] : aa[2];
  b = bb[2] < 0 ? -bb[2] : bb[2];
  r = pool_evrcmp(pool, b, a, pool->disttype != DISTTYPE_DEB ? EVRCMP_MATCH_RELEASE : EVRCMP_COMPARE);
  if (!r && (aa[2] < 0 || bb[2] < 0))
    {
      if (bb[2] >= 0)
	return 1;
      if (aa[2] >= 0)
	return -1;
    }
  return r;
}

/* common end of sort_by_srcversion and sort_by_common_dep */
static void
sort_by_name_evr_array(Pool *pool, Queue *plist, int count, int ent)
{
  Id lastname;
  int i, j, bad, havebad;
  Id *pp, *elements = plist->elements;

  if (ent < 2)
    {
      queue_truncate(plist, count);
      return;
    }
  solv_sort(elements + count * 2, ent, sizeof(Id) * 3, sort_by_name_evr_sortcmp, pool);
  lastname = 0;
  bad = havebad = 0;
  for (i = 0, pp = elements + count * 2; i < ent; i++, pp += 3)
    {
      if (lastname && pp[1] == lastname)
	{
          if (pp[0] != pp[-3] && sort_by_name_evr_sortcmp(pp - 3, pp, pool) == -1)
	    {
#if 0
	      printf("%s - %s: bad %s %s - %s\n", pool_solvid2str(pool, elements[pp[-3]]), pool_solvid2str(pool, elements[pp[0]]), pool_dep2str(pool, lastname), pool_id2str(pool, pp[-1] < 0 ? -pp[-1] : pp[-1]), pool_id2str(pool, pp[2] < 0 ? -pp[2] : pp[2]));
#endif
	      bad++;
	      havebad = 1;
	    }
	}
      else
	{
	  bad = 0;
	  lastname = pp[1];
	}
      elements[count + pp[0]] += bad;
    }

#if 0
for (i = 0; i < count; i++)
  printf("%s badness %d\n", pool_solvid2str(pool, elements[i]), elements[count + i]);
#endif

  if (havebad)
    {
      /* simple stable insertion sort */
      if (pool->installed)
	for (i = 0; i < count; i++)
	  if (pool->solvables[elements[i]].repo == pool->installed)
	    elements[i + count] = 0;
      for (i = 1; i < count; i++)
	for (j = i, pp = elements + count + j; j > 0; j--, pp--)
	  if (pp[-1] > pp[0])
	    {
	      Id *pp2 = pp - count;
	      Id p = pp[-1];
	      pp[-1] = pp[0];
	      pp[0] = p;
	      p = pp2[-1];
	      pp2[-1] = pp2[0];
	      pp2[0] = p;
	    }
	  else
	    break;
    }
  queue_truncate(plist, count);
}

#if 0
static void
sort_by_srcversion(Pool *pool, Queue *plist)
{
  int i, count = plist->count, ent = 0;
  queue_insertn(plist, count, count, 0);
  for (i = 0; i < count; i++)
    {
      Id name, evr, p = plist->elements[i];
      Solvable *s = pool->solvables + p;
      if (solvable_lookup_void(s, SOLVABLE_SOURCENAME))
	name = s->name;
      else
        name = solvable_lookup_id(s, SOLVABLE_SOURCENAME);
      if (solvable_lookup_void(s, SOLVABLE_SOURCEEVR))
	evr = s->evr;
      else
        evr = solvable_lookup_id(s, SOLVABLE_SOURCEEVR);
      if (!name || !evr || ISRELDEP(evr))
	continue;
      queue_push(plist, i);
      queue_push2(plist, name, evr);
      ent++;
    }
  sort_by_name_evr_array(pool, plist, count, ent);
}
#endif

static void
sort_by_common_dep(Pool *pool, Queue *plist)
{
  int i, count = plist->count, ent = 0;
  Id id, *dp;
  queue_insertn(plist, count, count, 0);
  for (i = 0; i < count; i++)
    {
      Id p = plist->elements[i];
      Solvable *s = pool->solvables + p;
      if (!s->provides)
	continue;
      for (dp = s->repo->idarraydata + s->provides; (id = *dp++) != 0; )
	{
	  Reldep *rd;
	  if (!ISRELDEP(id))
	    continue;
	  rd = GETRELDEP(pool, id);
	  if ((rd->flags == REL_EQ || rd->flags == (REL_EQ | REL_LT) || rd->flags == REL_LT) && !ISRELDEP(rd->evr))
	    {
	      if (rd->flags == REL_EQ)
		{
		  /* ignore hashes */
		  const char *s = pool_id2str(pool, rd->evr);
		  if (strlen(s) >= 4)
		    {
		      while ((*s >= 'a' && *s <= 'f') || (*s >= '0' && *s <= '9'))
			s++;
		      if (!*s)
			continue;
		    }
		}
	      queue_push(plist, i);
	      queue_push2(plist, rd->name, rd->flags == REL_LT ? -rd->evr : rd->evr);
	      ent++;
	    }
	}
    }
  sort_by_name_evr_array(pool, plist, count, ent);
}

/* check if we have an update candidate */
static void
dislike_old_versions(Pool *pool, Queue *plist)
{
  int i, count;

  for (i = 0, count = plist->count; i < count; i++)
    {
      Id p = plist->elements[i];
      Solvable *s = pool->solvables + p;
      Repo *repo = s->repo;
      Id q, qq;
      int bad = 0;

      if (!repo || repo == pool->installed)
	continue;
      FOR_PROVIDES(q, qq, s->name)
	{
	  Solvable *qs = pool->solvables + q;
	  if (q == p)
	    continue;
	  if (s->name != qs->name || s->arch != qs->arch)
	    continue;
	  if (repo->priority != qs->repo->priority)
	    {
	      if (repo->priority > qs->repo->priority)
		continue;
	      bad = 1;
	      break;
	    }
	  if (pool_evrcmp(pool, qs->evr, s->evr, EVRCMP_COMPARE) > 0)
	    {
	      bad = 1;
	      break;
	    }
	}
      if (!bad)
	continue;
      /* bring to back */
      if (i < plist->count - 1)
	{
	  memmove(plist->elements + i, plist->elements + i + 1, (plist->count - 1 - i) * sizeof(Id));
	  plist->elements[plist->count - 1] = p;
	}
      i--;
      count--;
    }
}


/* special lang package handling for urpm */
/* see https://bugs.mageia.org/show_bug.cgi?id=18315 */

static int
urpm_reorder_cmp(const void *ap, const void *bp, void *dp)
{
  return ((Id *)bp)[1] - ((Id *)ap)[1];
}

static void
urpm_reorder(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, count = plist->count;
  /* add locale score to packages */
  queue_insertn(plist, count, count, 0);
  for (i = count - 1; i >= 0; i--)
    {
      Solvable *s = pool->solvables + plist->elements[i];
      int score = 1;
      const char *sn = pool_id2str(pool, s->name);

      if (!strncmp(sn, "kernel-", 7))
	{
	  const char *devel = strstr(sn, "-devel-");
	  if (devel && strlen(sn) < 256)
	    {
	      char kn[256];
	      Id p, pp, knid;
	      memcpy(kn, sn, devel - sn);
	      strcpy(kn + (devel - sn), devel + 6);
	      knid = pool_str2id(pool, kn, 0);
	      if (knid)
		{
		  FOR_PROVIDES(p, pp, knid)
		    {
		      if (solv->decisionmap[p] > 0)
			{
			  score = 4;
			  break;
			}
		      else if (pool->installed && pool->solvables[p].repo == pool->installed)
			score = 3;
		    }
		}
	    }
	}
      else if ((sn = strstr(sn, "-kernel-")) != 0)
	{
	  sn += 8;
	  if (strlen(sn) < 256 - 8 && *sn >= '0' && *sn <= '9' && sn[1] == '.')
	    {
	      const char *flavor = strchr(sn, '-');
	      if (flavor)
		{
		  const char *release = strchr(flavor + 1, '-');
		  if (release)
		    {
		      char kn[256];
		      Id p, pp, knid;
		      memcpy(kn, "kernel", 7);
		      memcpy(kn + 6, flavor, release - flavor + 1);
		      memcpy(kn + 6 + (release - flavor) + 1, sn, flavor - sn);
		      strcpy(kn + 6 + (release + 1 - sn), release);
		      knid = pool_str2id(pool, kn, 0);
		      if (knid)
			{
			  FOR_PROVIDES(p, pp, knid)
			    {
			      if (solv->decisionmap[p] > 0)
				{
				  score = 4;
				  break;
				}
			      if (pool->installed && pool->solvables[p].repo == pool->installed)
				score = 3;
			    }
			}
		    }
		}
	    }
	}
      if (score == 1 && s->requires)
	{
	  Id id, *idp, p, pp;
	  const char *deps;
	  for (idp = s->repo->idarraydata + s->requires; (id = *idp) != 0; idp++)
	    {
	      while (ISRELDEP(id))
		{
		  Reldep *rd = GETRELDEP(pool, id);
		  id = rd->name;
		}
	      deps = strstr(pool_id2str(pool, id), "locales-");
	      if (!deps)
		continue;
	      if (!strncmp(deps + 8, "en", 2))
		score = 2;
	      else
		{
		  score = 0;
		  FOR_PROVIDES(p, pp, id)
		    {
		      if (solv->decisionmap[p] > 0)
			{
			  score = 4;
			  break;
			}
		      if (pool->installed && pool->solvables[p].repo == pool->installed)
			score = 3;
		    }
		  break;
		}
	    }
	}
      plist->elements[i * 2] = plist->elements[i];
      plist->elements[i * 2 + 1] = score;
    }
  solv_sort(plist->elements, count, sizeof(Id) * 2, urpm_reorder_cmp, pool);
  for (i = 0; i < count; i++)
    plist->elements[i] = plist->elements[2 * i];
  queue_truncate(plist, count);
}

/* support multiple favor groups by calling policy_filter_unwanted on
 * each of them and combining the result */
static void
policy_filter_unwanted_favored(Solver *solv, Queue *plist, int mode)
{
  int i, j, f;
  Queue qin, qprune;
  queue_init_clone(&qin, plist);
  queue_empty(plist);
  /* sort by favor group */
  solv_sort(qin.elements, qin.count, sizeof(Id), sort_by_favor_cmp, solv->favormap);
  /* go over groups */
  queue_init(&qprune);
  for (i = 0; i < qin.count; i = j)
    {
      /* find end of group */
      f = solv->favormap[qin.elements[i]];
      for (j = i + 1; j < qin.count; j++)
	if (solv->favormap[qin.elements[j]] != f)
	  break;
      /* prune this group */
      queue_empty(&qprune);
      queue_insertn(&qprune, 0, j, qin.elements);
      policy_filter_unwanted(solv, &qprune, mode | POLICY_MODE_FAVOR_REC);
      for (i = 0; i < qprune.count; i++)
	if (solv->favormap[qprune.elements[i]] == f)
	  queue_push(plist, qprune.elements[i]);
    }
  queue_free(&qprune);
  queue_free(&qin);
}

/*
 *  POLICY_MODE_CHOOSE:     default, do all pruning steps
 *  POLICY_MODE_RECOMMEND:  leave out prune_to_recommended
 *  POLICY_MODE_SUGGEST:    leave out prune_to_recommended, do prio pruning just per name
 */
void
policy_filter_unwanted(Solver *solv, Queue *plist, int mode)
{
  Pool *pool = solv->pool;
  if (mode == POLICY_MODE_SUPPLEMENT)
    {
      /* reorder only */
      dislike_old_versions(pool, plist);
      sort_by_common_dep(pool, plist);
      if (solv->urpmreorder)
        urpm_reorder(solv, plist);
      prefer_suggested(solv, plist);
      policy_prefer_favored(solv, plist);
      return;
    }
  if (mode & POLICY_MODE_FAVOR_REC)
    mode ^= POLICY_MODE_FAVOR_REC;
  else if (solv->favormap && plist->count > 1)
    {
      /* check if we have multiple favor groups */
      int i, f = solv->favormap[plist->elements[0]];
      for (i = 1; i < plist->count; i++)
	if (solv->favormap[plist->elements[i]] != f)
	  break;
      if (i < plist->count)
	{
	  policy_filter_unwanted_favored(solv, plist, mode);
	  return;
	}
    }
  if (plist->count > 1)
    {
      if (mode != POLICY_MODE_SUGGEST)
        solver_prune_to_highest_prio(solv, plist);
      else
        solver_prune_to_highest_prio_per_name(solv, plist);
    }
  if (plist->count > 1)
    prune_to_best_arch(pool, plist);
  if (plist->count > 1)
    prune_to_best_version(pool, plist);
  if (plist->count > 1 && (mode == POLICY_MODE_CHOOSE || mode == POLICY_MODE_CHOOSE_NOREORDER))
    {
      prune_to_recommended(solv, plist);
      if (plist->count > 1 && mode != POLICY_MODE_CHOOSE_NOREORDER)
	{
	  /* do some fancy reordering */
#if 0
	  sort_by_srcversion(pool, plist);
#endif
	  dislike_old_versions(pool, plist);
	  sort_by_common_dep(pool, plist);
	  move_installed_to_front(pool, plist);
	  if (solv->urpmreorder)
	    urpm_reorder(solv, plist);
	  prefer_suggested(solv, plist);
	  policy_prefer_favored(solv, plist);
	}
    }
}

void
pool_best_solvables(Pool *pool, Queue *plist, int flags)
{
  if (plist->count > 1)
    prune_to_highest_prio(pool, plist);
  if (plist->count > 1)
    prune_to_best_arch(pool, plist);
  if (plist->count > 1)
    prune_to_best_version(pool, plist);
  if (plist->count > 1)
    {
      dislike_old_versions(pool, plist);
      sort_by_common_dep(pool, plist);
      move_installed_to_front(pool, plist);
    }
}


/* check if there is an illegal architecture change if
 * installed solvable s1 is replaced by s2 */
int
policy_illegal_archchange(Solver *solv, Solvable *s1, Solvable *s2)
{
  Pool *pool = solv->pool;
  Id a1 = s1->arch, a2 = s2->arch;

  /* we allow changes to/from noarch */
  if (a1 == a2 || a1 == pool->noarchid || a2 == pool->noarchid)
    return 0;
  if (!pool->id2arch)
    return 0;
  a1 = pool_arch2score(pool, a1);
  a2 = pool_arch2score(pool, a2);
  if (((a1 ^ a2) & 0xffff0000) != 0)
    return 1;
  return 0;
}

/* check if there is an illegal vendor change if
 * installed solvable s1 is replaced by s2 */
int
policy_illegal_vendorchange(Solver *solv, Solvable *s1, Solvable *s2)
{
  Pool *pool = solv->pool;
  Id v1, v2;
  Id vendormask1, vendormask2;

  if (pool->custom_vendorcheck)
     return pool->custom_vendorcheck(pool, s1, s2);

  /* treat a missing vendor as empty string */
  v1 = s1->vendor ? s1->vendor : ID_EMPTY;
  v2 = s2->vendor ? s2->vendor : ID_EMPTY;
  if (v1 == v2)
    return 0;
  vendormask1 = pool_vendor2mask(pool, v1);
  if (!vendormask1)
    return 1;	/* can't match */
  vendormask2 = pool_vendor2mask(pool, v2);
  if ((vendormask1 & vendormask2) != 0)
    return 0;
  return 1;	/* no class matches */
}

/* check if it is illegal to replace installed
 * package "is" with package "s" (which must obsolete "is")
 */
int
policy_is_illegal(Solver *solv, Solvable *is, Solvable *s, int ignore)
{
  Pool *pool = solv->pool;
  int ret = 0;
  int duppkg = solv->dupinvolvedmap_all || (solv->dupinvolvedmap.size && MAPTST(&solv->dupinvolvedmap, is - pool->solvables));
  if (!(ignore & POLICY_ILLEGAL_DOWNGRADE) && !(duppkg ? solv->dup_allowdowngrade : solv->allowdowngrade))
    {
      if (is->name == s->name && pool_evrcmp(pool, is->evr, s->evr, EVRCMP_COMPARE) > 0)
	ret |= POLICY_ILLEGAL_DOWNGRADE;
    }
  if (!(ignore & POLICY_ILLEGAL_ARCHCHANGE) && !(duppkg ? solv->dup_allowarchchange : solv->allowarchchange))
    {
      if (is->arch != s->arch && policy_illegal_archchange(solv, is, s))
	ret |= POLICY_ILLEGAL_ARCHCHANGE;
    }
  if (!(ignore & POLICY_ILLEGAL_VENDORCHANGE) && !(duppkg ? solv->dup_allowvendorchange : solv->allowvendorchange))
    {
      if (is->vendor != s->vendor && policy_illegal_vendorchange(solv, is, s))
	ret |= POLICY_ILLEGAL_VENDORCHANGE;
    }
  if (!(ignore & POLICY_ILLEGAL_NAMECHANGE) && !(duppkg ? solv->dup_allownamechange : solv->allownamechange))
    {
      if (is->name != s->name)
	ret |= POLICY_ILLEGAL_NAMECHANGE;
    }
  return ret;
}

/*-------------------------------------------------------------------
 *
 * create reverse obsoletes map for installed solvables
 *
 * For each installed solvable find which packages with *different* names
 * obsolete the solvable.
 * This index is used in policy_findupdatepackages() below.
 */
void
policy_create_obsolete_index(Solver *solv)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Repo *installed = solv->installed;
  Id p, pp, obs, *obsp, *obsoletes, *obsoletes_data;
  int i, n, cnt;

  solv->obsoletes = solv_free(solv->obsoletes);
  solv->obsoletes_data = solv_free(solv->obsoletes_data);
  if (!installed || installed->start == installed->end)
    return;
  cnt = installed->end - installed->start;
  solv->obsoletes = obsoletes = solv_calloc(cnt, sizeof(Id));
  for (i = 1; i < pool->nsolvables; i++)
    {
      s = pool->solvables + i;
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      Solvable *ps = pool->solvables + p;
	      if (ps->repo != installed)
		continue;
	      if (ps->name == s->name)
		continue;
	      if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
		continue;
	      if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
		continue;
	      obsoletes[p - installed->start]++;
	    }
	}
    }
  n = 0;
  for (i = 0; i < cnt; i++)
    if (obsoletes[i])
      {
        n += obsoletes[i] + 1;
        obsoletes[i] = n;
      }
  solv->obsoletes_data = obsoletes_data = solv_calloc(n + 1, sizeof(Id));
  POOL_DEBUG(SOLV_DEBUG_STATS, "obsoletes data: %d entries\n", n + 1);
  for (i = pool->nsolvables - 1; i > 0; i--)
    {
      s = pool->solvables + i;
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      Solvable *ps = pool->solvables + p;
	      if (ps->repo != installed)
		continue;
	      if (ps->name == s->name)
		continue;
	      if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
		continue;
	      if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
		continue;
	      if (obsoletes_data[obsoletes[p - installed->start]] != i)
		obsoletes_data[--obsoletes[p - installed->start]] = i;
	    }
	}
    }
}


/* return true if solvable s obsoletes solvable with id pi */
static inline int
is_obsoleting(Pool *pool, Solvable *s, Id pi)
{
  Id p, pp, obs, *obsp;
  Solvable *si = pool->solvables + pi;
  if (pool->obsoleteusescolors && !pool_colormatch(pool, si, s))
    return 0;
  obsp = s->repo->idarraydata + s->obsoletes;
  while ((obs = *obsp++) != 0)	/* for all obsoletes */
    {
      FOR_PROVIDES(p, pp, obs)   /* and all matching providers of the obsoletes */
	{
	  if (p != pi)
	    continue;
	  if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, si, obs))
	    continue;
	  return 1;
	}
    }
  return 0;
}

/*
 * find update candidates
 *
 * s: installed solvable to be updated
 * qs: [out] queue to hold Ids of candidates
 * allow_all: 0 = dont allow downgrades, 1 = allow all candidates
 *            2 = dup mode
 *
 */
void
policy_findupdatepackages(Solver *solv, Solvable *s, Queue *qs, int allow_all)
{
  /* installed packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, pp, n;
  Solvable *ps;
  int haveprovobs = 0;
  int allowdowngrade = allow_all ? 1 : solv->allowdowngrade;
  int allownamechange = allow_all ? 1 : solv->allownamechange;
  int allowarchchange = allow_all ? 1 : solv->allowarchchange;
  int allowvendorchange = allow_all ? 1 : solv->allowvendorchange;
  if (allow_all == 2)
    {
      allowdowngrade = solv->dup_allowdowngrade;
      allownamechange = solv->dup_allownamechange;
      allowarchchange = solv->dup_allowarchchange;
      allowvendorchange = solv->dup_allowvendorchange;
    }

  queue_empty(qs);

  n = s - pool->solvables;

  /*
   * look for updates for s
   */
  FOR_PROVIDES(p, pp, s->name)	/* every provider of s' name */
    {
      if (p == n)		/* skip itself */
	continue;

      ps = pool->solvables + p;
      if (pool->considered && pool->whatprovideswithdisabled && ps->repo != pool->installed && pool_disabled_solvable(pool, ps)) 
	continue;
      if (s->name == ps->name)	/* name match */
	{
	  if (pool->implicitobsoleteusescolors && !pool_colormatch(pool, s, ps))
	    continue;
	  if (!allowdowngrade && pool_evrcmp(pool, s->evr, ps->evr, EVRCMP_COMPARE) > 0)
	    continue;
	}
      else if (!allownamechange)
	continue;
      else if ((!solv->noupdateprovide || solv->needupdateprovide) && ps->obsoletes)   /* provides/obsoletes combination ? */
	{
	  /* check if package ps that provides s->name obsoletes installed package s */
	  /* implicitobsoleteusescolors is somewhat wrong here, but we nevertheless
	   * use it to limit our update candidates */
	  if (pool->implicitobsoleteusescolors && !pool_colormatch(pool, s, ps))
	    continue;
	  if (!is_obsoleting(pool, ps, n))
	    continue;
	  haveprovobs = 1;		/* have matching provides/obsoletes combination */
	}
      else
        continue;
      if (!allowarchchange && s->arch != ps->arch && policy_illegal_archchange(solv, s, ps))
	continue;
      if (!allowvendorchange && s->vendor != ps->vendor && policy_illegal_vendorchange(solv, s, ps))
	continue;
      queue_push(qs, p);
    }
  if (!allownamechange)
    return;
  /* if we have found some valid candidates and noupdateprovide is not set, we're
     done. otherwise we fallback to all obsoletes */
  if (solv->needupdateprovide || (!solv->noupdateprovide && haveprovobs))
    return;
  if (solv->obsoletes && solv->obsoletes[n - solv->installed->start])
    {
      Id *opp;
      for (opp = solv->obsoletes_data + solv->obsoletes[n - solv->installed->start]; (p = *opp++) != 0;)
	{
	  ps = pool->solvables + p;
	  /* implicitobsoleteusescolors is somewhat wrong here, but we nevertheless
	   * use it to limit our update candidates */
	  if (pool->implicitobsoleteusescolors && !pool_colormatch(pool, s, ps))
	    continue;
	  if (!allowarchchange && s->arch != ps->arch && policy_illegal_archchange(solv, s, ps))
	    continue;
	  if (!allowvendorchange && s->vendor != ps->vendor && policy_illegal_vendorchange(solv, s, ps))
	    continue;
	  queue_push(qs, p);
	}
    }
}

const char *
policy_illegal2str(Solver *solv, int illegal, Solvable *s, Solvable *rs)
{
  Pool *pool = solv->pool;
  const char *str;
  if (illegal == POLICY_ILLEGAL_DOWNGRADE)
    {
      str = pool_tmpjoin(pool, "downgrade of ", pool_solvable2str(pool, s), 0);
      return pool_tmpappend(pool, str, " to ", pool_solvable2str(pool, rs));
    }
  if (illegal == POLICY_ILLEGAL_NAMECHANGE)
    {
      str = pool_tmpjoin(pool, "name change of ", pool_solvable2str(pool, s), 0);
      return pool_tmpappend(pool, str, " to ", pool_solvable2str(pool, rs));
    }
  if (illegal == POLICY_ILLEGAL_ARCHCHANGE)
    {
      str = pool_tmpjoin(pool, "architecture change of ", pool_solvable2str(pool, s), 0);
      return pool_tmpappend(pool, str, " to ", pool_solvable2str(pool, rs));
    }
  if (illegal == POLICY_ILLEGAL_VENDORCHANGE)
    {
      str = pool_tmpjoin(pool, "vendor change from '", pool_id2str(pool, s->vendor), "' (");
      if (rs->vendor)
	{
          str = pool_tmpappend(pool, str, pool_solvable2str(pool, s), ") to '");
          str = pool_tmpappend(pool, str, pool_id2str(pool, rs->vendor), "' (");
	}
      else
        str = pool_tmpappend(pool, str, pool_solvable2str(pool, s), ") to no vendor (");
      return pool_tmpappend(pool, str, pool_solvable2str(pool, rs), ")");
    }
  return "unknown illegal change";
}

