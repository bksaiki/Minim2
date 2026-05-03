#include "minim.h"

#include <string.h>

/* ======================================================================
 * Structural equality.
 *
 * The cdr case is turned into a loop (`continue`) rather than a tail
 * call so that a deep proper list does not chew through the C stack;
 * the car case still recurses, but a balanced tree of pair/vector
 * splits is fine.
 *
 * No allocation happens here, so neither argument needs shadow-stack
 * protection across the call.
 * ====================================================================== */

bool Mequal(mobj a, mobj b) {
    for (;;) {
        if (a == b) return true;

        if (Mpairp(a) && Mpairp(b)) {
            if (!Mequal(Mcar(a), Mcar(b))) return false;
            a = Mcdr(a);
            b = Mcdr(b);
            continue;
        }

        if (Mvectorp(a) && Mvectorp(b)) {
            size_t len = Mvector_length(a);
            if (Mvector_length(b) != len) return false;
            for (size_t i = 0; i < len; i++) {
                if (!Mequal(Mvector_ref(a, i), Mvector_ref(b, i)))
                    return false;
            }
            return true;
        }

        if (Mflonump(a) && Mflonump(b)) {
            return Mflonum_val(a) == Mflonum_val(b);
        }

        return false;
    }
}

/* ======================================================================
 * Hash. Must agree with Mequal: any two values that compare equal
 * must hash the same.
 *
 * The mixer is splitmix64-style: xor + multiply by the 64-bit golden
 * ratio + xor-shift. Cheap, well-distributed, non-cryptographic.
 *
 * Per-type seeds prevent shape-vs-leaf collisions that pure word-
 * input mixing wouldn't otherwise rule out (e.g. an empty vector
 * shouldn't collide with the immediate `()`). The seeds are arbitrary
 * high-entropy 64-bit constants from splitmix64's published
 * coefficients.
 *
 * The pair branch uses the same cdr-loop trick as Mequal so a long
 * proper list doesn't recurse on the C stack.
 * ====================================================================== */

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 0x9E3779B97F4A7C15ULL;
    h ^= h >> 31;
    return h;
}

uint64_t Mhash(mobj v) {
    if (Mpairp(v)) {
        uint64_t h = 0x517CC1B727220A95ULL;
        for (;;) {
            h = mix(h, Mhash(Mcar(v)));
            mobj cdr = Mcdr(v);
            if (Mpairp(cdr)) { v = cdr; continue; }
            return mix(h, Mhash(cdr));
        }
    }
    if (Mvectorp(v)) {
        uint64_t h = 0xC2B2AE3D27D4EB4FULL;
        size_t len = Mvector_length(v);
        h = mix(h, (uint64_t)len);
        for (size_t i = 0; i < len; i++)
            h = mix(h, Mhash(Mvector_ref(v, i)));
        return h;
    }
    if (Mflonump(v)) {
        /* Mequal compares flonums numerically, and IEEE-754 says
         * `+0.0 == -0.0`. Canonicalize the sign of zero so the bit
         * patterns we hash agree. NaN bit-patterns can vary, but
         * `nan != nan` under Mequal so divergent NaN hashes don't
         * break the invariant. */
        double d = Mflonum_val(v);
        if (d == 0.0) d = 0.0;
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        return mix(0xBF58476D1CE4E5B9ULL, bits);
    }
    /* Fixnum, symbol (interned), immediate (including char), closure,
     * prim, kont, env: Mequal reduces to word equality for these, so
     * hashing the mobj word is sufficient. */
    return mix(0x94D049BB133111EBULL, (uint64_t)v);
}
