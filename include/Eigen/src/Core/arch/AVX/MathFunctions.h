// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2014 Pedro Gonnet (pedro.gonnet@gmail.com)
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_MATH_FUNCTIONS_AVX_H
#define EIGEN_MATH_FUNCTIONS_AVX_H

// For some reason, this function didn't make it into the avxintirn.h
// used by the compiler, so we'll just wrap it.
#define _mm256_setr_m128(lo, hi) \
  _mm256_insertf128_si256(_mm256_castsi128_si256(lo), (hi), 1)

/* The sin, cos, exp, and log functions of this file are loosely derived from
 * Julien Pommier's sse math library: http://gruntthepeon.free.fr/ssemath/
 */

namespace Eigen {

namespace internal {

// Sine function
// Computes sin(x) by wrapping x to the interval [-Pi/4,3*Pi/4] and
// evaluating interpolants in [-Pi/4,Pi/4] or [Pi/4,3*Pi/4]. The interpolants
// are (anti-)symmetric and thus have only odd/even coefficients
template <>
EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED Packet8f
psin<Packet8f>(const Packet8f& _x) {
  Packet8f x = _x;

  // Some useful values.
  _EIGEN_DECLARE_CONST_Packet8i(one, 1);
  _EIGEN_DECLARE_CONST_Packet8f(one, 1.0f);
  _EIGEN_DECLARE_CONST_Packet8f(two, 2.0f);
  _EIGEN_DECLARE_CONST_Packet8f(one_over_four, 0.25f);
  _EIGEN_DECLARE_CONST_Packet8f(one_over_pi, 3.183098861837907e-01f);
  _EIGEN_DECLARE_CONST_Packet8f(neg_pi_first, -3.140625000000000e+00);
  _EIGEN_DECLARE_CONST_Packet8f(neg_pi_second, -9.670257568359375e-04);
  _EIGEN_DECLARE_CONST_Packet8f(neg_pi_third, -6.278329571784980e-07);
  _EIGEN_DECLARE_CONST_Packet8f(four_over_pi, 1.273239544735163e+00);

  // Map x from [-Pi/4,3*Pi/4] to z in [-1,3] and subtract the shifted period.
  Packet8f z = pmul(x, p8f_one_over_pi);
  Packet8f shift = _mm256_floor_ps(padd(z, p8f_one_over_four));
  x = pmadd(shift, p8f_neg_pi_first, x);
  x = pmadd(shift, p8f_neg_pi_second, x);
  x = pmadd(shift, p8f_neg_pi_third, x);
  z = pmul(x, p8f_four_over_pi);

  // Make a mask for the entries that need flipping, i.e. wherever the shift
  // is odd.
  Packet8i shift_ints = _mm256_cvtps_epi32(shift);
  Packet8i shift_isodd =
      (__m256i)_mm256_and_ps((__m256)shift_ints, (__m256)p8i_one);
#ifdef EIGEN_VECTORIZE_AVX2
  Packet8i sign_flip_mask = _mm256_slli_epi32(shift_isodd, 31);
#else
  __m128i lo =
      _mm_slli_epi32(_mm256_extractf128_si256((__m256i)shift_isodd, 0), 31);
  __m128i hi =
      _mm_slli_epi32(_mm256_extractf128_si256((__m256i)shift_isodd, 1), 31);
  Packet8i sign_flip_mask = _mm256_setr_m128(lo, hi);
#endif

  // Create a mask for which interpolant to use, i.e. if z > 1, then the mask
  // is set to ones for that entry.
  Packet8f ival_mask = _mm256_cmp_ps(z, p8f_one, _CMP_GT_OQ);

  // Evaluate the polynomial for the interval [1,3] in z.
  _EIGEN_DECLARE_CONST_Packet8f(coeff_right_0, 9.999999724233232e-01f);
  _EIGEN_DECLARE_CONST_Packet8f(coeff_right_2, -3.084242535619928e-01);
  _EIGEN_DECLARE_CONST_Packet8f(coeff_right_4, 1.584991525700324e-02);
  _EIGEN_DECLARE_CONST_Packet8f(coeff_right_6, -3.188805084631342e-04);
  Packet8f z_minus_two = psub(z, p8f_two);
  Packet8f z_minus_two2 = pmul(z_minus_two, z_minus_two);
  Packet8f right = pmadd(p8f_coeff_right_6, z_minus_two2, p8f_coeff_right_4);
  right = pmadd(right, z_minus_two2, p8f_coeff_right_2);
  right = pmadd(right, z_minus_two2, p8f_coeff_right_0);

  // Evaluate the polynomial for the interval [-1,1] in z.
  _EIGEN_DECLARE_CONST_Packet8f(coeff_left_1, 7.853981525427295e-01);
  _EIGEN_DECLARE_CONST_Packet8f(coeff_left_3, -8.074536727092352e-02);
  _EIGEN_DECLARE_CONST_Packet8f(coeff_left_5, 2.489871967827018e-03);
  _EIGEN_DECLARE_CONST_Packet8f(coeff_left_7, -3.587725841214251e-05);
  Packet8f z2 = pmul(z, z);
  Packet8f left = pmadd(p8f_coeff_left_7, z2, p8f_coeff_left_5);
  left = pmadd(left, z2, p8f_coeff_left_3);
  left = pmadd(left, z2, p8f_coeff_left_1);
  left = pmul(left, z);

  // Assemble the results, i.e. select the left and right polynomials.
  left = _mm256_andnot_ps(ival_mask, left);
  right = _mm256_and_ps(ival_mask, right);
  Packet8f res = _mm256_or_ps(left, right);

  // Flip the sign on the odd intervals and return the result.
  res = _mm256_xor_ps(res, (__m256)sign_flip_mask);
  return res;
}

// Natural logarithm
// Computes log(x) as log(2^e * m) = C*e + log(m), where the constant C =log(2)
// and m is in the range [sqrt(1/2),sqrt(2)). In this range, the logarithm can
// be easily approximated by a polynomial centered on m=1 for stability.
// TODO(gonnet): Further reduce the interval allowing for lower-degree
//               polynomial interpolants -> ... -> profit!
template <>
EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED Packet8f
plog<Packet8f>(const Packet8f& _x) {
  Packet8f x = _x;
  _EIGEN_DECLARE_CONST_Packet8f(1, 1.0f);
  _EIGEN_DECLARE_CONST_Packet8f(half, 0.5f);
  _EIGEN_DECLARE_CONST_Packet8f(126f, 126.0f);

  _EIGEN_DECLARE_CONST_Packet8f_FROM_INT(inv_mant_mask, ~0x7f800000);

  // The smallest non denormalized float number.
  _EIGEN_DECLARE_CONST_Packet8f_FROM_INT(min_norm_pos, 0x00800000);
  _EIGEN_DECLARE_CONST_Packet8f_FROM_INT(minus_inf, 0xff800000);

  // Polynomial coefficients.
  _EIGEN_DECLARE_CONST_Packet8f(cephes_SQRTHF, 0.707106781186547524f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p0, 7.0376836292E-2f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p1, -1.1514610310E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p2, 1.1676998740E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p3, -1.2420140846E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p4, +1.4249322787E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p5, -1.6668057665E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p6, +2.0000714765E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p7, -2.4999993993E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_p8, +3.3333331174E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_q1, -2.12194440e-4f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_log_q2, 0.693359375f);

  Packet8f invalid_mask = _mm256_cmp_ps(x, _mm256_setzero_ps(), _CMP_NGE_UQ); // not greater equal is true if x is NaN
  Packet8f iszero_mask = _mm256_cmp_ps(x, _mm256_setzero_ps(), _CMP_EQ_OQ);

  // Truncate input values to the minimum positive normal.
  x = pmax(x, p8f_min_norm_pos);

// Extract the shifted exponents (No bitwise shifting in regular AVX, so
// convert to SSE and do it there).
#ifdef EIGEN_VECTORIZE_AVX2
  Packet8f emm0 = _mm256_cvtepi32_ps(_mm256_srli_epi32((__m256i)x, 23));
#else
  __m128i lo = _mm_srli_epi32(_mm256_extractf128_si256((__m256i)x, 0), 23);
  __m128i hi = _mm_srli_epi32(_mm256_extractf128_si256((__m256i)x, 1), 23);
  Packet8f emm0 = _mm256_cvtepi32_ps(_mm256_setr_m128(lo, hi));
#endif
  Packet8f e = _mm256_sub_ps(emm0, p8f_126f);

  // Set the exponents to -1, i.e. x are in the range [0.5,1).
  x = _mm256_and_ps(x, p8f_inv_mant_mask);
  x = _mm256_or_ps(x, p8f_half);

  // part2: Shift the inputs from the range [0.5,1) to [sqrt(1/2),sqrt(2))
  // and shift by -1. The values are then centered around 0, which improves
  // the stability of the polynomial evaluation.
  //   if( x < SQRTHF ) {
  //     e -= 1;
  //     x = x + x - 1.0;
  //   } else { x = x - 1.0; }
  Packet8f mask = _mm256_cmp_ps(x, p8f_cephes_SQRTHF, _CMP_LT_OQ);
  Packet8f tmp = _mm256_and_ps(x, mask);
  x = psub(x, p8f_1);
  e = psub(e, _mm256_and_ps(p8f_1, mask));
  x = padd(x, tmp);

  Packet8f x2 = pmul(x, x);
  Packet8f x3 = pmul(x2, x);

  // Evaluate the polynomial approximant of degree 8 in three parts, probably
  // to improve instruction-level parallelism.
  Packet8f y, y1, y2;
  y = pmadd(p8f_cephes_log_p0, x, p8f_cephes_log_p1);
  y1 = pmadd(p8f_cephes_log_p3, x, p8f_cephes_log_p4);
  y2 = pmadd(p8f_cephes_log_p6, x, p8f_cephes_log_p7);
  y = pmadd(y, x, p8f_cephes_log_p2);
  y1 = pmadd(y1, x, p8f_cephes_log_p5);
  y2 = pmadd(y2, x, p8f_cephes_log_p8);
  y = pmadd(y, x3, y1);
  y = pmadd(y, x3, y2);
  y = pmul(y, x3);

  // Add the logarithm of the exponent back to the result of the interpolation.
  y1 = pmul(e, p8f_cephes_log_q1);
  tmp = pmul(x2, p8f_half);
  y = padd(y, y1);
  x = psub(x, tmp);
  y2 = pmul(e, p8f_cephes_log_q2);
  x = padd(x, y);
  x = padd(x, y2);

  // Filter out invalid inputs, i.e. negative arg will be NAN, 0 will be -INF.
  return _mm256_or_ps(
      _mm256_andnot_ps(iszero_mask, _mm256_or_ps(x, invalid_mask)),
      _mm256_and_ps(iszero_mask, p8f_minus_inf));
}

// Exponential function. Works by writing "x = m*log(2) + r" where
// "m = floor(x/log(2)+1/2)" and "r" is the remainder. The result is then
// "exp(x) = 2^m*exp(r)" where exp(r) is in the range [-1,1).
template <>
EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED Packet8f
pexp<Packet8f>(const Packet8f& _x) {
  _EIGEN_DECLARE_CONST_Packet8f(1, 1.0f);
  _EIGEN_DECLARE_CONST_Packet8f(half, 0.5f);
  _EIGEN_DECLARE_CONST_Packet8f(127, 127.0f);

  _EIGEN_DECLARE_CONST_Packet8f(exp_hi, 88.3762626647950f);
  _EIGEN_DECLARE_CONST_Packet8f(exp_lo, -88.3762626647949f);

  _EIGEN_DECLARE_CONST_Packet8f(cephes_LOG2EF, 1.44269504088896341f);

  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_p0, 1.9875691500E-4f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_p1, 1.3981999507E-3f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_p2, 8.3334519073E-3f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_p3, 4.1665795894E-2f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_p4, 1.6666665459E-1f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_p5, 5.0000001201E-1f);

  // Clamp x.
  Packet8f x = pmax(pmin(_x, p8f_exp_hi), p8f_exp_lo);

  // Express exp(x) as exp(m*ln(2) + r), start by extracting
  // m = floor(x/ln(2) + 0.5).
  Packet8f m = _mm256_floor_ps(pmadd(x, p8f_cephes_LOG2EF, p8f_half));

// Get r = x - m*ln(2). If no FMA instructions are available, m*ln(2) is
// subtracted out in two parts, m*C1+m*C2 = m*ln(2), to avoid accumulating
// truncation errors. Note that we don't use the "pmadd" function here to
// ensure that a precision-preserving FMA instruction is used.
#ifdef EIGEN_VECTORIZE_FMA
  _EIGEN_DECLARE_CONST_Packet8f(nln2, -0.6931471805599453f);
  Packet8f r = _mm256_fmadd_ps(m, p8f_nln2, x);
#else
  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_C1, 0.693359375f);
  _EIGEN_DECLARE_CONST_Packet8f(cephes_exp_C2, -2.12194440e-4f);
  Packet8f r = psub(x, pmul(m, p8f_cephes_exp_C1));
  r = psub(r, pmul(m, p8f_cephes_exp_C2));
#endif

  Packet8f r2 = pmul(r, r);

  // TODO(gonnet): Split into odd/even polynomials and try to exploit
  //               instruction-level parallelism.
  Packet8f y = p8f_cephes_exp_p0;
  y = pmadd(y, r, p8f_cephes_exp_p1);
  y = pmadd(y, r, p8f_cephes_exp_p2);
  y = pmadd(y, r, p8f_cephes_exp_p3);
  y = pmadd(y, r, p8f_cephes_exp_p4);
  y = pmadd(y, r, p8f_cephes_exp_p5);
  y = pmadd(y, r2, r);
  y = padd(y, p8f_1);

  // Build emm0 = 2^m.
  Packet8i emm0 = _mm256_cvttps_epi32(padd(m, p8f_127));
#ifdef EIGEN_VECTORIZE_AVX2
  emm0 = _mm256_slli_epi32(emm0, 23);
#else
  __m128i lo = _mm_slli_epi32(_mm256_extractf128_si256(emm0, 0), 23);
  __m128i hi = _mm_slli_epi32(_mm256_extractf128_si256(emm0, 1), 23);
  emm0 = _mm256_setr_m128(lo, hi);
#endif

  // Return 2^m * exp(r).
  return pmax(pmul(y, _mm256_castsi256_ps(emm0)), _x);
}

// Functions for sqrt.
// The EIGEN_FAST_MATH version uses the _mm_rsqrt_ps approximation and one step
// of Newton's method, at a cost of 1-2 bits of precision as opposed to the
// exact solution. The main advantage of this approach is not just speed, but
// also the fact that it can be inlined and pipelined with other computations,
// further reducing its effective latency.
#if EIGEN_FAST_MATH
template <>
EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED Packet8f
psqrt<Packet8f>(const Packet8f& _x) {
  _EIGEN_DECLARE_CONST_Packet8f(one_point_five, 1.5f);
  _EIGEN_DECLARE_CONST_Packet8f(minus_half, -0.5f);
  _EIGEN_DECLARE_CONST_Packet8f_FROM_INT(flt_min, 0x00800000);

  Packet8f neg_half = pmul(_x, p8f_minus_half);

  // select only the inverse sqrt of positive normal inputs (denormals are
  // flushed to zero and cause infs as well).
  Packet8f non_zero_mask = _mm256_cmp_ps(_x, p8f_flt_min, _CMP_GE_OQ);
  Packet8f x = _mm256_and_ps(non_zero_mask, _mm256_rsqrt_ps(_x));

  // Do a single step of Newton's iteration.
  x = pmul(x, pmadd(neg_half, pmul(x, x), p8f_one_point_five));

  // Multiply the original _x by it's reciprocal square root to extract the
  // square root.
  return pmul(_x, x);
}
#else
template <> EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED
Packet8f psqrt<Packet8f>(const Packet8f& x) {
  return _mm256_sqrt_ps(x);
}
#endif
template <> EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED
Packet4d psqrt<Packet4d>(const Packet4d& x) {
  return _mm256_sqrt_pd(x);
}
#if EIGEN_FAST_MATH

template<> EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED
Packet8f prsqrt<Packet8f>(const Packet8f& _x) {
 _EIGEN_DECLARE_CONST_Packet8f_FROM_INT(inf, 0x7f800000);
  _EIGEN_DECLARE_CONST_Packet8f_FROM_INT(nan, 0x7fc00000);
  _EIGEN_DECLARE_CONST_Packet8f(one_point_five, 1.5f);
  _EIGEN_DECLARE_CONST_Packet8f(minus_half, -0.5f);
  _EIGEN_DECLARE_CONST_Packet8f_FROM_INT(flt_min, 0x00800000);

  Packet8f neg_half = pmul(_x, p8f_minus_half);

  // select only the inverse sqrt of positive normal inputs (denormals are
  // flushed to zero and cause infs as well).
  Packet8f le_zero_mask = _mm256_cmp_ps(_x, p8f_flt_min, _CMP_LT_OQ);
  Packet8f x = _mm256_andnot_ps(le_zero_mask, _mm256_rsqrt_ps(_x));

  // Fill in NaNs and Infs for the negative/zero entries.
  Packet8f neg_mask = _mm256_cmp_ps(_x, _mm256_setzero_ps(), _CMP_LT_OQ);
  Packet8f zero_mask = _mm256_andnot_ps(neg_mask, le_zero_mask);
  Packet8f infs_and_nans = _mm256_or_ps(_mm256_and_ps(neg_mask, p8f_nan),
                                        _mm256_and_ps(zero_mask, p8f_inf));

  // Do a single step of Newton's iteration.
  x = pmul(x, pmadd(neg_half, pmul(x, x), p8f_one_point_five));

  // Insert NaNs and Infs in all the right places.
  return _mm256_or_ps(x, infs_and_nans);
}

#else
template <> EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED
Packet8f prsqrt<Packet8f>(const Packet8f& x) {
  _EIGEN_DECLARE_CONST_Packet8f(one, 1.0f);
  return _mm256_div_ps(p8f_one, _mm256_sqrt_ps(x));
}
#endif

template <> EIGEN_DEFINE_FUNCTION_ALLOWING_MULTIPLE_DEFINITIONS EIGEN_UNUSED
Packet4d prsqrt<Packet4d>(const Packet4d& x) {
  _EIGEN_DECLARE_CONST_Packet4d(one, 1.0);
  return _mm256_div_pd(p4d_one, _mm256_sqrt_pd(x));
}


}  // end namespace internal

}  // end namespace Eigen

#endif  // EIGEN_MATH_FUNCTIONS_AVX_H
