/*
 * Copyright (c) 2002 Matteo Frigo
 * Copyright (c) 2002 Steven G. Johnson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: buffered.c,v 1.19 2002-09-25 00:08:44 athena Exp $ */

#include "rdft.h"

typedef struct {
     uint nbuf;
     uint maxbufsz;
     uint skew_alignment;
     uint skew;
     const char *nam;
} bufadt;

typedef struct {
     solver super;
     const bufadt *adt;
} S;

typedef struct {
     plan_rdft super;

     plan *cld, *cldcpy, *cldrest;
     uint n, vl, nbuf, bufdist;
     int ivs, ovs;

     const S *slv;
} P;

/* transform a vector input with the help of bufs */
static void apply(plan *ego_, R *I, R *O)
{
     P *ego = (P *) ego_;
     plan_rdft *cld = (plan_rdft *) ego->cld;
     plan_rdft *cldcpy = (plan_rdft *) ego->cldcpy;
     plan_rdft *cldrest;
     uint i, vl = ego->vl, nbuf = ego->nbuf;
     int ivs = ego->ivs, ovs = ego->ovs;
     R *bufs;

     bufs = (R *)fftw_malloc(sizeof(R) * nbuf * ego->bufdist, BUFFERS);

     for (i = nbuf; i <= vl; i += nbuf) {
          /* transform to bufs: */
          cld->apply((plan *) cld, I, bufs);
	  I += ivs;

          /* copy back */
          cldcpy->apply((plan *) cldcpy, bufs, O);
	  O += ovs;
     }

     /* Do the remaining transforms, if any: */
     cldrest = (plan_rdft *) ego->cldrest;
     cldrest->apply((plan *) cldrest, I, O);

     X(free)(bufs);
}


static void awake(plan *ego_, int flg)
{
     P *ego = (P *) ego_;

     AWAKE(ego->cld, flg);
     AWAKE(ego->cldcpy, flg);
     AWAKE(ego->cldrest, flg);
}

static void destroy(plan *ego_)
{
     P *ego = (P *) ego_;
     X(plan_destroy)(ego->cldrest);
     X(plan_destroy)(ego->cldcpy);
     X(plan_destroy)(ego->cld);
}

static void print(plan *ego_, printer *p)
{
     P *ego = (P *) ego_;
     p->print(p, "(%s-%u%v/%u-%u%(%p%)%(%p%)%(%p%))",
              ego->slv->adt->nam,
              ego->n, ego->nbuf,
              ego->vl, ego->bufdist % ego->n,
              ego->cld, ego->cldcpy, ego->cldrest);
}


static uint compute_nbuf(uint n, uint vl, const S *ego)
{
     return X(compute_nbuf)(n, vl, ego->adt->nbuf, ego->adt->maxbufsz);
}

static int toobig(uint n, const S *ego)
{
     return (n > ego->adt->maxbufsz);
}

static int applicable0(const problem *p_, const S *ego, const planner *plnr)
{
     if (RDFTP(p_)) {
          const problem_rdft *p = (const problem_rdft *) p_;
          iodim *d = p->sz->dims;

          if (1
	      && p->vecsz->rnk <= 1
	      && p->sz->rnk == 1
	       ) {

	       if (toobig(p->sz->dims[0].n, ego) && CONSERVE_MEMORYP(plnr))
		    return 0;

               /*
		 In principle, the buffered transforms might be useful
		 when working out of place.  However, in order to
		 prevent infinite loops in the planner, we require
		 that the output stride of the buffered transforms be
		 greater than 1.
               */
               if (p->I != p->O)
                    return (d[0].os > 1);

               /* We can always do a single transform in-place */
               if (p->vecsz->rnk == 0)
                    return 1;

               /*
		* If the problem is in place, the input/output strides must
		* be the same or the whole thing must fit in the buffer.
		*/
               return ((X(tensor_inplace_strides2)(p->sz, p->vecsz))
                       || (compute_nbuf(d[0].n, p->vecsz->dims[0].n, ego)
                           == p->vecsz->dims[0].n));
          }
     }
     return 0;
}

static int applicable(const problem *p_, const S *ego, const planner *plnr)
{
     const problem_rdft *p;

     if (NO_BUFFERINGP(plnr)) return 0;
     if (!applicable0(p_, ego, plnr)) return 0;

     p = (const problem_rdft *) p_;
     if (NO_UGLYP(plnr)) {
	  if (p->I != p->O) return 0;
	  if (toobig(p->sz->dims[0].n, ego)) return 0;
     }
     return 1;
}

static plan *mkplan(const solver *ego_, const problem *p_, planner *plnr)
{
     const S *ego = (const S *) ego_;
     const bufadt *adt = ego->adt;
     P *pln;
     plan *cld = (plan *) 0;
     plan *cldcpy = (plan *) 0;
     plan *cldrest = (plan *) 0;
     const problem_rdft *p = (const problem_rdft *) p_;
     R *bufs = (R *) 0;
     uint nbuf = 0, bufdist, n, vl;
     int ivs, ovs;

     static const plan_adt padt = {
	  X(rdft_solve), awake, print, destroy
     };


     if (!applicable(p_, ego, plnr))
          goto nada;

     n = X(tensor_sz)(p->sz);
     vl = X(tensor_sz)(p->vecsz);

     nbuf = compute_nbuf(n, vl, ego);
     A(nbuf > 0);

     /*
      * Determine BUFDIST, the offset between successive array bufs.
      * bufdist = n + skew, where skew is chosen such that bufdist %
      * skew_alignment = skew.
      */
     if (vl == 1) {
          bufdist = n;
          ivs = ovs = 0;
     } else {
          bufdist =
               n + ((adt->skew_alignment + adt->skew - n % adt->skew_alignment)
                    % adt->skew_alignment);
          A(p->vecsz->rnk == 1);
          ivs = p->vecsz->dims[0].is;
          ovs = p->vecsz->dims[0].os;
     }

     /* initial allocation for the purpose of planning */
     bufs = (R *) fftw_malloc(sizeof(R) * nbuf * bufdist, BUFFERS);

     cld = X(mkplan_d)(plnr, 
		       X(mkproblem_rdft_d)(
			    X(mktensor_1d)(n, p->sz->dims[0].is, 1),
			    X(mktensor_1d)(nbuf, ivs, bufdist),
			    p->I, bufs, p->kind));
     if (!cld) goto nada;

     /* copying back from the buffer is a rank-0 transform: */
     cldcpy = X(mkplan_d)(plnr, 
			  X(mkproblem_rdft_d)(
			       X(mktensor_0d)(),
			       X(mktensor_2d)(nbuf, bufdist, ovs,
					      X(rdft_real_n)(p->kind[0], n),
					      1, p->sz->dims[0].os),
			       bufs, p->O, (rdft_kind *) 0));
     if (!cldcpy) goto nada;

     /* deallocate buffers, let apply() allocate them for real */
     X(free)(bufs);
     bufs = 0;

     /* plan the leftover transforms (cldrest): */
     cldrest = X(mkplan_d)(plnr, 
			   X(mkproblem_rdft_d)(
				X(tensor_copy)(p->sz),
				X(mktensor_1d)(vl % nbuf, ivs, ovs),
				p->I, p->O, p->kind));
     if (!cldrest) goto nada;

     pln = MKPLAN_RDFT(P, &padt, apply);
     pln->cld = cld;
     pln->cldcpy = cldcpy;
     pln->cldrest = cldrest;
     pln->slv = ego;
     pln->n = n;
     pln->vl = vl;
     pln->ivs = ivs * nbuf;
     pln->ovs = ovs * nbuf;

     pln->nbuf = nbuf;
     pln->bufdist = bufdist;

     {
	  opcnt t;
	  X(ops_add)(&cld->ops, &cldcpy->ops, &t);
	  X(ops_madd)(vl / nbuf, &t, &cldrest->ops, &pln->super.super.ops);
     }

     return &(pln->super.super);

 nada:
     if (bufs)
          X(free)(bufs);
     X(plan_destroy)(cldrest);
     X(plan_destroy)(cldcpy);
     X(plan_destroy)(cld);
     return (plan *) 0;
}

static solver *mksolver(const bufadt *adt)
{
     static const solver_adt sadt = { mkplan };
     S *slv = MKSOLVER(S, &sadt);
     slv->adt = adt;
     return &(slv->super);
}


void X(rdft_buffered_register)(planner *p)
{
     /* FIXME: what are good defaults? */
     static const bufadt adt = {
	  /* nbuf */           8,
	  /* maxbufsz */       (65536 / sizeof(R)),
	  /* skew_alignment */ 8,
	  /* skew */           5,
	  /* nam */            "rdft-buffered"
     };

     REGISTER_SOLVER(p, mksolver(&adt));
}
