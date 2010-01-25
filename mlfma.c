#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <complex.h>
#include <math.h>

#include <fftw3.h>

#include <mpi.h>

#include <omp.h>

#ifdef _MACOSX
#include <Accelerate/Accelerate.h>
#endif /* _MACOSX */

#ifdef _FREEBSD
#include <cblas.h>
#endif /* _FREEBSD */

#include "ScaleME.h"

#include "fsgreen.h"
#include "integrate.h"
#include "itsolver.h"
#include "mlfma.h"

#define UNSGN(x) ((x) > 0 ? (x) : -(x))
#define IDX(nx,i,j,k) ((k) + (nx) * ((j) + (nx) * (i)))

/* Define some no-op OpenMP functions if OpenMP is not used. */
#ifndef _OPENMP
int omp_get_max_threads () { return 1; }
int omp_get_thread_num () { return 0; }
#endif /* _OPENMP */

fmadesc fmaconf;

/* Some often-used buffers that shouldn't be continually reallocated. */
complex float *patbuf, *dirbuf;
int totbpnbr, nfft[3], nfftprod;

/* Computes the far-field pattern for the specified basis with the specified
 * center, and stores the output in a provided vector. sgn is positive for
 * radiation pattern and negative for receiving pattern. */
void farpattern (int nbs, int *bsl, void *vcrt, void *vpat, float *cen, int sgn) {
	float fbox[3];
	complex float fact, beta, *buf, *crt = (complex float *)vcrt,
		*pat = *((complex float **)vpat);
	int l, idx[3], bsoff[3];

	/* The buffer that will be used by this thread for this routine. */
	buf = patbuf + omp_get_thread_num() * fmaconf.bspboxvol;
	memset (buf, 0, fmaconf.bspboxvol * sizeof(complex float));

	fbox[0] = (cen[0] - fmaconf.min[0]) / (fmaconf.cell * fmaconf.bspbox);
	fbox[1] = (cen[1] - fmaconf.min[1]) / (fmaconf.cell * fmaconf.bspbox);
	fbox[2] = (cen[2] - fmaconf.min[2]) / (fmaconf.cell * fmaconf.bspbox);

	bsoff[0] = (int)(fbox[0]) * fmaconf.bspbox;
	bsoff[1] = (int)(fbox[1]) * fmaconf.bspbox;
	bsoff[2] = (int)(fbox[2]) * fmaconf.bspbox;

	if (sgn >= 0) {
		/* Scalar factors for the matrix multiplication. */
		fact = fmaconf.k0 * fmaconf.cellvol;
		beta = 1.0;

		/* Compute the far-field pattern for the basis functions. */
		for (l = 0; l < nbs; ++l) {
			/* Find the basis grid position. */
			bsindex (bsl[l], idx);

			/* Shift to a local grid position. */
			idx[0] -= bsoff[0];
			idx[1] -= bsoff[1];
			idx[2] -= bsoff[2];
			
			/* Augment the current vector. */
			buf[IDX(fmaconf.bspbox,idx[0],idx[1],idx[2])] = crt[l];
		}

		/* Perform the matrix-vector product. */
		cblas_cgemv (CblasColMajor, CblasNoTrans, fmaconf.nsamp,
				fmaconf.bspboxvol, &fact, fmaconf.radpats, 
				fmaconf.nsamp, buf, 1, &beta, pat, 1);
	} else {
		/* Distribute the far-field patterns to the basis functions. */
		/* Scalar factors for the matrix multiplication. */
		fact = I * fmaconf.k0 * fmaconf.k0 / (4 * M_PI);
		beta = 0.0;

		/* Perform the matrix-vector product. */
		cblas_cgemv (CblasColMajor, CblasConjTrans, fmaconf.nsamp,
				fmaconf.bspboxvol, &fact, fmaconf.radpats,
				fmaconf.nsamp, pat, 1, &beta, buf, 1);

		for (l = 0; l < nbs; ++l) {
			/* Find the basis grid position. */
			bsindex (bsl[l], idx);
			
			/* Shift to a local grid position. */
			idx[0] -= bsoff[0];
			idx[1] -= bsoff[1];
			idx[2] -= bsoff[2];
			
			/* Augment the current vector. */
			crt[l] += buf[IDX(fmaconf.bspbox,idx[0],idx[1],idx[2])];
		}
	}
}

/* Precompute the exponential radiation pattern for a point a distance rmc 
 * from the center of the parent box. */
int buildradpat (complex float *pat, float k, float *rmc,
		float *thetas, int ntheta, int nphi) {
	int i, j, nthsc = ntheta - 1;
	float s[3], sdr, dphi = 2 * M_PI / nphi, sn, phi;

	/* South pole first. */
	sdr = -rmc[2];
	*(pat++) = cexp (-I * k * sdr);

	for (i = 1; i < nthsc; ++i) {
		s[2] = thetas[i];
		sn = sin (acos (thetas[i]));

		for (j = 0, phi = 0; j < nphi; ++j, phi += dphi) {
			s[0] = sn * cos (phi);
			s[1] = sn * sin (phi);
			sdr = s[0] * rmc[0] + s[1] * rmc[1] + s[2] * rmc[2];
			*(pat++) = cexp (-I * k * sdr);
		}
	}

	/* North pole last. */
	sdr = rmc[2];
	*pat = cexp (-I * k * sdr);

	return ntheta;
}

/* Build the extended Green's function on an expanded cubic grid. */
int greengrid (complex float *grf, int m, int mex, float k0, float cell, int *off) {
	int i, j, k, ip, jp, kp;
	float dist[3], zero[3] = {0., 0., 0.}, scale;

	/* The scale of the integral equation solution. */
	scale = k0 * k0 / (float)(mex * mex * mex);

	/* Compute the interactions. */
	for (i = 0; i < mex; ++i) {
		ip = (i < m) ? i : (i - mex);
		dist[0] = (float)(ip - off[0]) * fmaconf.cell;
		for (j = 0; j < mex; ++j) {
			jp = (j < m) ? j : (j - mex);
			dist[1] = (float)(jp - off[1]) * fmaconf.cell;
			for (k = 0; k < mex; ++k) {
				kp = (k < m) ? k : (k - mex);
				dist[2] = (float)(kp - off[2]) * fmaconf.cell;

				/* Handle the self term specially. */
				if (kp == off[2] && jp == off[1] && ip == off[0])
					*(grf++) = selfint (k0, cell) / (mex * mex * mex);
				else *(grf++) = scale * srcint (k0, zero, dist, cell);
			}
		}
	}

	return mex;
}

/* Precomputes the near interactions for redundant calculations and sets up
 * the wave vector directions to be used for fast calculation of far-field patterns. */
int fmmprecalc () {
	float *thetas, clen;
	int ntheta, nphi;

	/* Get the finest level parameters. */
	ScaleME_getFinestLevelParams (&(fmaconf.nsamp), &ntheta, &nphi, NULL, NULL);
	/* Allocate the theta array. */
	thetas = malloc (ntheta * sizeof(float));
	/* Populate the theta array. */
	ScaleME_getFinestLevelParams (&(fmaconf.nsamp), &ntheta, &nphi, thetas, NULL);

	/* Allocate storage for the radiation patterns. */
	fmaconf.radpats = malloc (fmaconf.bspboxvol * (fmaconf.nsamp + 
				omp_get_max_threads()) * sizeof(complex float));
	patbuf = fmaconf.radpats + fmaconf.bspboxvol * fmaconf.nsamp;

	/* Calculate the box center. */
	clen = 0.5 * (float)fmaconf.bspbox;

#pragma omp parallel default(shared)
{
	int i, j, k, l;
	complex float *pptr;
	float dist[3];

#pragma omp for
	for (l = 0; l < fmaconf.bspboxvol; ++l) {
		/* The pointer to the relevant pattern. */
		pptr = fmaconf.radpats + l * fmaconf.nsamp;

		/* The basis index with respect to the parent box. */
		k = l % fmaconf.bspbox;
		j = (l / fmaconf.bspbox) % fmaconf.bspbox;
		i = l / (fmaconf.bspbox * fmaconf.bspbox);

		/* The distance from the basis to the box center. */
		dist[0] = ((float)i + 0.5 - clen) * fmaconf.cell;
		dist[1] = ((float)j + 0.5 - clen) * fmaconf.cell;
		dist[2] = ((float)k + 0.5 - clen) * fmaconf.cell;

		/* Construct the radiation pattern. */
		buildradpat (pptr, fmaconf.k0, dist, thetas, ntheta, nphi);
	}
}

	/* The FFT size. */
	nfft[0] = nfft[1] = nfft[2] = 2 * fmaconf.bspbox;
	nfftprod = nfft[0] * nfft[1] * nfft[2];

	/* Build the expanded grid. */
	totbpnbr = nfftprod * fmaconf.nborsvol;
	fmaconf.gridints = fftwf_malloc (totbpnbr * (1 + omp_get_max_threads()) * sizeof(complex float));
	dirbuf = fmaconf.gridints + totbpnbr;

	/* The forward FFT plan transforms all boxes in one pass. */
	fmaconf.fplan = fftwf_plan_many_dft (3, nfft, fmaconf.nborsvol,
			fmaconf.gridints, NULL, 1, nfftprod, fmaconf.gridints,
			NULL, 1, nfftprod, FFTW_FORWARD, FFTW_MEASURE);
	/* The inverse FFT plan only transforms a single box. */
	fmaconf.bplan = fftwf_plan_dft_3d (nfft[0], nfft[1], nfft[2],
			fmaconf.gridints, fmaconf.gridints, FFTW_BACKWARD, FFTW_MEASURE);

#pragma omp parallel default(shared)
{
	int off[3], l, i, j, k;
	complex float *grf;

#pragma omp for
	for (l = 0; l < fmaconf.nborsvol; ++l) {
		k = l % fmaconf.nbors;
		j = (l / fmaconf.nbors) % fmaconf.nbors;
		i = l / (fmaconf.nbors * fmaconf.nbors);

		grf = fmaconf.gridints + l * nfftprod;

		off[0] = (i - fmaconf.numbuffer) * fmaconf.bspbox;
		off[1] = (j - fmaconf.numbuffer) * fmaconf.bspbox;
		off[2] = (k - fmaconf.numbuffer) * fmaconf.bspbox;

		/* Build the Green's function grid for this local box. */
		greengrid (grf, fmaconf.bspbox, nfft[0], fmaconf.k0, fmaconf.cell, off);
	}
}

	/* Perform the Fourier transform of the Green's function. */
	fftwf_execute (fmaconf.fplan);

	free (thetas);

	return fmaconf.nbors;
}

/* Evaluate at a group of observers the fields due to a group of sources. */
void blockinteract (int nsrc, int nobs, int *srclist,
		int *obslist, void *vsrc, void *vobs, float *bc) {
	int l, bsoff[3], idx[3], i, j, k;
	complex float *csrc = (complex float *)vsrc, *cobs = (complex float *)vobs;
	complex float *buf, *bptr, *cbox;
	float fbox[3];

	/* Allocate and clear the buffer array. */
	buf = dirbuf + omp_get_thread_num() * totbpnbr;
	memset (buf, 0, totbpnbr * sizeof(complex float));

	/* Calculate the box position. */
	fbox[0] = (bc[0] - fmaconf.min[0]) / (fmaconf.cell * fmaconf.bspbox);
	fbox[1] = (bc[1] - fmaconf.min[1]) / (fmaconf.cell * fmaconf.bspbox);
	fbox[2] = (bc[2] - fmaconf.min[2]) / (fmaconf.cell * fmaconf.bspbox);

	/* Now calculate an offset for the basis indices. */
	bsoff[0] = ((int)(fbox[0]) - fmaconf.numbuffer) * fmaconf.bspbox;
	bsoff[1] = ((int)(fbox[1]) - fmaconf.numbuffer) * fmaconf.bspbox;
	bsoff[2] = ((int)(fbox[2]) - fmaconf.numbuffer) * fmaconf.bspbox;

	/* Populate the local grid. */
	for (l = 0; l < nsrc; ++l) {
		bsindex (srclist[l], idx);

		/* Convert the global grid position to a local position. */
		idx[0] -= bsoff[0];
		idx[1] -= bsoff[1];
		idx[2] -= bsoff[2];

		/* Find the local box number. */
		i = idx[0] / fmaconf.bspbox;
		j = idx[1] / fmaconf.bspbox;
		k = idx[2] / fmaconf.bspbox;

		/* Point to the local buffer for this box. */
		bptr = buf + nfftprod * IDX(fmaconf.nbors,i,j,k);

		/* Find the position in the local box. */
		i = idx[0] % fmaconf.bspbox;
		j = idx[1] % fmaconf.bspbox;
		k = idx[2] % fmaconf.bspbox;

		bptr[IDX(nfft[0],i,j,k)] = csrc[l];
	}

	/* Transform the local grids in place. */
	fftwf_execute_dft (fmaconf.fplan, buf, buf);

	/* The convolutions are now multiplications. */
	for (l = 0; l < totbpnbr; ++l) buf[l] *= fmaconf.gridints[l];

	/* Point to the center box. */
	cbox = buf + nfftprod * fmaconf.numbuffer * (1 + fmaconf.nbors * (1 + fmaconf.nbors));
	for (l = 0, bptr = buf; l < fmaconf.nborsvol; ++l, bptr += nfftprod) {
		if (bptr == cbox) continue;
		for (i = 0; i < nfftprod; ++i) cbox[i] += bptr[i];
	}

	/* Inverse transform the grid in place. */
	fftwf_execute_dft (fmaconf.bplan, cbox, cbox);

	/* Augment with output with the local convolution. */
	for (l = 0; l < nobs; ++l) {
		bsindex (obslist[l], idx);

		/* Convert the global grid position to a local position. */
		idx[0] %= fmaconf.bspbox;
		idx[1] %= fmaconf.bspbox;
		idx[2] %= fmaconf.bspbox;

		cobs[l] += cbox[IDX(nfft[0],idx[0],idx[1],idx[2])];
	}

	return;
}

/* initialisation and finalisation routines for ScaleME */
int ScaleME_preconf (void) {
	int error;
	float len, cen[3];
	
	/* The problem and tree are both three-dimensional. */
	ScaleME_setDimen (3);
	ScaleME_setTreeType (3);

	/* The fields are scalar-valued. */
	ScaleME_setFields (1);

	/* The wave number is real-valued. */
	ScaleME_setWaveNumber (fmaconf.k0);

	/* Set some MLFMA parameters. */
	ScaleME_setNumBasis (fmaconf.gnumbases);
	ScaleME_setMaxLevel (fmaconf.maxlev);
	ScaleME_setPrecision (fmaconf.precision);
	ScaleME_setMAC (fmaconf.numbuffer);
	ScaleME_setInterpOrder (fmaconf.interpord);

	ScaleME_setTopComputeLevel (fmaconf.toplev);

	if (fmaconf.fo2iterm > 0)
		ScaleME_selectFastO2I (fmaconf.fo2iterm, fmaconf.fo2iord, fmaconf.fo2iosr);

	/* Set the root box length. */
	len = (1 <<  fmaconf.maxlev) * fmaconf.bspbox * fmaconf.cell;
	/* Position the root box properly. */
	cen[0] = fmaconf.min[0] + 0.5 * len;
	cen[1] = fmaconf.min[1] + 0.5 * len;
	cen[2] = fmaconf.min[2] + 0.5 * len;
	ScaleME_setRootBox (len, cen);
	
	/*  let all processes start the initialisation together */
	MPI_Barrier(MPI_COMM_WORLD); 
	
	/* open the std files */
	MPFMA_stdout = stdout;
	MPFMA_stderr = stderr; 

	/* Use the external near-field interactions. */
	ScaleME_setBlockDirInterFunc (blockinteract);
	ScaleME_useExternFarField (farpattern);

	/* Finish the setup with the external interactions. */
	error = ScaleME_initSetUp (MPI_COMM_WORLD, NULL, NULL, NULL, bscenter);

	if (error) {
		fprintf(stdout, "ERROR: ScaleME pre-init failed.\n");
		ScaleME_finalizeParHostFMA();
		return -1;
	}

	return 0;
}

int ScaleME_postconf (void) {
	if (ScaleME_completeSetUp()) {
		fprintf(stdout, "ERROR: ScaleME setup routine failed.\n");
		goto error_handle;
	}

	if (ScaleME_initParHostDataStructs()) {
		fprintf(stdout, "ERROR: ScaleME parallel host init failed.\n");
		goto error_handle;
	}

	return 0;

error_handle:
	ScaleME_finalizeParHostFMA();
	return -1;
}
