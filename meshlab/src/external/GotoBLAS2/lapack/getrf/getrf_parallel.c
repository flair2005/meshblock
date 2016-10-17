/*********************************************************************/
/*                                                                   */
/*             Optimized BLAS libraries                              */
/*                     By Kazushige Goto <kgoto@tacc.utexas.edu>     */
/*                                                                   */
/* Copyright (c) The University of Texas, 2009. All rights reserved. */
/* UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING  */
/* THIS SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF      */
/* MERCHANTABILITY, FITNESS FOR ANY PARTICULAR PURPOSE,              */
/* NON-INFRINGEMENT AND WARRANTIES OF PERFORMANCE, AND ANY WARRANTY  */
/* THAT MIGHT OTHERWISE ARISE FROM COURSE OF DEALING OR USAGE OF     */
/* TRADE. NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH RESPECT TO   */
/* THE USE OF THE SOFTWARE OR DOCUMENTATION.                         */
/* Under no circumstances shall University be liable for incidental, */
/* special, indirect, direct or consequential damages or loss of     */
/* profits, interruption of business, or related expenses which may  */
/* arise from use of Software or Documentation, including but not    */
/* limited to those resulting from defects in Software and/or        */
/* Documentation, or loss or inaccuracy of data of any kind.         */
/*********************************************************************/

#include <stdio.h>
#include "common.h"

static FLOAT dm1 = -1.;

double sqrt(double);

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 8
#endif

#ifndef DIVIDE_RATE
#define DIVIDE_RATE 2
#endif

#define GEMM_PQ  MAX(GEMM_P, GEMM_Q)
#define REAL_GEMM_R (GEMM_R - GEMM_PQ)

#ifndef GETRF_FACTOR
#define GETRF_FACTOR 0.75
#endif

#undef  GETRF_FACTOR
#define GETRF_FACTOR 1.00

static inline long FORMULA1(long M, long N, long IS, long BK, long T) {

  double m = (double)(M - IS - BK);
  double n = (double)(N - IS - BK);
  double b = (double)BK;
  double a = (double)T;

  return (long)((n + GETRF_FACTOR * m * b * (1. - a) / (b + m)) / a);

}

#define FORMULA2(M, N, IS, BK, T) (BLASLONG)((double)(N - IS + BK) * (1. - sqrt(1. - 1. / (double)(T))))


static void inner_basic_thread(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n, FLOAT *sa, FLOAT *sb, BLASLONG mypos){

  BLASLONG is, min_i;
  BLASLONG js, min_j;
  BLASLONG jjs, min_jj;

  BLASLONG m = args -> m;
  BLASLONG n = args -> n;
  BLASLONG k = args -> k;

  BLASLONG lda = args -> lda;
  BLASLONG off = args -> ldb;

  FLOAT *b = (FLOAT *)args -> b + (k          ) * COMPSIZE;
  FLOAT *c = (FLOAT *)args -> b + (    k * lda) * COMPSIZE;
  FLOAT *d = (FLOAT *)args -> b + (k + k * lda) * COMPSIZE;
  FLOAT *sbb = sb;

  volatile BLASLONG *flag = (volatile BLASLONG *)args -> d;

  blasint *ipiv = (blasint *)args -> c;

  if (range_n) {
    n      = range_n[1] - range_n[0];
    c     += range_n[0] * lda * COMPSIZE;
    d     += range_n[0] * lda * COMPSIZE;
  }

  if (args -> a == NULL) {
    TRSM_ILTCOPY(k, k, (FLOAT *)args -> b, lda, 0, sb);
    sbb = (FLOAT *)((((long)(sb + k * k * COMPSIZE) + GEMM_ALIGN) & ~GEMM_ALIGN) + GEMM_OFFSET_B);
  } else {
    sb  = (FLOAT *)args -> a;
  }

  for (js = 0; js < n; js += REAL_GEMM_R) {
    min_j = n - js;
    if (min_j > REAL_GEMM_R) min_j = REAL_GEMM_R;

    for (jjs = js; jjs < js + min_j; jjs += GEMM_UNROLL_N){
      min_jj = js + min_j - jjs;
      if (min_jj > GEMM_UNROLL_N) min_jj = GEMM_UNROLL_N;
      
      if (GEMM_UNROLL_N <= 8) {

	LASWP_NCOPY(min_jj, off + 1, off + k, 
		    c + (- off + jjs * lda) * COMPSIZE, lda,
		    ipiv, sbb + k * (jjs - js) * COMPSIZE);

      } else {

	LASWP_PLUS(min_jj, off + 1, off + k, ZERO, 
#ifdef COMPLEX
		   ZERO,
#endif
		   c + (- off + jjs * lda) * COMPSIZE, lda, NULL, 0, ipiv, 1);
	
	GEMM_ONCOPY (k, min_jj, c + jjs * lda * COMPSIZE, lda, sbb + (jjs - js) * k * COMPSIZE);

      }

      for (is = 0; is < k; is += GEMM_P) {
	min_i = k - is;
	if (min_i > GEMM_P) min_i = GEMM_P;
	
	TRSM_KERNEL_LT(min_i, min_jj, k, dm1,
#ifdef COMPLEX
		       ZERO,
#endif
		       sb  + k * is * COMPSIZE,
		       sbb + (jjs - js) * k * COMPSIZE, 
		       c   + (is + jjs * lda) * COMPSIZE, lda, is);
      }
    }

    if ((js + REAL_GEMM_R >= n) && (mypos >= 0)) flag[mypos * CACHE_LINE_SIZE] = 0;

    for (is = 0; is < m; is += GEMM_P){
      min_i = m - is;
      if (min_i > GEMM_P) min_i = GEMM_P;
      
      GEMM_ITCOPY (k, min_i, b + is * COMPSIZE, lda, sa);
      
      GEMM_KERNEL_N(min_i, min_j, k, dm1,
#ifdef COMPLEX
		    ZERO,
#endif
		    sa, sbb, d + (is + js * lda) * COMPSIZE, lda);
    }
  }
}


/* Non blocking implementation */

typedef struct {
  volatile BLASLONG working[MAX_CPU_NUMBER][CACHE_LINE_SIZE * DIVIDE_RATE];
} job_t;

#define ICOPY_OPERATION(M, N, A, LDA, X, Y, BUFFER) GEMM_ITCOPY(M, N, (FLOAT *)(A) + ((Y) + (X) * (LDA)) * COMPSIZE, LDA, BUFFER);
#define OCOPY_OPERATION(M, N, A, LDA, X, Y, BUFFER) GEMM_ONCOPY(M, N, (FLOAT *)(A) + ((X) + (Y) * (LDA)) * COMPSIZE, LDA, BUFFER);

#ifndef COMPLEX
#define KERNEL_OPERATION(M, N, K, SA, SB, C, LDC, X, Y) \
	GEMM_KERNEL_N(M, N, K, dm1, SA, SB, (FLOAT *)(C) + ((X) + (Y) * LDC) * COMPSIZE, LDC)
#else
#define KERNEL_OPERATION(M, N, K, SA, SB, C, LDC, X, Y) \
	GEMM_KERNEL_N(M, N, K, dm1, ZERO, SA, SB, (FLOAT *)(C) + ((X) + (Y) * LDC) * COMPSIZE, LDC)
#endif

static int inner_advanced_thread(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n, FLOAT *sa, FLOAT *sb, BLASLONG mypos){

  job_t *job = (job_t *)args -> common;

  BLASLONG xxx, bufferside;

  FLOAT *buffer[DIVIDE_RATE];

  BLASLONG jjs, min_jj, div_n;

  BLASLONG i, current;
  BLASLONG is, min_i;

  BLASLONG m, n_from, n_to;
  BLASLONG k = args -> k;

  BLASLONG lda = args -> lda;
  BLASLONG off = args -> ldb;

  FLOAT *a = (FLOAT *)args -> b + (k          ) * COMPSIZE;
  FLOAT *b = (FLOAT *)args -> b + (    k * lda) * COMPSIZE;
  FLOAT *c = (FLOAT *)args -> b + (k + k * lda) * COMPSIZE;
  FLOAT *sbb= sb;

  blasint *ipiv = (blasint *)args -> c;

  volatile BLASLONG *flag = (volatile BLASLONG *)args -> d;

  if (args -> a == NULL) {
    TRSM_ILTCOPY(k, k, (FLOAT *)args -> b, lda, 0, sb);
    sbb = (FLOAT *)((((long)(sb + k * k * COMPSIZE) + GEMM_ALIGN) & ~GEMM_ALIGN) + GEMM_OFFSET_B);
  } else {
    sb  = (FLOAT *)args -> a;
  }

  m      = range_m[1] - range_m[0];
  n_from = range_n[mypos + 0];
  n_to   = range_n[mypos + 1];

  a     += range_m[0] * COMPSIZE;
  c     += range_m[0] * COMPSIZE;

  div_n = (n_to - n_from + DIVIDE_RATE - 1) / DIVIDE_RATE;
  
  buffer[0] = sbb;


  for (i = 1; i < DIVIDE_RATE; i++) {
    buffer[i] = buffer[i - 1] + GEMM_Q * ((div_n + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1)) * COMPSIZE;
  }

  for (xxx = n_from, bufferside = 0; xxx < n_to; xxx += div_n, bufferside ++) {
    
    for (i = 0; i < args -> nthreads; i++)
      while (job[mypos].working[i][CACHE_LINE_SIZE * bufferside]) {};
    
    for(jjs = xxx; jjs < MIN(n_to, xxx + div_n); jjs += min_jj){
      min_jj = MIN(n_to, xxx + div_n) - jjs;
      if (min_jj > GEMM_UNROLL_N) min_jj = GEMM_UNROLL_N;

      if (GEMM_UNROLL_N <= 8) {

	LASWP_NCOPY(min_jj, off + 1, off + k, 
		    b + (- off + jjs * lda) * COMPSIZE, lda,
		    ipiv, buffer[bufferside] + (jjs - xxx) * k * COMPSIZE);

      } else {

	LASWP_PLUS(min_jj, off + 1, off + k, ZERO, 
#ifdef COMPLEX
		   ZERO,
#endif
		   b + (- off + jjs * lda) * COMPSIZE, lda, NULL, 0, ipiv, 1);
	
	GEMM_ONCOPY (k, min_jj, b + jjs * lda * COMPSIZE, lda, 
		     buffer[bufferside] + (jjs - xxx) * k * COMPSIZE);
      }

      for (is = 0; is < k; is += GEMM_P) {
	min_i = k - is;
	if (min_i > GEMM_P) min_i = GEMM_P;
	
	TRSM_KERNEL_LT(min_i, min_jj, k, dm1,
#ifdef COMPLEX
		       ZERO,
#endif
		       sb + k * is * COMPSIZE,
		       buffer[bufferside] + (jjs - xxx) * k * COMPSIZE, 
		       b   + (is + jjs * lda) * COMPSIZE, lda, is);
      }
    }
	
    for (i = 0; i < args -> nthreads; i++)
      job[mypos].working[i][CACHE_LINE_SIZE * bufferside] = (BLASLONG)buffer[bufferside];

  }
  
  flag[mypos * CACHE_LINE_SIZE] = 0;
  
  if (m == 0) {
    for (xxx = 0; xxx < DIVIDE_RATE; xxx++) {
      job[mypos].working[mypos][CACHE_LINE_SIZE * xxx] = 0;
    }
  }

  for(is = 0; is < m; is += min_i){
    min_i = m - is;
    if (min_i >= GEMM_P * 2) {
      min_i = GEMM_P;
    } else 
      if (min_i > GEMM_P) {
	min_i = ((min_i + 1) / 2 + GEMM_UNROLL_M - 1) & ~(GEMM_UNROLL_M - 1);
      }
      
      ICOPY_OPERATION(k, min_i, a, lda, 0, is, sa);
      
      current = mypos;

      do {
	
	div_n = (range_n[current + 1]  - range_n[current] + DIVIDE_RATE - 1) / DIVIDE_RATE;
	
	for (xxx = range_n[current], bufferside = 0; xxx < range_n[current + 1]; xxx += div_n, bufferside ++) {
	  
	  if ((current != mypos) && (!is)) {
	    	    while(job[current].working[mypos][CACHE_LINE_SIZE * bufferside] == 0) {};
	  }

	  KERNEL_OPERATION(min_i, MIN(range_n[current + 1] - xxx, div_n), k,
			   sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
			   c, lda, is, xxx);
	  
	  if (is + min_i >= m) {
	    job[current].working[mypos][CACHE_LINE_SIZE * bufferside] = 0;
	  }
	}
	
	current ++;
	if (current >= args -> nthreads) current = 0;
	
      } while (current != mypos);
  }
  
  for (i = 0; i < args -> nthreads; i++) {
    for (xxx = 0; xxx < DIVIDE_RATE; xxx++) {
      while (job[mypos].working[i][CACHE_LINE_SIZE * xxx] ) {};
    }
  }

  return 0;
}

#if 1

blasint CNAME(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n, FLOAT *sa, FLOAT *sb, BLASLONG myid) {

  BLASLONG m, n, mn, lda, offset;
  BLASLONG init_bk, next_bk, range_n_mine[2], range_n_new[2];
  blasint *ipiv, iinfo, info;
  int mode;
  blas_arg_t newarg;

  FLOAT *a, *sbb;
  FLOAT dummyalpha[2] = {ZERO, ZERO};

  blas_queue_t queue[MAX_CPU_NUMBER];

  BLASLONG range_M[MAX_CPU_NUMBER + 1];
  BLASLONG range_N[MAX_CPU_NUMBER + 1];

  job_t        job[MAX_CPU_NUMBER];

  BLASLONG width, nn, mm;
  BLASLONG i, j, k, is, bk;

  BLASLONG num_cpu;

  volatile BLASLONG flag[MAX_CPU_NUMBER * CACHE_LINE_SIZE] __attribute__((aligned(128)));

#ifndef COMPLEX
#ifdef XDOUBLE
  mode  =  BLAS_XDOUBLE | BLAS_REAL;
#elif defined(DOUBLE)
  mode  =  BLAS_DOUBLE  | BLAS_REAL;
#else
  mode  =  BLAS_SINGLE  | BLAS_REAL;
#endif  
#else
#ifdef XDOUBLE
  mode  =  BLAS_XDOUBLE | BLAS_COMPLEX;
#elif defined(DOUBLE)
  mode  =  BLAS_DOUBLE  | BLAS_COMPLEX;
#else
  mode  =  BLAS_SINGLE  | BLAS_COMPLEX;
#endif  
#endif

  m    = args -> m;
  n    = args -> n;
  a    = (FLOAT *)args -> a;
  lda  = args -> lda;
  ipiv = (blasint *)args -> c;
  offset = 0;

  if (range_n) {
    m     -= range_n[0];
    n      = range_n[1] - range_n[0];
    offset = range_n[0];
    a     += range_n[0] * (lda + 1) * COMPSIZE;
  }

  if (m <= 0 || n <= 0) return 0;
  
  newarg.c   = ipiv;
  newarg.lda = lda;
  newarg.common   = (void *)job;

  info = 0;

  mn = MIN(m, n);

  init_bk = (mn / 2 + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);
  if (init_bk > GEMM_Q) init_bk = GEMM_Q;

  if (init_bk <= GEMM_UNROLL_N) {
    info = GETF2(args, NULL, range_n, sa, sb, 0);
    return info;
  }

  next_bk = init_bk;

  bk = mn;
  if (bk > next_bk) bk = next_bk;
  
  range_n_new[0] = offset;
  range_n_new[1] = offset + bk;
  
  iinfo   = CNAME(args, NULL, range_n_new, sa, sb, 0);
  
  if (iinfo && !info) info = iinfo;
  
  TRSM_ILTCOPY(bk, bk, a, lda, 0, sb);

  sbb = (FLOAT *)((((long)(sb + bk * bk * COMPSIZE) + GEMM_ALIGN) & ~GEMM_ALIGN) + GEMM_OFFSET_B);
  
  is = 0;
  num_cpu = 0;

  while (is < mn) {
    
    width  = (FORMULA1(m, n, is, bk, args -> nthreads) + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);
    if (width > mn - is - bk) width = mn - is - bk;

    if (width < bk) {
      next_bk = (FORMULA2(m, n, is, bk, args -> nthreads) + GEMM_UNROLL_N) & ~(GEMM_UNROLL_N - 1);
      
      if (next_bk > bk) next_bk = bk;

      width = next_bk;
      if (width > mn - is - bk) width = mn - is - bk;
    }
    
    if (num_cpu > 0) exec_blas_async_wait(num_cpu, &queue[0]);

    mm = m - bk - is;
    nn = n - bk - is;

    newarg.a   = sb;
    newarg.b   = a + (is + is * lda) * COMPSIZE;
    newarg.d   = (void *)flag;
    newarg.m   = mm;
    newarg.n   = nn;
    newarg.k   = bk;
    newarg.ldb = is + offset;
    
    nn -= width;

    range_n_mine[0] = 0;
    range_n_mine[1] = width;

    range_N[0] = width;
    range_M[0] = 0;

    num_cpu  = 0;
    
    while (nn > 0){
      
      if (mm >= nn) {

	width  = blas_quickdivide(nn + args -> nthreads - num_cpu, args -> nthreads - num_cpu - 1);
	if (nn < width) width = nn;
	nn -= width;
	range_N[num_cpu + 1] = range_N[num_cpu] + width;
	
	width  = blas_quickdivide(mm + args -> nthreads - num_cpu, args -> nthreads - num_cpu - 1);
	if (mm < width) width = mm;
	if (nn <=    0) width = mm;
	mm -= width;
	range_M[num_cpu + 1] = range_M[num_cpu] + width;

      } else {

	width  = blas_quickdivide(mm + args -> nthreads - num_cpu, args -> nthreads - num_cpu - 1);
	if (mm < width) width = mm;
	mm -= width;
	range_M[num_cpu + 1] = range_M[num_cpu] + width;

	width  = blas_quickdivide(nn + args -> nthreads - num_cpu, args -> nthreads - num_cpu - 1);
	if (nn < width) width = nn;
	if (mm <=    0) width = nn;
	nn -= width;
	range_N[num_cpu + 1] = range_N[num_cpu] + width;
	
      }

      queue[num_cpu].mode    = mode;
      queue[num_cpu].routine = inner_advanced_thread;
      queue[num_cpu].args    = &newarg;
      queue[num_cpu].range_m = &range_M[num_cpu];
      queue[num_cpu].range_n = &range_N[0];
      queue[num_cpu].sa      = NULL;
      queue[num_cpu].sb      = NULL;
      queue[num_cpu].next    = &queue[num_cpu + 1];
      flag[num_cpu * CACHE_LINE_SIZE] = 1;
      
      num_cpu ++;

    }
    
    newarg.nthreads = num_cpu;
    
    if (num_cpu > 0) {
      for (j = 0; j < num_cpu; j++) {
	for (i = 0; i < num_cpu; i++) {
	  for (k = 0; k < DIVIDE_RATE; k++) {
	    job[j].working[i][CACHE_LINE_SIZE * k] = 0;
	  }
	}
      }
    }

    is += bk;

    bk = mn - is;
    if (bk > next_bk) bk = next_bk;
    
    range_n_new[0] = offset + is;
    range_n_new[1] = offset + is + bk;

    if (num_cpu > 0) {

      queue[num_cpu - 1].next = NULL;
      
      exec_blas_async(0, &queue[0]);
      
      inner_basic_thread(&newarg, NULL, range_n_mine, sa, sbb, -1);
      
      iinfo   = GETRF_SINGLE(args, NULL, range_n_new, sa, sbb, 0);
      
      if (iinfo && !info) info = iinfo + is;

      for (i = 0; i < num_cpu; i ++) while (flag[i * CACHE_LINE_SIZE]) {};

      TRSM_ILTCOPY(bk, bk, a + (is +  is * lda) * COMPSIZE, lda, 0, sb);

    } else {

      inner_basic_thread(&newarg, NULL, range_n_mine, sa, sbb, -1);

      iinfo   = GETRF_SINGLE(args, NULL, range_n_new, sa, sbb, 0);

      if (iinfo && !info) info = iinfo + is;
    
    }
    
  }
  
  next_bk = init_bk;
  is = 0;
  
  while (is < mn) {
    
    bk = mn - is;
    if (bk > next_bk) bk = next_bk;
    
    width  = (FORMULA1(m, n, is, bk, args -> nthreads) + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);
    if (width > mn - is - bk) width = mn - is - bk;

    if (width < bk) {
      next_bk = (FORMULA2(m, n, is, bk, args -> nthreads) + GEMM_UNROLL_N) & ~(GEMM_UNROLL_N - 1);
      if (next_bk > bk) next_bk = bk;
    }

    blas_level1_thread(mode, bk, is + bk + offset + 1, mn + offset, (void *)dummyalpha, 
		       a + (- offset + is * lda) * COMPSIZE, lda, NULL, 0,
		       ipiv, 1, (void *)LASWP_PLUS, args -> nthreads);
    
    is += bk;
  }
  
  return info;
}

#else

blasint CNAME(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n, FLOAT *sa, FLOAT *sb, BLASLONG myid) {

  BLASLONG m, n, mn, lda, offset;
  BLASLONG i, is, bk, init_bk, next_bk, range_n_new[2];
  blasint *ipiv, iinfo, info;
  int mode;
  blas_arg_t newarg;
  FLOAT *a, *sbb;
  FLOAT dummyalpha[2] = {ZERO, ZERO};

  blas_queue_t queue[MAX_CPU_NUMBER];
  BLASLONG range[MAX_CPU_NUMBER + 1];

  BLASLONG width, nn, num_cpu;

  volatile BLASLONG flag[MAX_CPU_NUMBER * CACHE_LINE_SIZE] __attribute__((aligned(128)));

#ifndef COMPLEX
#ifdef XDOUBLE
  mode  =  BLAS_XDOUBLE | BLAS_REAL;
#elif defined(DOUBLE)
  mode  =  BLAS_DOUBLE  | BLAS_REAL;
#else
  mode  =  BLAS_SINGLE  | BLAS_REAL;
#endif  
#else
#ifdef XDOUBLE
  mode  =  BLAS_XDOUBLE | BLAS_COMPLEX;
#elif defined(DOUBLE)
  mode  =  BLAS_DOUBLE  | BLAS_COMPLEX;
#else
  mode  =  BLAS_SINGLE  | BLAS_COMPLEX;
#endif  
#endif

  m    = args -> m;
  n    = args -> n;
  a    = (FLOAT *)args -> a;
  lda  = args -> lda;
  ipiv = (blasint *)args -> c;
  offset = 0;

  if (range_n) {
    m     -= range_n[0];
    n      = range_n[1] - range_n[0];
    offset = range_n[0];
    a     += range_n[0] * (lda + 1) * COMPSIZE;
  }

  if (m <= 0 || n <= 0) return 0;
  
  newarg.c   = ipiv;
  newarg.lda = lda;
  newarg.common = NULL;
  newarg.nthreads = args -> nthreads;

  mn = MIN(m, n);

  init_bk = (mn / 2 + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);
  if (init_bk > GEMM_Q) init_bk = GEMM_Q;

  if (init_bk <= GEMM_UNROLL_N) {
    info = GETF2(args, NULL, range_n, sa, sb, 0);
    return info;
  }

  width = FORMULA1(m, n, 0, init_bk, args -> nthreads);
  width = (width + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);
  if (width > n - init_bk) width = n - init_bk;

  if (width < init_bk) {
    long temp;

    temp = FORMULA2(m, n, 0, init_bk, args -> nthreads);
    temp = (temp + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);

    if (temp < GEMM_UNROLL_N) temp = GEMM_UNROLL_N;
    if (temp < init_bk) init_bk = temp;

  }

  next_bk = init_bk;
  bk      = init_bk;

  range_n_new[0] = offset;
  range_n_new[1] = offset + bk;
  
  info   = CNAME(args, NULL, range_n_new, sa, sb, 0);
  
  TRSM_ILTCOPY(bk, bk, a, lda, 0, sb);

  is = 0;
  num_cpu = 0;

  sbb = (FLOAT *)((((long)(sb + GEMM_PQ * GEMM_PQ * COMPSIZE) + GEMM_ALIGN) & ~GEMM_ALIGN) + GEMM_OFFSET_B);

  while (is < mn) {

    width  = FORMULA1(m, n, is, bk, args -> nthreads);
    width = (width + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);
    
    if (width < bk) {

      next_bk = FORMULA2(m, n, is, bk, args -> nthreads);
      next_bk = (next_bk + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);

      if (next_bk > bk) next_bk = bk;
#if 0
      if (next_bk < GEMM_UNROLL_N) next_bk = MIN(GEMM_UNROLL_N, mn - bk - is);
#else
      if (next_bk < GEMM_UNROLL_N) next_bk = MAX(GEMM_UNROLL_N, mn - bk - is);
#endif

      width = next_bk;
    }
    
    if (width > mn - is - bk) {
      next_bk = mn - is - bk;
      width   = next_bk;
    }

    nn = n - bk - is;
    if (width > nn) width = nn;

    if (num_cpu > 1)  exec_blas_async_wait(num_cpu - 1, &queue[1]);

    range[0] = 0;
    range[1] = width;
    
    num_cpu = 1;
    nn -= width;
    
    newarg.a   = sb;
    newarg.b   = a + (is + is * lda) * COMPSIZE;
    newarg.d   = (void *)flag;
    newarg.m   = m - bk - is;
    newarg.n   = n - bk - is;
    newarg.k   = bk;
    newarg.ldb = is + offset;
    
    while (nn > 0){
      
      width  = blas_quickdivide(nn + args -> nthreads - num_cpu, args -> nthreads - num_cpu);
      
      nn -= width;
      if (nn < 0) width = width + nn;
      
      range[num_cpu + 1] = range[num_cpu] + width;
      
      queue[num_cpu].mode    = mode;
      //queue[num_cpu].routine = inner_advanced_thread;
      queue[num_cpu].routine = (void *)inner_basic_thread;
      queue[num_cpu].args    = &newarg;
      queue[num_cpu].range_m = NULL;
      queue[num_cpu].range_n = &range[num_cpu];
      queue[num_cpu].sa      = NULL;
      queue[num_cpu].sb      = NULL;
      queue[num_cpu].next    = &queue[num_cpu + 1];
      flag[num_cpu * CACHE_LINE_SIZE] = 1;

      num_cpu ++;
    }
    
    queue[num_cpu - 1].next = NULL;

    is += bk;
    
    bk = n - is;
    if (bk > next_bk) bk = next_bk;
    
    range_n_new[0] = offset + is;
    range_n_new[1] = offset + is + bk;
    
    if (num_cpu > 1) {

      exec_blas_async(1, &queue[1]);
    
#if 0
      inner_basic_thread(&newarg, NULL, &range[0], sa, sbb, 0);

      iinfo = GETRF_SINGLE(args, NULL, range_n_new, sa, sbb, 0);
#else

      if (range[1] >= bk * 4) {

	BLASLONG myrange[2];

	myrange[0] = 0;
	myrange[1] = bk;

	inner_basic_thread(&newarg, NULL, &myrange[0], sa, sbb, -1);

	iinfo = GETRF_SINGLE(args, NULL, range_n_new, sa, sbb, 0);

	myrange[0] = bk;
	myrange[1] = range[1];

	inner_basic_thread(&newarg, NULL, &myrange[0], sa, sbb, -1);

      } else {

	inner_basic_thread(&newarg, NULL, &range[0], sa, sbb, -1);

	iinfo = GETRF_SINGLE(args, NULL, range_n_new, sa, sbb, 0);
      }

#endif

      for (i = 1; i < num_cpu; i ++) while (flag[i * CACHE_LINE_SIZE]) {};
      
      TRSM_ILTCOPY(bk, bk, a + (is +  is * lda) * COMPSIZE, lda, 0, sb);
      
    } else {

      inner_basic_thread(&newarg, NULL, &range[0], sa, sbb, -1);
      
      iinfo = GETRF_SINGLE(args, NULL, range_n_new, sa, sbb, 0);
    }

      if (iinfo && !info) info = iinfo + is;
      
  }
  
  next_bk = init_bk;
  bk      = init_bk;
  
  is = 0;
  
  while (is < mn) {
    
    bk = mn - is;
    if (bk > next_bk) bk = next_bk;
    
    width  = FORMULA1(m, n, is, bk, args -> nthreads);
    width = (width + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);

    if (width < bk) {
      next_bk = FORMULA2(m, n, is, bk, args -> nthreads);
      next_bk = (next_bk + GEMM_UNROLL_N - 1) & ~(GEMM_UNROLL_N - 1);

      if (next_bk > bk) next_bk = bk;
#if 0
      if (next_bk < GEMM_UNROLL_N) next_bk = MIN(GEMM_UNROLL_N, mn - bk - is);
#else
      if (next_bk < GEMM_UNROLL_N) next_bk = MAX(GEMM_UNROLL_N, mn - bk - is);
#endif
    }

    if (width > mn - is - bk) {
      next_bk = mn - is - bk;
      width   = next_bk;
    }

    blas_level1_thread(mode, bk, is + bk + offset + 1, mn + offset, (void *)dummyalpha, 
		       a + (- offset + is * lda) * COMPSIZE, lda, NULL, 0,
		       ipiv, 1, (void *)LASWP_PLUS, args -> nthreads);
    
    is += bk;
  }
  
  return info;
}

#endif

