#include <complex.h> // This is provided by GCC.
#include <math.h>

#include <Complex.h> // This is provided by ScaleME.

#include "fsgreen.h"
#include "integrate.h"
#include "mlfma.h"

fmadesc fmaconf;

static complex float genpat (int gi, float *cen, float *s) {
	float sc, sr, rv[3];
	complex float val;

	bscenter (gi, rv);

	sc = s[0] * cen[0] + s[1] * cen[1] + s[2] * cen[2];
	sr = s[0] * rv[0] + s[1] * rv[1] + s[2] * rv[2];

	val = cexp (I * fmaconf.k0 * sc) * cexp (-I * fmaconf.k0 * sr);
	val *= fmaconf.cell[0] * fmaconf.cell[1] * fmaconf.cell[2];

	return val;
}

/* Compute the radiation pattern of a square cell. The formulation assumes
 * the Green's function does not have a 4 pi term in the denominator. */
void radpattern (int gi, float *cen, float *s, Complex *ans) {
	complex float val;

	/* The k0 term is due to the limit of the radiation pattern. */
	val = fmaconf.k0 * genpat (gi, cen, s);

	/* Copy the value into the solution. */
	ans->re = creal (val);
	ans->im = cimag (val);
}

/* The receiving pattern is the conjugate of the radiation pattern. */
void rcvpattern (int gi, float *cen, float *s, Complex *ans) {
	complex float val;

	val = conj (genpat (gi, cen, s));
	/* Include constants outside the integral. The 4 pi factor is included
	 * because it was neglected in radiation pattern construction. */
	val *= fmaconf.k0 * fmaconf.k0 / (4 * M_PI);

	ans->re = creal (val);
	ans->im = cimag (val);
}

/* Direct interactions between two global basis indices. */
void impedance (int gi, int gj, Complex *ans) {
	float src[3], obs[3];
	complex float val;

	/* This is the self term, so use the analytic approximation. */
	if (gi == gj) val = selfint (fmaconf.k0, fmaconf.cell);
	else { 
		/* Find the source and observation centers. */
		bscenter (gi, obs);
		bscenter (gj, src); 
		
		/* Compute the near-neighbor term, using Gaussian quadrature. */
		val = srcint (fmaconf.k0, src, obs, fmaconf.cell);
	}

	/* The factor of (1 / 4 pi) is included in the Green's function
	 * representations. The k0 term is outside the integral, so include
	 * it here. */
	val *= fmaconf.k0 * fmaconf.k0;
	
	/* Copy the value into the solution. */
	ans->re = creal (val);
	ans->im = cimag (val);
	return;
}

/* Find the center of the basis function referred to by the global index. */
void bscenter (int gi, float *cen) {
	int i, j, k;

	i = gi % fmaconf.nx;
	j = (gi / fmaconf.nx) % fmaconf.ny;
	k = gi / (fmaconf.nx * fmaconf.ny);

	cen[0] = fmaconf.min[0] + ((float)i + 0.5) * fmaconf.cell[0];
	cen[1] = fmaconf.min[1] + ((float)j + 0.5) * fmaconf.cell[1];
	cen[2] = fmaconf.min[2] + ((float)k + 0.5) * fmaconf.cell[2];
}
