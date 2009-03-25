#include <stdlib.h>
#include <math.h>
#include <complex.h>
#include <string.h>

#include <mpi.h>

#include "frechet.h"
#include "measure.h"
#include "excite.h"
#include "mlfma.h"
#include "cg.h"

/* Least-squares solution using CG. The residual must be precomputed. */
float cgls (complex float *rn, complex float *sol, solveparm *slv,
		measdesc *src, measdesc *obs, float regparm) {
	int i, j, k;
	complex float *ifld, *pn, *scat, *adjcrt;
	float rnorm, pnorm, lrnorm, lpnorm, alpha, beta, rninc;

	ifld = malloc (3 * fmaconf.numbases * sizeof(complex float));
	pn = ifld + fmaconf.numbases;
	adjcrt = pn + fmaconf.numbases;

	scat = malloc (obs->count * sizeof(complex float));

	/* Initialize some values. */
	lrnorm = 0;
	for (i = 0; i < fmaconf.numbases; ++i) {
		pn[i] = rn[i];
		sol[i] = 0.0;
		alpha = cabs (rn[i]);
		lrnorm += alpha * alpha;
	}

	/* Reduce the local norms into a global norm. */
	MPI_Allreduce (&lrnorm, &rnorm, 1, MPI_FLOAT, MPI_SUM, MPI_COMM_WORLD);
	pnorm = (rninc = rnorm);

	for (j = 0; j < slv->maxit; ++j) {
		memset (adjcrt, 0, fmaconf.numbases * sizeof(complex float));
		alpha = 0;
		for (i = 0; i < src->count; ++i) {
			/* Build the incident field. */
			buildrhs (ifld, src->locations + 3 * i);
			/* Solve for the internal field. */
			cgmres (ifld, ifld, 1, slv);
			/* Compute the Frechet derivative. */
			frechet (pn, ifld, scat, obs, slv);
			/* Compute the adjoint Frechet derivative. */
			frechadj (scat, ifld, adjcrt, obs, slv);
			/* Update the local norm. */
			for (k = 0; k < obs->count; ++k) {
				beta = cabs (scat[k]);
				alpha += beta * beta;
			}
		}

		alpha  = rnorm / (alpha + regparm * pnorm);
		beta = rnorm;

		/* Update the residual. */
		for (i = 0, lrnorm = 0; i < fmaconf.numbases; ++i) {
			rn[i] -= alpha * (adjcrt[i] + regparm * pn[i]);
			rnorm = cabs (rn[i]);
			lrnorm += rnorm * rnorm;
		}

		/* Reduce the new residual norm. */
		MPI_Allreduce (&lrnorm, &rnorm, 1, MPI_FLOAT, MPI_SUM, MPI_COMM_WORLD);

		beta = rnorm / beta;

		pnorm = lpnorm = 0;

		/* Update the solution and the search vector. */
		for (i = 0, lpnorm = 0; i < fmaconf.numbases; ++i) {
			sol[i] += alpha * pn[i];
			pn[i] = rn[i] + beta * pn[i];
			pnorm = cabs(pn[i]);
			lpnorm += pnorm * pnorm;
		}

		/* Reduce the new search norm. */
		MPI_Allreduce (&lpnorm, &pnorm, 1, MPI_FLOAT, MPI_SUM, MPI_COMM_WORLD);

		if (sqrt(rnorm / rninc) < slv->epscg) break;
	}

	free (ifld);
	free (scat);

	return sqrt(rnorm / rninc);
}

float cgmn (complex float *rhs, complex float *sol, solveparm *slv,
		measdesc *src, measdesc *obs, float regparm) {
	int i, j, nmeas;
	complex float *ifld, *mptr, *rn, *pn, *scat, *adjcrt, *asol;
	float rnorm, pnorm, lrnorm, alpha, beta, rninc;

	nmeas = src->count * obs->count;

	ifld = malloc (2 * fmaconf.numbases * sizeof(complex float));
	adjcrt = ifld + fmaconf.numbases; 

	scat = malloc (4 * nmeas * sizeof(complex float));
	rn = scat + nmeas;
	pn = rn + nmeas;
	asol = pn + nmeas;

	/* Initialize some values. */
	rnorm = 0;
	for (i = 0; i < nmeas; ++i) {
		pn[i] = rn[i] = rhs[i];
		alpha = cabs (rhs[i]);
		rnorm += alpha * alpha;
		asol[i] = 0;
	}

	pnorm = (rninc = rnorm);

	for (j = 0; j < slv->maxit; ++j) {
		alpha = 0;
		memset (adjcrt, 0, fmaconf.numbases * sizeof(complex float));
		for (i = 0, mptr = pn; i < src->count; ++i, mptr += obs->count) {
			/* Build the incident field. */
			buildrhs (ifld, src->locations + 3 * i);
			/* Solve for the internal field. */
			cgmres (ifld, ifld, 1, slv);
			/* Compute the adjoint Frechet derivative. */
			frechadj (mptr, ifld, adjcrt, obs, slv);
		}

		for (i = 0, lrnorm = 0; i < fmaconf.numbases; ++i) { 
			alpha = cabs (adjcrt[i]);
			lrnorm += alpha * alpha;
		}

		MPI_Allreduce (&lrnorm, &alpha, 1, MPI_FLOAT, MPI_SUM, MPI_COMM_WORLD);

		alpha  = rnorm / (alpha + regparm * pnorm);
		beta = rnorm;

		for (i = 0, mptr = scat; i < src->count; ++i, mptr += obs->count) {
			/* Build the incident field. */
			buildrhs (ifld, src->locations + 3 * i);
			/* Solve for the internal field. */
			cgmres (ifld, ifld, 1, slv);
			/* Compute the adjoint Frechet derivative. */
			frechet (adjcrt, ifld, mptr, obs, slv);
		}

		/* Update the residual. */
		for (i = 0, rnorm = 0; i < nmeas; ++i) {
			rn[i] -= alpha * (scat[i] + regparm * pn[i]);
			lrnorm = cabs (rn[i]);
			rnorm += lrnorm * lrnorm;
		}

		beta = rnorm / beta;

		/* Update the solution and the search vector. */
		for (i = 0, pnorm = 0; i < nmeas; ++i) {
			asol[i] += alpha * pn[i];
			pn[i] = rn[i] + beta * pn[i];
			lrnorm = cabs(pn[i]);
			pnorm += lrnorm * lrnorm;
		}

		if (sqrt(rnorm / rninc) < slv->epscg) break;
	}

	memset (sol, 0, fmaconf.numbases * sizeof(complex float));
	/* Compute the adjoint acting on the RHS. */
	for (i = 0, mptr = asol; i < src->count; ++i, mptr += obs->count) {
		/* Build the incident field. */
		buildrhs (ifld, src->locations + 3 * i);
		/* Solve for the internal field. */
		cgmres (ifld, ifld, 1, slv);
		/* The contribution to the adjoint Frechet derivative. */
		frechadj (mptr, ifld, sol, obs, slv);
	}

	free (ifld);
	free (scat);

	return sqrt(rnorm / rninc);
}
