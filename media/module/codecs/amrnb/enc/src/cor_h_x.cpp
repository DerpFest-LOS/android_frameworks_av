/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*
------------------------------------------------------------------------------



 Pathname: ./audio/gsm-amr/c/src/cor_h_x.c

     Date: 09/07/2000

------------------------------------------------------------------------------
 REVISION HISTORY

 Description: Created a separate file for cor_h_x function.

 Description: Synchronized file with UMTS versin 3.2.0. Updated coding
              template.

 Description: Made the following changes per comments from Phase 2/3 review:
              1. Modified FOR loop in the code to count down.
              2. Fixed typecasting issue with TI C compiler.

 Description: Added call to round() and L_shl() functions in the last FOR
              loop to make code bit-exact. Updated copyright year.

 Description: Modified to pass pOverflow in via a pointer, rather than
 invoking it as a global variable.

 Description: Made the following changes
              1. Unrolled the correlation loop and add mechanism control
                 to compute odd or even number of computations.
              2. Use pointer to avoid continuos addresses calculation
              2. Eliminated math operations that check for saturation.

 Description: Changed round function name to pv_round to avoid conflict with
              round function in C standard library.

 Description:

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"
#include "cnst.h"
#include "cor_h_x.h"
#include "basic_op.h"

/*----------------------------------------------------------------------------
; MACROS
; Define module specific macros here
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; DEFINES
; Include all pre-processor statements here. Include conditional
; compile variables also.
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; LOCAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; LOCAL STORE/BUFFER/POINTER DEFINITIONS
; Variable declaration - defined here and used outside this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL FUNCTION REFERENCES
; Declare functions defined elsewhere and referenced in this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
; Declare variables used in this module but defined elsewhere
----------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: cor_h_x
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    h = vector containing the impulse response of the weighted synthesis
        filter; vector contents are of type Word16; vector length is
        2 * L_SUBFR
    x = target signal vector; vector contents are of type Word16; vector
        length is L_SUBFR
    dn = vector containing the correlation between the target and the
         impulse response; vector contents are of type Word16; vector
         length is L_CODE
    sf = scaling factor of type Word16 ; 2 when mode is MR122, 1 for all
         other modes

 Outputs:
    dn contents are the newly calculated correlation values

    pOverflow = pointer of type Flag * to overflow indicator.

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function computes the correlation between the target signal (x) and the
 impulse response (h).

 The correlation is given by: d[n] = sum_{i=n}^{L-1} x[i] h[i-n],
 where: n=0,...,L-1

 d[n] is normalized such that the sum of 5 maxima of d[n] corresponding to
 each position track does not saturate.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 cor_h.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

void cor_h_x (
    Word16 h[],    // (i): impulse response of weighted synthesis filter
    Word16 x[],    // (i): target
    Word16 dn[],   // (o): correlation between target and h[]
    Word16 sf      // (i): scaling factor: 2 for 12.2, 1 for others
)
{
    cor_h_x2(h, x, dn, sf, NB_TRACK, STEP);
}


void cor_h_x2 (
    Word16 h[],    // (i): impulse response of weighted synthesis filter
    Word16 x[],    // (i): target
    Word16 dn[],   // (o): correlation between target and h[]
    Word16 sf,     // (i): scaling factor: 2 for 12.2, 1 for others
    Word16 nb_track,// (i): the number of ACB tracks
    Word16 step    // (i): step size from one pulse position to the next
                           in one track
)
{
    Word16 i, j, k;
    Word32 s, y32[L_CODE], max, tot;

    // first keep the result on 32 bits and find absolute maximum

    tot = 5;

    for (k = 0; k < nb_track; k++)
    {
        max = 0;
        for (i = k; i < L_CODE; i += step)
        {
            s = 0;
            for (j = i; j < L_CODE; j++)
                s = L_mac (s, x[j], h[j - i]);

            y32[i] = s;

            s = L_abs (s);
            if (L_sub (s, max) > (Word32) 0L)
                max = s;
        }
        tot = L_add (tot, L_shr (max, 1));
    }

    j = sub (norm_l (tot), sf);

    for (i = 0; i < L_CODE; i++)
    {
        dn[i] = pv_round (L_shl (y32[i], j));
    }
}

------------------------------------------------------------------------------
 RESOURCES USED [optional]

 When the code is written for a specific target processor the
 the resources used should be documented below.

 HEAP MEMORY USED: x bytes

 STACK MEMORY USED: x bytes

 CLOCK CYCLES: (cycle count equation for this function) + (variable
                used to represent cycle count for each subroutine
                called)
     where: (cycle count variable) = cycle count for [subroutine
                                     name]

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void cor_h_x(
    Word16 h[],       /* (i): impulse response of weighted synthesis filter */
    Word16 x[],       /* (i): target                                        */
    Word16 dn[],      /* (o): correlation between target and h[]            */
    Word16 sf,        /* (i): scaling factor: 2 for 12.2, 1 for others      */
    Flag   *pOverflow /* (o): pointer to overflow flag                      */
)
{
    Word16 i;
    Word16 j;
    Word16 k;

    Word32 s;
    Word32 mul;
    Word32 y32[L_CODE];
    Word32 max;
    Word32 tot;

    Word16 *p_x;
    Word16 *p_ptr;
    Word32 *p_y32;


    tot = 5;
    for (k = 0; k < NB_TRACK; k++)              /* NB_TRACK = 5 */
    {
        max = 0;
        for (i = k; i < L_CODE; i += STEP)      /* L_CODE = 40; STEP = 5 */
        {
            s = 0;
            p_x = &x[i];
            p_ptr = h;

            for (j = (L_CODE - i - 1) >> 1; j != 0; j--)
            {
                __builtin_mul_overflow(*(p_x++), *(p_ptr++), &mul);
                __builtin_add_overflow(s, mul << 1, &s);
                __builtin_mul_overflow(*(p_x++), *(p_ptr++), &mul);
                __builtin_add_overflow(s, mul << 1, &s);
            }

            __builtin_mul_overflow(*(p_x++), *(p_ptr++), &mul);
            __builtin_add_overflow(s, mul << 1, &s);

            if (!((L_CODE - i) & 1))    /* if even number of iterations */
            {
                __builtin_mul_overflow(*(p_x++), *(p_ptr++), &mul);
                __builtin_add_overflow(s, mul << 1, &s);
            }

            y32[i] = s;

            if (s < 0)
            {
                s = -s;
            }

            if (s > max)
            {
                max = s;
            }
        }

        __builtin_add_overflow(tot, (max >> 1), &tot);
    }


    j = norm_l(tot) - sf;

    p_ptr = dn;
    p_y32 = y32;;

    for (i = L_CODE >> 1; i != 0; i--)
    {
        Word32 result;
        s = L_shl(*(p_y32++), j, pOverflow);
        __builtin_add_overflow(s, 0x00008000, &result);
        *(p_ptr++) = result >> 16;
        s = L_shl(*(p_y32++), j, pOverflow);
        __builtin_add_overflow(s, 0x00008000, &result);
        *(p_ptr++) = result >> 16;
    }

    return;
}
