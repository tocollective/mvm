/*
 * Copyright 2026 tocollective contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "internal.h"

#include <math.h>

static double math_sign(const double value) {
  if (value == 0.0 || isnan(value))
    return value;
  return copysign(1.0, value);
}

static double math_round_even(const double value) {
  if (!isfinite(value) || value == 0.0)
    return value;
  const double lower = floor(value);
  const double fraction = value - lower;
  double rounded = lower;
  if (fraction > 0.5 || (fraction == 0.5 && fmod(lower, 2.0) != 0.0))
    rounded = lower + 1.0;
  return copysign(rounded, value);
}

static double math_fraction(const double value) {
  double integral;
  return modf(value, &integral);
}

// Unlike C fmin/fmax, NaN propagates and signed zero is ordered as in JVM Math.
static double math_min(const double left, const double right) {
  if (isnan(left))
    return left;
  if (isnan(right))
    return right;
  if (left == 0.0 && right == 0.0)
    return signbit(left) ? left : right;
  return left < right ? left : right;
}

static double math_max(const double left, const double right) {
  if (isnan(left))
    return left;
  if (isnan(right))
    return right;
  if (left == 0.0 && right == 0.0)
    return signbit(left) ? right : left;
  return left > right ? left : right;
}

static double math_clamp(const double value, const double lower,
                         const double upper) {
  if (lower > upper ||
      (lower == 0.0 && upper == 0.0 && !signbit(lower) && signbit(upper)))
    return NAN;
  return math_max(lower, math_min(value, upper));
}

static double math_lerp(const double start, const double end,
                        const double amount) {
  if (!isfinite(start) || !isfinite(end) || !isfinite(amount))
    return NAN;
  if (amount == 0.0)
    return start;
  if (amount == 1.0)
    return end;
  // Avoid overflowing end - start when finite endpoints have opposite signs.
  const double value = (signbit(start) != 0) != (signbit(end) != 0)
                           ? (1.0 - amount) * start + amount * end
                           : fma(amount, end - start, start);
  if (amount > 0.0 && amount < 1.0)
    return math_clamp(value, math_min(start, end), math_max(start, end));
  return value;
}

static double math_to_radians(const double value) {
  return value * 0.017453292519943295769236907684886127;
}

static double math_to_degrees(const double value) {
  return value * 57.29577951308232087679815481410517033;
}

static bool math_is_nan(const double value) { return isnan(value) != 0; }
static bool math_is_infinite(const double value) { return isinf(value) != 0; }
static bool math_is_finite(const double value) { return isfinite(value) != 0; }
static bool math_is_integer(const double value) {
  return isfinite(value) && trunc(value) == value;
}
static bool math_sign_bit(const double value) { return signbit(value) != 0; }

mvm_result_t mvm_math_register(mvm_t *mvm) {
  static const mvm_kind_t args[] = {MVM_KIND_DOUBLE, MVM_KIND_DOUBLE,
                                    MVM_KIND_DOUBLE};
  static const struct {
    const char *name;
    void (*function)(void);
    mvm_kind_t result_kind;
    uint8_t argc;
  } functions[] = {
      {"mathAbs", (void (*)(void))fabs, MVM_KIND_DOUBLE, 1},
      {"mathSign", (void (*)(void))math_sign, MVM_KIND_DOUBLE, 1},
      {"mathFloor", (void (*)(void))floor, MVM_KIND_DOUBLE, 1},
      {"mathCeil", (void (*)(void))ceil, MVM_KIND_DOUBLE, 1},
      {"mathRound", (void (*)(void))round, MVM_KIND_DOUBLE, 1},
      {"mathRoundEven", (void (*)(void))math_round_even, MVM_KIND_DOUBLE, 1},
      {"mathTrunc", (void (*)(void))trunc, MVM_KIND_DOUBLE, 1},
      {"mathFraction", (void (*)(void))math_fraction, MVM_KIND_DOUBLE, 1},
      {"mathSqrt", (void (*)(void))sqrt, MVM_KIND_DOUBLE, 1},
      {"mathCbrt", (void (*)(void))cbrt, MVM_KIND_DOUBLE, 1},
      {"mathExp", (void (*)(void))exp, MVM_KIND_DOUBLE, 1},
      {"mathExp2", (void (*)(void))exp2, MVM_KIND_DOUBLE, 1},
      {"mathExpm1", (void (*)(void))expm1, MVM_KIND_DOUBLE, 1},
      {"mathLog", (void (*)(void))log, MVM_KIND_DOUBLE, 1},
      {"mathLog2", (void (*)(void))log2, MVM_KIND_DOUBLE, 1},
      {"mathLog10", (void (*)(void))log10, MVM_KIND_DOUBLE, 1},
      {"mathLog1p", (void (*)(void))log1p, MVM_KIND_DOUBLE, 1},
      {"mathSin", (void (*)(void))sin, MVM_KIND_DOUBLE, 1},
      {"mathCos", (void (*)(void))cos, MVM_KIND_DOUBLE, 1},
      {"mathTan", (void (*)(void))tan, MVM_KIND_DOUBLE, 1},
      {"mathAsin", (void (*)(void))asin, MVM_KIND_DOUBLE, 1},
      {"mathAcos", (void (*)(void))acos, MVM_KIND_DOUBLE, 1},
      {"mathAtan", (void (*)(void))atan, MVM_KIND_DOUBLE, 1},
      {"mathSinh", (void (*)(void))sinh, MVM_KIND_DOUBLE, 1},
      {"mathCosh", (void (*)(void))cosh, MVM_KIND_DOUBLE, 1},
      {"mathTanh", (void (*)(void))tanh, MVM_KIND_DOUBLE, 1},
      {"mathAsinh", (void (*)(void))asinh, MVM_KIND_DOUBLE, 1},
      {"mathAcosh", (void (*)(void))acosh, MVM_KIND_DOUBLE, 1},
      {"mathAtanh", (void (*)(void))atanh, MVM_KIND_DOUBLE, 1},
      {"mathErf", (void (*)(void))erf, MVM_KIND_DOUBLE, 1},
      {"mathErfc", (void (*)(void))erfc, MVM_KIND_DOUBLE, 1},
      {"mathToRadians", (void (*)(void))math_to_radians, MVM_KIND_DOUBLE, 1},
      {"mathToDegrees", (void (*)(void))math_to_degrees, MVM_KIND_DOUBLE, 1},
      {"mathMin", (void (*)(void))math_min, MVM_KIND_DOUBLE, 2},
      {"mathMax", (void (*)(void))math_max, MVM_KIND_DOUBLE, 2},
      {"mathPow", (void (*)(void))pow, MVM_KIND_DOUBLE, 2},
      {"mathAtan2", (void (*)(void))atan2, MVM_KIND_DOUBLE, 2},
      {"mathHypot", (void (*)(void))hypot, MVM_KIND_DOUBLE, 2},
      {"mathFmod", (void (*)(void))fmod, MVM_KIND_DOUBLE, 2},
      {"mathRemainder", (void (*)(void))remainder, MVM_KIND_DOUBLE, 2},
      {"mathCopySign", (void (*)(void))copysign, MVM_KIND_DOUBLE, 2},
      {"mathNextAfter", (void (*)(void))nextafter, MVM_KIND_DOUBLE, 2},
      {"mathClamp", (void (*)(void))math_clamp, MVM_KIND_DOUBLE, 3},
      {"mathLerp", (void (*)(void))math_lerp, MVM_KIND_DOUBLE, 3},
      {"mathFma", (void (*)(void))fma, MVM_KIND_DOUBLE, 3},
      {"mathIsNaN", (void (*)(void))math_is_nan, MVM_KIND_BOOL, 1},
      {"mathIsInfinite", (void (*)(void))math_is_infinite, MVM_KIND_BOOL, 1},
      {"mathIsFinite", (void (*)(void))math_is_finite, MVM_KIND_BOOL, 1},
      {"mathIsInteger", (void (*)(void))math_is_integer, MVM_KIND_BOOL, 1},
      {"mathSignBit", (void (*)(void))math_sign_bit, MVM_KIND_BOOL, 1},
  };
  for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
    const mvm_result_t status = mvm_register_native(
        mvm, functions[i].name, functions[i].function, functions[i].result_kind,
        functions[i].argc, args, NULL);
    if (status != MVM_RESULT_OK)
      return status;
  }
  return MVM_RESULT_OK;
}
