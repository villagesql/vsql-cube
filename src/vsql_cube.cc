/* Copyright (c) 2026 VillageSQL Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

// vsql_cube: Multi-dimensional cube type for VillageSQL.
//
// Port of PostgreSQL's cube extension. Supports n-dimensional boxes and points
// (up to 100 dimensions). Storage is parameterized: cube(n) uses 8 + 2*n*8
// bytes per row. Bare cube defaults to cube(32) = 520 bytes.
//
// Binary layout for cube(n):
//   Bytes 0-1:         uint16_t ndim      (actual dims stored, 0–n)
//   Byte  2:           uint8_t  flags     (bit 0 = is_point)
//   Bytes 3-7:         padding            (zero-filled)
//   Bytes 8 to 8+n*8-1:      double ll[n]  (lower-left corners)
//   Bytes 8+n*8 to 8+2n*8-1: double ur[n]  (upper-right corners)
//   Total: 8 + 2*n*8 bytes

#include <villagesql/vsql.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

using namespace vsql;

// =============================================================================
// Constants
// =============================================================================

static constexpr const char kCubeName[] = "cube";
// Absolute maximum dimensions (matches PostgreSQL cube extension).
constexpr int kAbsoluteMaxDims = 100;
// Default when no parameter given: cube == cube(32).
constexpr int kDefaultMaxDims = 32;
// Storage sizes in bytes: header(8) + 2 × n × sizeof(double).
constexpr size_t kDefaultStorageSize = 8 + 2 * kDefaultMaxDims * sizeof(double);  // 520
constexpr size_t kMaxStorageSize     = 8 + 2 * kAbsoluteMaxDims * sizeof(double); // 1608
// Max string output: 2 corners × 100 dims × 25 chars + brackets + commas
constexpr size_t kMaxDecodeLen = 5200;

// =============================================================================
// Binary Representation
// =============================================================================

struct CubeData {
  uint16_t ndim;
  uint8_t flags;
  uint8_t padding[5];
  double ll[kAbsoluteMaxDims];
  double ur[kAbsoluteMaxDims];
};

// Bit flags
constexpr uint8_t kFlagIsPoint = 0x01;

// Validate a raw cube buffer. Returns n_slots (the column's max dims) on
// success, or -1 if the buffer size is not a valid cube layout.
static inline int cube_n_slots(size_t bin_len) {
  if (bin_len < 8 || (bin_len - 8) % (2 * sizeof(double)) != 0) return -1;
  int n = static_cast<int>((bin_len - 8) / (2 * sizeof(double)));
  if (n > kAbsoluteMaxDims) return -1;
  return n;
}

// Read a CubeData from binary buffer. n_slots is the column's declared max
// dims (derived from bin_len via cube_n_slots). Unused slots are zeroed.
static void cube_from_buf(const unsigned char *buf, int n_slots, CubeData *c) {
  c->ndim = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
  // Clamp ndim to n_slots. A corrupt blob could encode a larger ndim than the
  // buffer can hold; without this, callers looping on c->ndim would read/write
  // beyond ll[kAbsoluteMaxDims] and ur[kAbsoluteMaxDims].
  if (c->ndim > static_cast<uint16_t>(n_slots))
    c->ndim = static_cast<uint16_t>(n_slots);
  c->flags = buf[2];
  memset(c->padding, 0, sizeof(c->padding));
  memcpy(c->ll, buf + 8, sizeof(double) * n_slots);
  memcpy(c->ur, buf + 8 + sizeof(double) * n_slots, sizeof(double) * n_slots);
  // Zero-fill unused slots so callers can loop up to kAbsoluteMaxDims safely.
  if (n_slots < kAbsoluteMaxDims) {
    memset(c->ll + n_slots, 0, sizeof(double) * (kAbsoluteMaxDims - n_slots));
    memset(c->ur + n_slots, 0, sizeof(double) * (kAbsoluteMaxDims - n_slots));
  }
}

// Write a CubeData to binary buffer. Writes exactly n_slots doubles for ll
// and ur (zero-filling slots beyond c->ndim). buf must be at least
// 8 + 2*n_slots*sizeof(double) bytes.
static void cube_to_buf(const CubeData *c, unsigned char *buf, int n_slots) {
  buf[0] = static_cast<uint8_t>(c->ndim & 0xFF);
  buf[1] = static_cast<uint8_t>((c->ndim >> 8) & 0xFF);
  buf[2] = c->flags;
  memset(buf + 3, 0, 5);
  memcpy(buf + 8, c->ll, sizeof(double) * n_slots);
  memcpy(buf + 8 + sizeof(double) * n_slots, c->ur, sizeof(double) * n_slots);
}

// Get ll[i], returning 0.0 for dims beyond ndim
static inline double cube_ll(const CubeData &c, int i) {
  return (i < c.ndim) ? c.ll[i] : 0.0;
}

// Get ur[i], returning ll[i] for dims beyond ndim (point extension)
static inline double cube_ur(const CubeData &c, int i) {
  if (i >= c.ndim) return 0.0;
  return (c.flags & kFlagIsPoint) ? c.ll[i] : c.ur[i];
}

// Normalize a cube: ensure ll[i] <= ur[i], set is_point if all corners equal
static void cube_normalize(CubeData *c) {
  bool is_point = true;
  for (int i = 0; i < c->ndim; i++) {
    if (c->ll[i] > c->ur[i]) std::swap(c->ll[i], c->ur[i]);
    if (c->ll[i] != c->ur[i]) is_point = false;  // Exact comparison is correct here: only set when values came in equal
  }
  if (is_point) {
    c->flags |= kFlagIsPoint;
  } else {
    c->flags &= ~kFlagIsPoint;
  }
}

// =============================================================================
// String Parsing
// =============================================================================

// Skip whitespace
static const char *skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    ++p;
  return p;
}

// Parse a floating-point number at *p, advance p past it
static bool parse_double(const char *&p, const char *end, double *out) {
  char buf[64];
  const char *start = p;
  // Allow leading sign, digits, '.', 'e'/'E', sign after 'e'
  if (p < end && (*p == '-' || *p == '+')) ++p;
  while (p < end && (*p >= '0' && *p <= '9')) ++p;
  if (p < end && *p == '.') {
    ++p;
    while (p < end && (*p >= '0' && *p <= '9')) ++p;
  }
  if (p < end && (*p == 'e' || *p == 'E')) {
    ++p;
    if (p < end && (*p == '-' || *p == '+')) ++p;
    while (p < end && (*p >= '0' && *p <= '9')) ++p;
  }
  size_t len = static_cast<size_t>(p - start);
  if (len == 0 || len >= sizeof(buf)) return false;
  memcpy(buf, start, len);
  buf[len] = '\0';
  char *endptr;
  *out = strtod(buf, &endptr);
  if (endptr != buf + len) return false;
  return std::isfinite(*out);  // reject NaN and Inf
}

// Parse a comma-separated list of doubles inside optional parens.
// Returns number of coords parsed, or -1 on error.
static int parse_coord_list(const char *&p, const char *end,
                            double *coords, int max_coords, bool *had_parens) {
  p = skip_ws(p, end);
  *had_parens = false;
  if (p < end && *p == '(') {
    *had_parens = true;
    ++p;
    p = skip_ws(p, end);
  }
  int n = 0;
  while (true) {
    p = skip_ws(p, end);
    if (p >= end) break;
    if (*had_parens && *p == ')') break;
    if (n > 0) {
      if (p >= end || *p != ',') return -1;
      ++p;
      p = skip_ws(p, end);
    }
    if (n >= max_coords) return -1;
    if (!parse_double(p, end, &coords[n])) return -1;
    ++n;
  }
  if (*had_parens) {
    p = skip_ws(p, end);
    if (p >= end || *p != ')') return -1;
    ++p;
  }
  return n;
}

// Parse the full cube string into a CubeData.
// Returns false on success, true on error.
static bool cube_parse(const char *from, size_t from_len, CubeData *c) {
  const char *p = from;
  const char *end = from + from_len;
  p = skip_ws(p, end);

  bool has_bracket = false;
  if (p < end && *p == '[') {
    has_bracket = true;
    ++p;
    p = skip_ws(p, end);
  }

  // Parse first corner
  double coords1[kAbsoluteMaxDims];
  bool had_parens1 = false;
  int n1 = parse_coord_list(p, end, coords1, kAbsoluteMaxDims, &had_parens1);
  if (n1 < 0 || n1 == 0) return true;

  p = skip_ws(p, end);

  // Check for second corner
  bool two_corners = false;
  double coords2[kAbsoluteMaxDims];
  int n2 = 0;

  if (p < end && *p == ',') {
    // Could be "x1,x2,x3" (n-dim point without parens) or two corners
    // Two corners requires both to have parens: "(x1,...),(y1,...)"
    // A bare comma here means we're still in the first corner's coord list
    // BUT: if had_parens1 is true, then we're done with first corner
    if (had_parens1) {
      two_corners = true;
      ++p;
      p = skip_ws(p, end);
      bool had_parens2 = false;
      n2 = parse_coord_list(p, end, coords2, kAbsoluteMaxDims, &had_parens2);
      if (n2 < 0) return true;
    } else {
      // No parens on first group and we see a comma — it's a bare n-dim point
      // like "x1,x2,x3". We already have n1=1 coord; need to continue parsing.
      // Re-parse from the beginning as a flat list.
      p = from;
      p = skip_ws(p, end);
      if (has_bracket) { ++p; p = skip_ws(p, end); }
      bool dummy;
      n1 = parse_coord_list(p, end, coords1, kAbsoluteMaxDims, &dummy);
      if (n1 < 0) return true;
      p = skip_ws(p, end);
    }
  }

  if (has_bracket) {
    p = skip_ws(p, end);
    if (p >= end || *p != ']') return true;
    ++p;
  }

  p = skip_ws(p, end);
  if (p != end) return true;  // trailing garbage

  // Build CubeData
  memset(c, 0, sizeof(*c));
  c->ndim = static_cast<uint16_t>(n1);
  for (int i = 0; i < n1; i++) c->ll[i] = coords1[i];

  if (two_corners) {
    if (n2 != n1) return true;  // dimension mismatch
    for (int i = 0; i < n1; i++) c->ur[i] = coords2[i];
  } else {
    // Point: ur == ll
    for (int i = 0; i < n1; i++) c->ur[i] = coords1[i];
  }

  cube_normalize(c);
  return false;
}

// =============================================================================
// String Formatting
// =============================================================================

// Format a double without trailing zeros (like PostgreSQL %g)
static int fmt_double(char *buf, size_t size, double v) {
  // Use %.15g: up to 15 significant digits, strips trailing zeros
  return snprintf(buf, size, "%.15g", v);
}

// Build the string representation of a CubeData into buf.
// Returns number of bytes written, or 0 on buffer overflow.
static size_t cube_format(const CubeData &c, char *buf, size_t size) {
  char *p = buf;
  char *end = buf + size;
  char tmp[32];
  bool is_point = (c.flags & kFlagIsPoint) || c.ndim == 0;

  // For a point: "(x1, x2, ..., xn)"
  // For a box:   "(x1, ..., xn),(y1, ..., yn)"

  auto write_corner = [&](const double *coords) -> bool {
    if (p >= end) return false;
    *p++ = '(';
    for (int i = 0; i < c.ndim; i++) {
      if (i > 0) {
        if (p + 2 > end) return false;
        *p++ = ',';
        *p++ = ' ';
      }
      int n = fmt_double(tmp, sizeof(tmp), coords[i]);
      if (n <= 0 || p + n > end) return false;
      memcpy(p, tmp, n);
      p += n;
    }
    if (p >= end) return false;
    *p++ = ')';
    return true;
  };

  if (!write_corner(c.ll)) return 0;
  if (!is_point) {
    if (p >= end) return 0;
    *p++ = ',';
    if (!write_corner(c.ur)) return 0;
  }

  return static_cast<size_t>(p - buf);
}

// =============================================================================
// Type Parameters (resolve_params_fn support)
// =============================================================================

// CubeParams holds the parsed type parameter: cube(n) → n is the max dims.
struct CubeParams {
  int64_t n;

  static CubeParams parse(const std::map<std::string, std::string> &params) {
    auto it = params.find("n");
    int64_t dim = kDefaultMaxDims;
    if (it != params.end()) dim = strtoll(it->second.c_str(), nullptr, 10);
    return CubeParams{dim};
  }

  // Inverse of parse: render a typed CubeParams back into the canonical
  // key/value string form. Used by paths that produce a typed P at runtime
  // (e.g., constant-string from_string) and need to publish the equivalent
  // string-form params back to the server. Mirrors what cube_int_to_params
  // emits for the integer-syntax case.
  static void to_strings(const CubeParams &p,
                         std::map<std::string, std::string> &out) {
    out["n"] = std::to_string(p.n);
  }
};

// Called when SQL uses bare integer syntax: cube(32) → {"n" → "32"}.
bool cube_int_to_params(int64_t value,
                        std::map<std::string, std::string> &params,
                        char *error_msg) {
  if (value < 1 || value > kAbsoluteMaxDims) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "cube: dimension must be 1..%d, got %" PRId64, kAbsoluteMaxDims, value);
    return true;
  }
  params["n"] = std::to_string(value);
  return false;
}

// Validates the params map and computes storage sizes for this instantiation.
bool cube_resolve_params(const std::map<std::string, std::string> &params,
                         ResolvedTypeParams *result,
                         char *error_msg) {
  auto it = params.find("n");
  if (it == params.end()) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN, "cube: missing dimension parameter 'n'");
    return true;
  }
  char *endptr;
  errno = 0;
  long long n = strtoll(it->second.c_str(), &endptr, 10);
  if (errno == ERANGE || *endptr != '\0' || n < 1 || n > kAbsoluteMaxDims) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "cube: dimension must be 1..%d, got '%s'",
             kAbsoluteMaxDims, it->second.c_str());
    return true;
  }
  result->persisted_length = static_cast<int64_t>(8 + 2 * n * sizeof(double));
  result->max_decode_buffer_length = static_cast<int64_t>(kMaxDecodeLen);
  return false;
}

// =============================================================================
// Type Encode/Decode
// =============================================================================

// When p is known, n_slots is taken from p.value().n; the parsed cube must
// have ndim <= n_slots. When p is unknown, n_slots is inferred from the
// parsed cube's ndim and written back via p.set(). The encoded layout always
// pads ll/ur out to n_slots doubles each.
void cube_encode(MaybeParams<CubeParams> &p, std::string_view from,
                 CustomResult out) {
  try {
    // Empty/whitespace-only input is invalid (matches PostgreSQL cube behavior)
    const char *q = from.data();
    const char *end = from.data() + from.size();
    q = skip_ws(q, end);
    if (q == end) {
      out.warning("cube: invalid cube string");
      return;
    }
    CubeData c;
    if (cube_parse(from.data(), from.size(), &c)) {
      out.warning("cube: invalid cube string");
      return;
    }

    int n_slots;
    if (p.is_known()) {
      int64_t n = p.value().n;
      if (n < 1 || n > kAbsoluteMaxDims) {
        out.warning("cube: invalid dimension");
        return;
      }
      n_slots = static_cast<int>(n);
      if (c.ndim > n_slots) {
        out.warning("cube: too many dimensions for declared cube(n)");
        return;
      }
    } else {
      n_slots = c.ndim < 1 ? 1 : c.ndim;
      if (n_slots > kAbsoluteMaxDims) {
        out.warning("cube: too many dimensions");
        return;
      }
      p.set(CubeParams{static_cast<int64_t>(n_slots)});
    }

    size_t needed = 8 + 2 * static_cast<size_t>(n_slots) * sizeof(double);
    auto buf = out.buffer();
    if (buf.size() < needed) {
      out.warning("cube: output buffer too small");
      return;
    }
    cube_to_buf(&c, buf.data(), n_slots);
    out.set_length(needed);
  } catch (...) {
    out.error("cube: internal error");
  }
}

void cube_decode(CustomArg in, StringResult out) {
  try {
    auto buf = in.value();
    int n_slots = cube_n_slots(buf.size());
    if (n_slots < 0) {
      out.error("cube_decode: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(buf.data(), n_slots, &c);
    auto dst = out.buffer();
    size_t n = cube_format(c, dst.data(), dst.size());
    if (n == 0) {
      out.error("cube_decode: output buffer too small");
      return;
    }
    out.set_length(n);
  } catch (...) {
    out.error("cube_decode: internal error");
  }
}

int cube_compare(CustomArg a, CustomArg b) {
  try {
    auto va = a.value();
    auto vb = b.value();
    int a_slots = cube_n_slots(va.size());
    int b_slots = cube_n_slots(vb.size());
    // Invalid/truncated buffer: establish a defined total order.
    // Invalid sorts before valid.
    if (a_slots < 0 || b_slots < 0) {
      if (a_slots < 0 && b_slots < 0) return 0;
      return (a_slots < 0) ? -1 : 1;
    }
    CubeData ca, cb;
    cube_from_buf(va.data(), a_slots, &ca);
    cube_from_buf(vb.data(), b_slots, &cb);
    int max_d = std::max(ca.ndim, cb.ndim);
    for (int i = 0; i < max_d; i++) {
      double la = cube_ll(ca, i), lb = cube_ll(cb, i);
      if (la < lb) return -1;
      if (la > lb) return 1;
      double ua = cube_ur(ca, i), ub = cube_ur(cb, i);
      if (ua < ub) return -1;
      if (ua > ub) return 1;
    }
    return 0;
  } catch (...) {
    return 0;
  }
}

// =============================================================================
// Helper: write cube result from CubeData
// =============================================================================

// Used by VDF and aggregate functions returning CUBE.
// out.params().n is the column's declared dimension (kDefaultMaxDims if no
// column context, since CubeParams::parse defaults to kDefaultMaxDims).
static void set_cube_result_typed(const CubeData &c,
                                  CustomResultWith<CubeParams> &out) {
  int n_slots = static_cast<int>(out.params().n);
  if (c.ndim > n_slots) {
    char msg[VEF_MAX_ERROR_LEN];
    snprintf(msg, sizeof(msg),
             "cube: value has %d dimensions but column allows %d",
             static_cast<int>(c.ndim), n_slots);
    out.error(msg);
    return;
  }
  size_t buf_size = 8 + 2 * static_cast<size_t>(n_slots) * sizeof(double);
  auto buf = out.buffer();
  cube_to_buf(&c, buf.data(), n_slots);
  out.set_length(buf_size);
}

// =============================================================================
// String Conversion VDFs
// =============================================================================

// CUBE_FROM_STRING(s STRING) → cube
// Self-sizing: parses the cube first to determine the number of dims, then
// writes exactly that many slots. Does not enforce column type limits (the
// type system handles assignment checking).
void cube_from_string_impl(StringArg arg, CustomResult out) {
  try {
    if (arg.is_null()) { out.set_null(); return; }
    auto sv = arg.value();
    // Reject empty/whitespace-only input.
    const char *p = sv.data();
    const char *end = sv.data() + sv.size();
    p = skip_ws(p, end);
    if (p == end) {
      out.error("cube_from_string: invalid cube string");
      return;
    }
    CubeData c;
    if (cube_parse(sv.data(), sv.size(), &c)) {
      out.error("cube_from_string: invalid cube string");
      return;
    }
    int n_slots = static_cast<int>(c.ndim);
    if (n_slots < 1) n_slots = 1;
    size_t buf_size = 8 + 2 * static_cast<size_t>(n_slots) * sizeof(double);
    auto buf = out.buffer();
    if (buf.size() < buf_size) {
      out.error("cube_from_string: output buffer too small");
      return;
    }
    cube_to_buf(&c, buf.data(), n_slots);
    out.set_length(buf_size);
  } catch (...) {
    out.error("cube_from_string: internal error");
  }
}

// CUBE_TO_STRING(c cube) → STRING
void cube_to_string_impl(CustomArg arg, StringResult out) {
  try {
    if (arg.is_null()) { out.set_null(); return; }
    auto span = arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      out.error("cube_to_string: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    auto buf = out.buffer();
    size_t n = cube_format(c, buf.data(), buf.size());
    if (n == 0) {
      out.error("cube_to_string: output buffer too small");
      return;
    }
    out.set_length(n);
  } catch (...) {
    out.error("cube_to_string: internal error");
  }
}

// =============================================================================
// Constructors
// =============================================================================

// CUBE_POINT(x REAL) → cube
void cube_point_impl(RealArg x, CustomResultWith<CubeParams> out) {
  try {
    if (x.is_null()) { out.set_null(); return; }
    CubeData c;
    memset(&c, 0, sizeof(c));
    c.ndim = 1;
    c.flags = kFlagIsPoint;
    c.ll[0] = c.ur[0] = x.value();
    set_cube_result_typed(c, out);
  } catch (...) {
    out.error("cube_point: internal error");
  }
}

// CUBE_BOX(lo REAL, hi REAL) → cube
void cube_box_impl(RealArg lo, RealArg hi, CustomResultWith<CubeParams> out) {
  try {
    if (lo.is_null() || hi.is_null()) { out.set_null(); return; }
    CubeData c;
    memset(&c, 0, sizeof(c));
    c.ndim = 1;
    c.ll[0] = lo.value();
    c.ur[0] = hi.value();
    cube_normalize(&c);
    set_cube_result_typed(c, out);
  } catch (...) {
    out.error("cube_box: internal error");
  }
}

// CUBE_POINT_ND(coords_csv STRING) → cube  (n-dim point from CSV)
void cube_point_nd_impl(StringArg arg, CustomResultWith<CubeParams> out) {
  try {
    if (arg.is_null()) { out.set_null(); return; }
    std::string_view sv = arg.value();
    CubeData c;
    memset(&c, 0, sizeof(c));
    int n = 0;
    size_t pos = 0;
    while (pos <= sv.size()) {
      size_t comma = sv.find(',', pos);
      std::string_view tok = sv.substr(pos, comma == std::string_view::npos
                                                ? std::string_view::npos
                                                : comma - pos);
      // Trim whitespace
      size_t s = tok.find_first_not_of(" \t\r\n");
      size_t e = tok.find_last_not_of(" \t\r\n");
      if (s == std::string_view::npos) {
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
        continue;
      }
      tok = tok.substr(s, e - s + 1);
      if (n >= kAbsoluteMaxDims) {
        char msg[VEF_MAX_ERROR_LEN];
        snprintf(msg, sizeof(msg),
                 "cube_point_nd: exceeds maximum %d dimensions", kAbsoluteMaxDims);
        out.error(msg);
        return;
      }
      char tmp[64];
      if (tok.size() >= sizeof(tmp)) {
        out.error("cube_point_nd: token too long");
        return;
      }
      memcpy(tmp, tok.data(), tok.size());
      tmp[tok.size()] = '\0';
      char *endptr;
      double v = strtod(tmp, &endptr);
      if (endptr != tmp + tok.size()) {
        char msg[VEF_MAX_ERROR_LEN];
        snprintf(msg, sizeof(msg), "cube_point_nd: invalid number '%s'", tmp);
        out.error(msg);
        return;
      }
      if (!std::isfinite(v)) {
        char msg[VEF_MAX_ERROR_LEN];
        snprintf(msg, sizeof(msg), "cube_point_nd: non-finite value '%s'", tmp);
        out.error(msg);
        return;
      }
      c.ll[n] = c.ur[n] = v;
      n++;
      if (comma == std::string_view::npos) break;
      pos = comma + 1;
    }
    if (n == 0) {
      out.error("cube_point_nd: no coordinates");
      return;
    }
    c.ndim = static_cast<uint16_t>(n);
    c.flags = kFlagIsPoint;
    set_cube_result_typed(c, out);
  } catch (...) {
    out.error("cube_point_nd: internal error");
  }
}

// CUBE_BOX_ND(lo_csv STRING, hi_csv STRING) → cube
void cube_box_nd_impl(StringArg lo_arg, StringArg hi_arg,
                      CustomResultWith<CubeParams> out) {
  try {
    if (lo_arg.is_null() || hi_arg.is_null()) { out.set_null(); return; }

    auto parse_csv = [&](std::string_view sv, double *o, int *n_out) -> bool {
      int n = 0;
      size_t pos = 0;
      while (pos <= sv.size()) {
        size_t comma = sv.find(',', pos);
        std::string_view tok = sv.substr(pos, comma == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : comma - pos);
        size_t s = tok.find_first_not_of(" \t\r\n");
        size_t e = tok.find_last_not_of(" \t\r\n");
        if (s == std::string_view::npos) {
          if (comma == std::string_view::npos) break;
          pos = comma + 1;
          continue;
        }
        tok = tok.substr(s, e - s + 1);
        if (n >= kAbsoluteMaxDims) return false;
        char tmp[64];
        if (tok.size() >= sizeof(tmp)) return false;
        memcpy(tmp, tok.data(), tok.size());
        tmp[tok.size()] = '\0';
        char *endptr;
        double v = strtod(tmp, &endptr);
        if (endptr != tmp + tok.size()) return false;
        if (!std::isfinite(v)) return false;  // reject NaN and Inf
        o[n++] = v;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
      }
      *n_out = n;
      return n > 0;
    };

    CubeData c;
    memset(&c, 0, sizeof(c));
    int n_lo = 0, n_hi = 0;
    if (!parse_csv(lo_arg.value(), c.ll, &n_lo)) {
      out.error("cube_box_nd: invalid lo coords");
      return;
    }
    if (!parse_csv(hi_arg.value(), c.ur, &n_hi)) {
      out.error("cube_box_nd: invalid hi coords");
      return;
    }
    if (n_lo != n_hi) {
      char msg[VEF_MAX_ERROR_LEN];
      snprintf(msg, sizeof(msg),
               "cube_box_nd: lo has %d dims, hi has %d dims", n_lo, n_hi);
      out.error(msg);
      return;
    }
    c.ndim = static_cast<uint16_t>(n_lo);
    cube_normalize(&c);
    set_cube_result_typed(c, out);
  } catch (...) {
    out.error("cube_box_nd: internal error");
  }
}

// CUBE_ADD_DIM(c cube, lo REAL, hi REAL) → cube
void cube_add_dim_impl(CustomArg c_arg, RealArg lo, RealArg hi,
                       CustomResultWith<CubeParams> out) {
  try {
    if (c_arg.is_null() || lo.is_null() || hi.is_null()) {
      out.set_null(); return;
    }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      out.error("cube_add_dim: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    if (c.ndim >= kAbsoluteMaxDims) {
      char msg[VEF_MAX_ERROR_LEN];
      snprintf(msg, sizeof(msg),
               "cube_add_dim: already at max %d dimensions", kAbsoluteMaxDims);
      out.error(msg);
      return;
    }
    int i = c.ndim;
    c.ll[i] = lo.value();
    c.ur[i] = hi.value();
    c.ndim++;
    cube_normalize(&c);
    set_cube_result_typed(c, out);
  } catch (...) {
    out.error("cube_add_dim: internal error");
  }
}

// =============================================================================
// Accessors
// =============================================================================

// CUBE_DIM(c cube) → INT
void cube_dim_impl(CustomArg c_arg, IntResult r) {
  try {
    if (c_arg.is_null()) { r.set_null(); return; }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      r.error("cube_dim: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    r.set(c.ndim);
  } catch (...) {
    r.error("cube_dim: internal error");
  }
}

// CUBE_LL_COORD(c cube, n INT) → REAL  (1-indexed)
void cube_ll_coord_impl(CustomArg c_arg, IntArg n_arg, RealResult r) {
  try {
    if (c_arg.is_null() || n_arg.is_null()) { r.set_null(); return; }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      r.error("cube_ll_coord: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    long long n = n_arg.value();
    if (n < 1 || n > c.ndim) { r.set_null(); return; }
    r.set(c.ll[n - 1]);
  } catch (...) {
    r.error("cube_ll_coord: internal error");
  }
}

// CUBE_UR_COORD(c cube, n INT) → REAL  (1-indexed)
void cube_ur_coord_impl(CustomArg c_arg, IntArg n_arg, RealResult r) {
  try {
    if (c_arg.is_null() || n_arg.is_null()) { r.set_null(); return; }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      r.error("cube_ur_coord: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    long long n = n_arg.value();
    if (n < 1 || n > c.ndim) { r.set_null(); return; }
    r.set((c.flags & kFlagIsPoint) ? c.ll[n - 1] : c.ur[n - 1]);
  } catch (...) {
    r.error("cube_ur_coord: internal error");
  }
}

// CUBE_IS_POINT(c cube) → INT  (1 = point, 0 = box)
void cube_is_point_impl(CustomArg c_arg, IntResult r) {
  try {
    if (c_arg.is_null()) { r.set_null(); return; }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      r.error("cube_is_point: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    r.set((c.flags & kFlagIsPoint) ? 1 : 0);
  } catch (...) {
    r.error("cube_is_point: internal error");
  }
}

// CUBE_COORD(c cube, n INT) → REAL
// n = 1..ndim → lower-left; n = ndim+1..2*ndim → upper-right  (like -> operator)
void cube_coord_impl(CustomArg c_arg, IntArg n_arg, RealResult r) {
  try {
    if (c_arg.is_null() || n_arg.is_null()) { r.set_null(); return; }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      r.error("cube_coord: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    long long n = n_arg.value();
    if (n < 1 || n > 2 * c.ndim) { r.set_null(); return; }
    if (n <= c.ndim) {
      r.set(c.ll[n - 1]);
    } else {
      r.set((c.flags & kFlagIsPoint) ? c.ll[n - c.ndim - 1]
                                     : c.ur[n - c.ndim - 1]);
    }
  } catch (...) {
    r.error("cube_coord: internal error");
  }
}

// =============================================================================
// Predicates
// =============================================================================

// Returns the effective working dimension count (max of both)
static int max_dim(const CubeData &a, const CubeData &b) {
  return std::max(a.ndim, b.ndim);
}

// CUBE_OVERLAPS(a cube, b cube) → INT  (1=overlap, 0=no overlap)
void cube_overlaps_impl(CustomArg a_arg, CustomArg b_arg, IntResult r) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { r.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      r.error("cube_overlaps: invalid input");
      return;
    }
    CubeData a, b;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    int nd = max_dim(a, b);
    for (int i = 0; i < nd; i++) {
      double all = cube_ll(a, i), aur = cube_ur(a, i);
      double bll = cube_ll(b, i), bur = cube_ur(b, i);
      if (all > bur || bll > aur) { r.set(0); return; }
    }
    r.set(1);
  } catch (...) {
    r.error("cube_overlaps: internal error");
  }
}

// CUBE_CONTAINS(a cube, b cube) → INT  (1 if a contains b)
void cube_contains_impl(CustomArg a_arg, CustomArg b_arg, IntResult r) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { r.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      r.error("cube_contains: invalid input");
      return;
    }
    CubeData a, b;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    int nd = max_dim(a, b);
    for (int i = 0; i < nd; i++) {
      if (cube_ll(a, i) > cube_ll(b, i) || cube_ur(a, i) < cube_ur(b, i)) {
        r.set(0); return;
      }
    }
    r.set(1);
  } catch (...) {
    r.error("cube_contains: internal error");
  }
}

// CUBE_CONTAINED_BY(a cube, b cube) → INT  (1 if a is inside b)
void cube_contained_by_impl(CustomArg a_arg, CustomArg b_arg, IntResult r) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { r.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      r.error("cube_contained_by: invalid input");
      return;
    }
    // contained_by(a, b) ≡ contains(b, a)
    CubeData a, b;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    int nd = max_dim(a, b);
    for (int i = 0; i < nd; i++) {
      if (cube_ll(b, i) > cube_ll(a, i) || cube_ur(b, i) < cube_ur(a, i)) {
        r.set(0); return;
      }
    }
    r.set(1);
  } catch (...) {
    r.error("cube_contained_by: internal error");
  }
}

// =============================================================================
// Distance Functions
// =============================================================================

// Gap between two intervals [a_lo, a_hi] and [b_lo, b_hi] on one dimension.
// Returns 0 if they overlap, positive gap otherwise.
static inline double interval_gap(double a_lo, double a_hi,
                                   double b_lo, double b_hi) {
  double gap = std::max(a_lo - b_hi, b_lo - a_hi);
  return gap > 0.0 ? gap : 0.0;
}

// CUBE_DISTANCE(a, b) → REAL  (Euclidean / L2)
void cube_distance_impl(CustomArg a_arg, CustomArg b_arg, RealResult r) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { r.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      r.error("cube_distance: invalid input");
      return;
    }
    CubeData a, b;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    int nd = max_dim(a, b);
    double sum = 0.0;
    for (int i = 0; i < nd; i++) {
      double g = interval_gap(cube_ll(a, i), cube_ur(a, i),
                              cube_ll(b, i), cube_ur(b, i));
      sum += g * g;
    }
    r.set(sqrt(sum));
  } catch (...) {
    r.error("cube_distance: internal error");
  }
}

// CUBE_TAXICAB_DISTANCE(a, b) → REAL  (L1 / Manhattan)
void cube_taxicab_distance_impl(CustomArg a_arg, CustomArg b_arg, RealResult r) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { r.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      r.error("cube_taxicab_distance: invalid input");
      return;
    }
    CubeData a, b;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    int nd = max_dim(a, b);
    double sum = 0.0;
    for (int i = 0; i < nd; i++) {
      sum += interval_gap(cube_ll(a, i), cube_ur(a, i),
                          cube_ll(b, i), cube_ur(b, i));
    }
    r.set(sum);
  } catch (...) {
    r.error("cube_taxicab_distance: internal error");
  }
}

// CUBE_CHEBYSHEV_DISTANCE(a, b) → REAL  (L-inf / Chebyshev)
void cube_chebyshev_distance_impl(CustomArg a_arg, CustomArg b_arg,
                                   RealResult r) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { r.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      r.error("cube_chebyshev_distance: invalid input");
      return;
    }
    CubeData a, b;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    int nd = max_dim(a, b);
    double mx = 0.0;
    for (int i = 0; i < nd; i++) {
      double g = interval_gap(cube_ll(a, i), cube_ur(a, i),
                              cube_ll(b, i), cube_ur(b, i));
      if (g > mx) mx = g;
    }
    r.set(mx);
  } catch (...) {
    r.error("cube_chebyshev_distance: internal error");
  }
}

// =============================================================================
// Geometry Functions
// =============================================================================

// CUBE_UNION(a, b) → cube
void cube_union_impl(CustomArg a_arg, CustomArg b_arg,
                     CustomResultWith<CubeParams> out) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { out.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      out.error("cube_union: invalid input");
      return;
    }
    CubeData a, b, result;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    memset(&result, 0, sizeof(result));
    result.ndim = static_cast<uint16_t>(max_dim(a, b));
    for (int i = 0; i < result.ndim; i++) {
      result.ll[i] = std::min(cube_ll(a, i), cube_ll(b, i));
      result.ur[i] = std::max(cube_ur(a, i), cube_ur(b, i));
    }
    cube_normalize(&result);
    set_cube_result_typed(result, out);
  } catch (...) {
    out.error("cube_union: internal error");
  }
}

// CUBE_INTER(a, b) → cube
void cube_inter_impl(CustomArg a_arg, CustomArg b_arg,
                     CustomResultWith<CubeParams> out) {
  try {
    if (a_arg.is_null() || b_arg.is_null()) { out.set_null(); return; }
    auto as = a_arg.value(), bs = b_arg.value();
    int a_slots = cube_n_slots(as.size()), b_slots = cube_n_slots(bs.size());
    if (a_slots < 0 || b_slots < 0) {
      out.error("cube_inter: invalid input");
      return;
    }
    CubeData a, b, result;
    cube_from_buf(as.data(), a_slots, &a);
    cube_from_buf(bs.data(), b_slots, &b);
    memset(&result, 0, sizeof(result));
    result.ndim = static_cast<uint16_t>(max_dim(a, b));
    bool is_point = true;
    for (int i = 0; i < result.ndim; i++) {
      result.ll[i] = std::max(cube_ll(a, i), cube_ll(b, i));
      result.ur[i] = std::min(cube_ur(a, i), cube_ur(b, i));
      // Do NOT swap ll/ur when ll > ur: a degenerate result (non-overlapping
      // cubes) is valid and must be preserved, matching PostgreSQL behavior.
      if (result.ll[i] != result.ur[i]) is_point = false;
    }
    if (is_point) result.flags |= kFlagIsPoint;
    set_cube_result_typed(result, out);
  } catch (...) {
    out.error("cube_inter: internal error");
  }
}

// CUBE_ENLARGE(c cube, radius REAL, n_dims INT) → cube
void cube_enlarge_impl(CustomArg c_arg, RealArg r_arg, IntArg n_arg,
                       CustomResultWith<CubeParams> out) {
  try {
    if (c_arg.is_null() || r_arg.is_null() || n_arg.is_null()) {
      out.set_null(); return;
    }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      out.error("cube_enlarge: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);
    double radius = r_arg.value();
    // Negative radius shrinks the cube. If shrinkage inverts lo and hi,
    // both collapse to the midpoint (see below). Matches PostgreSQL behavior.
    long long n = n_arg.value();
    if (n < 0 || n > kAbsoluteMaxDims) {
      char msg[VEF_MAX_ERROR_LEN];
      snprintf(msg, sizeof(msg),
               "cube_enlarge: n_dims %lld out of range (0..%d)", n, kAbsoluteMaxDims);
      out.error(msg);
      return;
    }

    CubeData result;
    memset(&result, 0, sizeof(result));
    result.ndim = static_cast<uint16_t>(std::max(static_cast<long long>(c.ndim), n));

    for (int i = 0; i < result.ndim; i++) {
      double lo = cube_ll(c, i);
      double hi = cube_ur(c, i);
      if (i < n) {
        double new_lo = lo - radius;
        double new_hi = hi + radius;
        if (new_lo > new_hi) {
          // Shrink beyond center: both set to midpoint
          double mid = (lo + hi) / 2.0;
          result.ll[i] = mid;
          result.ur[i] = mid;
        } else {
          result.ll[i] = new_lo;
          result.ur[i] = new_hi;
        }
      } else {
        result.ll[i] = lo;
        result.ur[i] = hi;
      }
    }
    cube_normalize(&result);
    set_cube_result_typed(result, out);
  } catch (...) {
    out.error("cube_enlarge: internal error");
  }
}

// CUBE_SUBSET(c cube, dims_csv STRING) → cube
// dims_csv: comma-separated 1-indexed dimension numbers
void cube_subset_impl(CustomArg c_arg, StringArg dims_arg,
                      CustomResultWith<CubeParams> out) {
  try {
    if (c_arg.is_null() || dims_arg.is_null()) { out.set_null(); return; }
    auto span = c_arg.value();
    int n_slots = cube_n_slots(span.size());
    if (n_slots < 0) {
      out.error("cube_subset: invalid input");
      return;
    }
    CubeData c;
    cube_from_buf(span.data(), n_slots, &c);

    // Parse dimension list
    int dims[kAbsoluteMaxDims];
    int n_dims = 0;
    std::string_view sv = dims_arg.value();
    size_t pos = 0;
    while (pos <= sv.size()) {
      size_t comma = sv.find(',', pos);
      std::string_view tok = sv.substr(pos, comma == std::string_view::npos
                                                ? std::string_view::npos
                                                : comma - pos);
      size_t s = tok.find_first_not_of(" \t\r\n");
      if (s != std::string_view::npos) {
        size_t e = tok.find_last_not_of(" \t\r\n");
        tok = tok.substr(s, e - s + 1);
        char tmp[16];
        if (tok.size() >= sizeof(tmp)) {
          out.error("cube_subset: dim token too long");
          return;
        }
        memcpy(tmp, tok.data(), tok.size());
        tmp[tok.size()] = '\0';
        char *endptr;
        errno = 0;
        long dim = strtol(tmp, &endptr, 10);
        if (errno == ERANGE || endptr != tmp + tok.size() || dim < 1 || dim > c.ndim) {
          char msg[VEF_MAX_ERROR_LEN];
          snprintf(msg, sizeof(msg),
                   "cube_subset: dim %ld out of range (1..%d)", dim, c.ndim);
          out.error(msg);
          return;
        }
        if (n_dims >= kAbsoluteMaxDims) {
          out.error("cube_subset: too many dims in list");
          return;
        }
        dims[n_dims++] = static_cast<int>(dim - 1);  // convert to 0-indexed
      }
      if (comma == std::string_view::npos) break;
      pos = comma + 1;
    }
    if (n_dims == 0) {
      out.error("cube_subset: empty dim list");
      return;
    }

    CubeData result;
    memset(&result, 0, sizeof(result));
    result.ndim = static_cast<uint16_t>(n_dims);
    for (int i = 0; i < n_dims; i++) {
      result.ll[i] = c.ll[dims[i]];
      result.ur[i] = (c.flags & kFlagIsPoint) ? c.ll[dims[i]] : c.ur[dims[i]];
    }
    cube_normalize(&result);
    set_cube_result_typed(result, out);
  } catch (...) {
    out.error("cube_subset: internal error");
  }
}

// =============================================================================
// Aggregate Functions
// =============================================================================

// CUBE_AGG(c cube) → cube
// Computes the bounding box over a set of cube values (equivalent to folding
// cube_union across all rows). Returns NULL for empty groups.

using CubeAggState = std::optional<CubeData>;

void cube_agg_clear(CubeAggState &state) { state = std::nullopt; }

void cube_agg_accumulate(CubeAggState &state, CustomArg arg) {
  if (arg.is_null()) return;
  auto span = arg.value();
  int n_slots = cube_n_slots(span.size());
  if (n_slots < 0) return;
  CubeData incoming;
  cube_from_buf(span.data(), n_slots, &incoming);
  if (!state.has_value()) {
    state = incoming;
    return;
  }
  CubeData &cur = *state;
  int nd = std::max(cur.ndim, incoming.ndim);
  for (int i = 0; i < nd; i++) {
    double ll_i = cube_ll(cur, i);
    double ur_i = cube_ur(cur, i);
    cur.ll[i] = std::min(ll_i, cube_ll(incoming, i));
    cur.ur[i] = std::max(ur_i, cube_ur(incoming, i));
  }
  cur.ndim = static_cast<uint16_t>(nd);
  cube_normalize(&cur);
}

void cube_agg_result(const CubeAggState &state,
                     CustomResultWith<CubeParams> out) {
  if (!state.has_value()) { out.set_null(); return; }
  set_cube_result_typed(*state, out);
}

// CUBE_SCALAR_AGG(x REAL) → cube
// Builds a 1-D bounding interval [min(x), max(x)] over a set of float values.
// Matches PostgreSQL's cube_agg(float8) behavior. Returns NULL for empty groups.
// The server skips accumulate for NULL input rows; no explicit null check needed.

struct CubeScalarAggState {
  bool initialized{false};
  double lo{0.0};
  double hi{0.0};
};

void cube_scalar_agg_clear(CubeScalarAggState &state) { state = {}; }

void cube_scalar_agg_accumulate(CubeScalarAggState &state, RealArg arg) {
  double v = arg.value();
  if (!state.initialized) {
    state.lo = state.hi = v;
    state.initialized = true;
    return;
  }
  if (v < state.lo) state.lo = v;
  if (v > state.hi) state.hi = v;
}

void cube_scalar_agg_result(const CubeScalarAggState &state,
                            CustomResultWith<CubeParams> out) {
  if (!state.initialized) { out.set_null(); return; }
  CubeData c;
  memset(&c, 0, sizeof(c));
  c.ndim = 1;
  c.ll[0] = state.lo;
  c.ur[0] = state.hi;
  cube_normalize(&c);
  set_cube_result_typed(c, out);
}

// =============================================================================
// Extension Registration
// =============================================================================

constexpr auto CUBE =
    ::vsql::make_type<kCubeName>()
        .persisted_length(-1)
        .max_decode_buffer_length(static_cast<int64_t>(kMaxDecodeLen))
        .max_persisted_length(static_cast<int64_t>(kMaxStorageSize))
        .params<CubeParams, &CubeParams::parse, &CubeParams::to_strings>()
        .int_to_params<&cube_int_to_params>()
        .resolve_params<&cube_resolve_params>()
        .from_string<&cube_encode>()
        .to_string<&cube_decode>()
        .compare<&cube_compare>()
        .intrinsic_default_str("(0)")
        .build();

using namespace ::vsql;

VEF_GENERATE_ENTRY_POINTS(
  make_extension()

    // Custom type
    .type(CUBE)

    // String conversion functions
    .func(make_func<&cube_from_string_impl>("cube_from_string")
      .returns(CUBE)
      .param(STRING)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_to_string_impl>("cube_to_string")
      .returns(STRING)
      .param(CUBE)
      .buffer_size(kMaxDecodeLen)
      .deterministic()
      .build())

    // Constructors
    .func(make_func<&cube_point_impl>("cube_point")
      .returns(CUBE)
      .param(REAL)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_box_impl>("cube_box")
      .returns(CUBE)
      .param(REAL)
      .param(REAL)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_point_nd_impl>("cube_point_nd")
      .returns(CUBE)
      .param(STRING)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_box_nd_impl>("cube_box_nd")
      .returns(CUBE)
      .param(STRING)
      .param(STRING)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_add_dim_impl>("cube_add_dim")
      .returns(CUBE)
      .param(CUBE)
      .param(REAL)
      .param(REAL)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())

    // Accessors
    .func(make_func<&cube_dim_impl>("cube_dim")
      .returns(INT)
      .param(CUBE)
      .deterministic()
      .build())
    .func(make_func<&cube_ll_coord_impl>("cube_ll_coord")
      .returns(REAL)
      .param(CUBE)
      .param(INT)
      .deterministic()
      .build())
    .func(make_func<&cube_ur_coord_impl>("cube_ur_coord")
      .returns(REAL)
      .param(CUBE)
      .param(INT)
      .deterministic()
      .build())
    .func(make_func<&cube_is_point_impl>("cube_is_point")
      .returns(INT)
      .param(CUBE)
      .deterministic()
      .build())
    .func(make_func<&cube_coord_impl>("cube_coord")
      .returns(REAL)
      .param(CUBE)
      .param(INT)
      .deterministic()
      .build())

    // Predicates
    .func(make_func<&cube_overlaps_impl>("cube_overlaps")
      .returns(INT)
      .param(CUBE)
      .param(CUBE)
      .deterministic()
      .build())
    .func(make_func<&cube_contains_impl>("cube_contains")
      .returns(INT)
      .param(CUBE)
      .param(CUBE)
      .deterministic()
      .build())
    .func(make_func<&cube_contained_by_impl>("cube_contained_by")
      .returns(INT)
      .param(CUBE)
      .param(CUBE)
      .deterministic()
      .build())

    // Distance
    .func(make_func<&cube_distance_impl>("cube_distance")
      .returns(REAL)
      .param(CUBE)
      .param(CUBE)
      .deterministic()
      .build())
    .func(make_func<&cube_taxicab_distance_impl>("cube_taxicab_distance")
      .returns(REAL)
      .param(CUBE)
      .param(CUBE)
      .deterministic()
      .build())
    .func(make_func<&cube_chebyshev_distance_impl>("cube_chebyshev_distance")
      .returns(REAL)
      .param(CUBE)
      .param(CUBE)
      .deterministic()
      .build())

    // Geometry
    .func(make_func<&cube_union_impl>("cube_union")
      .returns(CUBE)
      .param(CUBE)
      .param(CUBE)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_inter_impl>("cube_inter")
      .returns(CUBE)
      .param(CUBE)
      .param(CUBE)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_enlarge_impl>("cube_enlarge")
      .returns(CUBE)
      .param(CUBE)
      .param(REAL)
      .param(INT)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())
    .func(make_func<&cube_subset_impl>("cube_subset")
      .returns(CUBE)
      .param(CUBE)
      .param(STRING)
      .buffer_size(kMaxStorageSize)
      .deterministic()
      .build())

    // Aggregates
    .func(make_aggregate_func<CubeAggState, &cube_agg_result>("cube_agg")
      .returns(CUBE)
      .param(CUBE)
      .clear<&cube_agg_clear>()
      .accumulate<&cube_agg_accumulate>()
      .buffer_size(kMaxStorageSize)
      .build())
    .func(make_aggregate_func<CubeScalarAggState, &cube_scalar_agg_result>(
              "cube_scalar_agg")
      .returns(CUBE)
      .param(REAL)
      .clear<&cube_scalar_agg_clear>()
      .accumulate<&cube_scalar_agg_accumulate>()
      .buffer_size(kMaxStorageSize)
      .build())
)
