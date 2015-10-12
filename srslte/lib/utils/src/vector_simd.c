/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2015 The srsLTE Developers. See the
 * COPYRIGHT file at the top-level directory of this distribution.
 *
 * \section LICENSE
 *
 * This file is part of the srsLTE library.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */


#include <float.h>
#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "srslte/utils/vector_simd.h"

#include <inttypes.h>
#include <stdio.h>

#include <xmmintrin.h>

void print128_num(__m128i var)
{
    int16_t *val = (int16_t*) &var;//can also use uint32_t instead of 16_t
    printf("Numerical: %d %d %d %d %d %d %d %d \n", 
           val[0], val[1], val[2], val[3], val[4], val[5], 
           val[6], val[7]);
}

void srslte_vec_sum_sss_simd(short *x, short *y, short *z, uint32_t len)
{
  unsigned int number = 0;
  const unsigned int points = len / 8;

  const __m128i* xPtr = (const __m128i*) x;
  const __m128i* yPtr = (const __m128i*) y;
  __m128i* zPtr = (__m128i*) z;

  __m128i xVal, yVal, zVal;
  for(;number < points; number++){

    xVal = _mm_load_si128(xPtr);
    yVal = _mm_load_si128(yPtr);

    zVal = _mm_add_epi16(xVal, yVal);

    _mm_store_si128(zPtr, zVal); 

    xPtr ++;
    yPtr ++;
    zPtr ++;
  }

  number = points * 8;
  for(;number < len; number++){
    z[number] = x[number] + y[number];
  }
}

void srslte_vec_sub_sss_simd(short *x, short *y, short *z, uint32_t len)
{
  unsigned int number = 0;
  const unsigned int points = len / 8;

  const __m128i* xPtr = (const __m128i*) x;
  const __m128i* yPtr = (const __m128i*) y;
  __m128i* zPtr = (__m128i*) z;

  __m128i xVal, yVal, zVal;
  for(;number < points; number++){

    xVal = _mm_load_si128(xPtr);
    yVal = _mm_load_si128(yPtr);

    zVal = _mm_sub_epi16(xVal, yVal);

    _mm_store_si128(zPtr, zVal); 

    xPtr ++;
    yPtr ++;
    zPtr ++;
  }

  number = points * 8;
  for(;number < len; number++){
    z[number] = x[number] - y[number];
  }
}

void srslte_vec_sc_div2_sss_simd(short *x, int k, short *z, uint32_t len)
{
  unsigned int number = 0;
  const unsigned int points = len / 8;

  const __m128i* xPtr = (const __m128i*) x;
  __m128i* zPtr = (__m128i*) z;

  __m128i xVal, zVal;
  for(;number < points; number++){

    xVal = _mm_load_si128(xPtr);
    
    zVal = _mm_srai_epi16(xVal, k);                 
      
    _mm_store_si128(zPtr, zVal); 

    xPtr ++;
    zPtr ++;
  }

  number = points * 8;
  short divn = (1<<k);
  for(;number < len; number++){
    z[number] = x[number] / divn;
  }
}

void srslte_vec_lut_sss_simd(short *x, unsigned short *lut, short *y, uint32_t len)
{
  unsigned int number = 0;
  const unsigned int points = len / 8;

  const __m128i* xPtr = (const __m128i*) x;
  const __m128i* lutPtr = (__m128i*) lut;

  __m128i xVal, lutVal;
  for(;number < points; number++){

    xVal   = _mm_load_si128(xPtr);
    lutVal = _mm_load_si128(lutPtr);
    
    for (int i=0;i<8;i++) {
      uint16_t x = (uint16_t) _mm_extract_epi16(xVal, i); 
      uint16_t l = (uint16_t) _mm_extract_epi16(lutVal, i);
      y[l] = x;
    }
    xPtr ++;
    lutPtr ++;
  }

  number = points * 8;
  for(;number < len; number++){
    y[lut[number]] = x[number];
  }
  
}

/* Modified from volk_32f_s32f_convert_16i_a_sse2. Removed clipping */
void srslte_vec_convert_fi_simd(float *x, int16_t *z, float scale, uint32_t len)
{
  unsigned int number = 0;

  const unsigned int eighthPoints = len / 8;

  const float* inputVectorPtr = (const float*)x;
  int16_t* outputVectorPtr = z;

  __m128 vScalar = _mm_set_ps1(scale);
  __m128 inputVal1, inputVal2;
  __m128i intInputVal1, intInputVal2;
  __m128 ret1, ret2;

  for(;number < eighthPoints; number++){
    inputVal1 = _mm_load_ps(inputVectorPtr); inputVectorPtr += 4;
    inputVal2 = _mm_load_ps(inputVectorPtr); inputVectorPtr += 4;

    ret1 = _mm_mul_ps(inputVal1, vScalar);
    ret2 = _mm_mul_ps(inputVal2, vScalar);

    intInputVal1 = _mm_cvtps_epi32(ret1);
    intInputVal2 = _mm_cvtps_epi32(ret2);

    printf("intinput: "); print128_num(intInputVal1);
    printf("intinput2: "); print128_num(intInputVal2);

    intInputVal1 = _mm_packs_epi32(intInputVal1, intInputVal2);

    _mm_store_si128((__m128i*)outputVectorPtr, intInputVal1);
    outputVectorPtr += 8;
  }

  number = eighthPoints * 8;
  for(; number < len; number++){
    z[number] = (int16_t) (x[number] * scale);
  }

}