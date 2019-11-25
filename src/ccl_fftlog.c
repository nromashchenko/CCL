#include <stdlib.h>
#include <math.h>

#include <fftw3.h>

#include <gsl/gsl_sf_result.h>
#include <gsl/gsl_sf_gamma.h>
#include <complex.h>
#include "ccl_fftlog.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_LN2
#define M_LN2 0.69314718056
#endif
/* This code is FFTLog, which is described in arXiv:astro-ph/9905191 */

static double complex lngamma_fftlog(double complex z)
{
  gsl_sf_result lnr, phi;
  gsl_sf_lngamma_complex_e(creal(z), cimag(z), &lnr, &phi);
  return lnr.val + I*phi.val;
}

static double complex gamma_fftlog(double complex z)
{
  return cexp(lngamma_fftlog(z));
}

static double complex polar (double r, double phi)
{
  return (r*cos(phi) +I*(r*sin(phi)));
}

static void lngamma_4(double x, double y, double* lnr, double* arg)
{
  double complex w = lngamma_fftlog(x+y*I);
  if(lnr) *lnr = creal(w);
  if(arg) *arg = cimag(w);
}

static double goodkr(int N, double mu, double q, double L, double kr)
{
  double xp = (mu+1+q)/2;
  double xm = (mu+1-q)/2;
  double y = M_PI*N/(2*L);
  double lnr, argm, argp;
  lngamma_4(xp, y, &lnr, &argp);
  lngamma_4(xm, y, &lnr, &argm);
  double arg = log(2/kr) * N/L + (argp + argm)/M_PI;
  double iarg = round(arg);
  if(arg != iarg)
    kr *= exp((arg - iarg)*L/N);
  return kr;
}

/* Pre-compute the coefficients that appear in the FFTLog implementation of
 * the discrete Hankel transform.  The parameters N, mu, and q here are the
 * same as for the function fht().  The parameter L is defined (for whatever
 * reason) to be N times the logarithmic spacing of the input array, i.e.
 *   L = N * log(r[N-1]/r[0])/(N-1) */
static void compute_u_coefficients(int N, double mu, double q, double L, double kcrc, double complex u[])
{
  double y = M_PI/L;
  double k0r0 = kcrc * exp(-L);
  double t = -2*y*log(k0r0/2);

  if(q == 0) {
    double x = (mu+1)/2;
    double lnr, phi;
    for(int m = 0; m <= N/2; m++) {
      lngamma_4(x, m*y, &lnr, &phi);
      u[m] = polar(1.0,m*t + 2*phi);
    }
  }
  else {
    double xp = (mu+1+q)/2;
    double xm = (mu+1-q)/2;
    double lnrp, phip, lnrm, phim;
    for(int m = 0; m <= N/2; m++) {
      lngamma_4(xp, m*y, &lnrp, &phip);
      lngamma_4(xm, m*y, &lnrm, &phim);
      u[m] = polar(exp(q*M_LN2 + lnrp - lnrm), m*t + phip - phim);
    }
  }

  for(int m = N/2+1; m < N; m++)
    u[m] = conj(u[N-m]);
  if((N % 2) == 0)
    u[N/2] = (creal(u[N/2]) + I*0.0);
}

/* Compute the discrete Hankel transform of the function a(r).  See the FFTLog
 * documentation (or the Fortran routine of the same name in the FFTLog
 * sources) for a description of exactly what this function computes.
 * If u is NULL, the transform coefficients will be computed anew and discarded
 * afterwards.  If you plan on performing many consecutive transforms, it is
 * more efficient to pre-compute the u coefficients. */
void fht(int N, const double r[], const double complex a[], double k[], double complex b[], double mu,
         double q, double kcrc, int noring, double complex* u)
{
  double L = log(r[N-1]/r[0]) * N/(N-1.);
  double complex* ulocal = NULL;
  if(u == NULL) {
    if(noring)
      kcrc = goodkr(N, mu, q, L, kcrc);
    ulocal = malloc (sizeof(complex double)*N);
    compute_u_coefficients(N, mu, q, L, kcrc, ulocal);
    u = ulocal;
  }

  /* Compute the convolution b = a*u using FFTs */
  fftw_plan forward_plan = fftw_plan_dft_1d(N, (fftw_complex*) a, (fftw_complex*) b,  -1, FFTW_ESTIMATE);
  fftw_plan reverse_plan = fftw_plan_dft_1d(N, (fftw_complex*) b, (fftw_complex*) b, +1, FFTW_ESTIMATE);
  fftw_execute(forward_plan);
  for(int m = 0; m < N; m++)
    b[m] *= u[m] / (double)(N);       // divide by N since FFTW doesn't normalize the inverse FFT
  fftw_execute(reverse_plan);
  fftw_destroy_plan(forward_plan);
  fftw_destroy_plan(reverse_plan);

  /* Reverse b array */
  double complex tmp;
  for(int n = 0; n < N/2; n++) {
    tmp = b[n];
    b[n] = b[N-n-1];
    b[N-n-1] = tmp;
  }

  /* Compute k's corresponding to input r's */
  double k0r0 = kcrc * exp(-L);
  k[0] = k0r0/r[0];
  for(int n = 1; n < N; n++)
    k[n] = k[0] * exp(n*L/N);

  free(ulocal);
}

static void fftlog_xi_dim(double dim, double mu, double epsilon,
			  int N, const double k[], const double pk[],
			  double r[], double xi[])
{
  double complex* a = malloc(sizeof(complex double)*N);
  double complex* b = malloc(sizeof(complex double)*N);

  for(int i = 0; i < N; i++)
    a[i] = pow(k[i], dim/2-epsilon) * pk[i];
  double one_over_2pi_dhalf = pow(2*M_PI,-dim/2);
  //  N, r, fr, k, fk,mu, q,    kcrc, noring,*u_return
  fht(N, k, a,  r, b, mu, epsilon, 1, 1, NULL);
  for(int i = 0; i < N; i++)
    xi[i] = one_over_2pi_dhalf * pow(r[i], -dim/2-epsilon) * creal(b[i]);

  free(a);
  free(b);
}
 
void ccl_fftlog_ComputeXi2D(double mu,double epsilon,
			    int N, const double l[],const double cl[],
			    double th[], double xi[])
{
  fftlog_xi_dim(2, mu, epsilon, N, l, cl, th, xi);
}

void ccl_fftlog_ComputeXi3D(double l, double epsilon,
			    int N, const double k[], const double pk[],
			    double r[], double xi[])
{
  fftlog_xi_dim(3., l+0.5, epsilon, N, k, pk, r, xi);
}