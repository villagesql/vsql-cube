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

#include "cube_rtree.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace cube_rtree {
namespace {

using Bytes = std::vector<unsigned char>;
using TrxRef = Segment::TrxRef;
using KeyPartData = IndexScanKey::KeyPartData;
using vsql::preview_storage::Error;

// Page formats. All offsets are relative to the start of the page DATA area
// (Page::HEADER_SIZE), and all integers and doubles the index writes are
// little-endian. Cube values are copied as the column stores them.
//
// Metadata, on the segment root page after the segment header:
//   [0] magic  [1] version  [2] height  [3] unused
//   [4..7] root page  [8..11] dims  [12..15] key length
//
// Tree node:
//   [0] magic  [1] level (0 = leaf)  [2..3] entry count  [4..7] unused
//   [8..] entries, each geo.leaf_slot or geo.node_slot bytes
//
// Leaf entry:
//   [0..7] inserting trx  [8..15] deleting trx (0 = not delete-marked)
//   [16..17] key length  [18..] key bytes, zero-padded to geo.key_len
//   then per primary key part: [2-byte length][bytes, zero-padded to max]
//
// Internal entry:
//   [0..3] child page  [4..] dims lower bounds, then dims upper bounds
constexpr uint8_t kMetaMagic = 0xC8;
constexpr uint8_t kMetaVersion = 1;
constexpr uint32_t kMetaOff = Segment::NUM_SEGMENTS_SIZE + Segment::HEADER_SIZE;
constexpr uint32_t kMetaLen = 16;

constexpr uint8_t kNodeMagic = 0xC7;
constexpr uint32_t kNodeHeaderLen = 8;

constexpr uint32_t kLeafInsTrx = 0;
constexpr uint32_t kLeafDelTrx = 8;
constexpr uint32_t kLeafKeyLen = 16;
constexpr uint32_t kLeafKey = 18;

constexpr uint32_t kNodeChild = 0;
constexpr uint32_t kNodeBox = 4;

// A node must hold enough entries for a split to leave both halves non-empty.
constexpr uint32_t kMinCapacity = 4;
constexpr uint32_t kMaxCapacity = 0xFFFF;
// Bounds a corrupt tree; a valid tree of 4-entry nodes stays far below it.
constexpr size_t kMaxHeight = 64;

// Cube binary layout, as in vsql_cube.cc: [ndim:2][flags:1][pad:5][ll][ur].
constexpr uint32_t kCubeHeaderLen = 8;
constexpr uint32_t kCubeMaxDims = 100;
constexpr uint8_t kCubeFlagIsPoint = 0x01;

// Results a scan collects per batch when the server gives no limit, and the
// largest batch a refill grows to.
constexpr uint32_t kDefaultBatch = 64;
constexpr uint32_t kMaxBatch = 1u << 16;

Page::Offset data_off(uint32_t off) {
  return static_cast<Page::Offset>(Page::HEADER_SIZE + off);
}

void put_u16(unsigned char *p, uint32_t v) {
  p[0] = static_cast<unsigned char>(v & 0xff);
  p[1] = static_cast<unsigned char>((v >> 8) & 0xff);
}

uint32_t get_u16(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
}

void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

uint32_t get_u32(const unsigned char *p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

void put_u64(unsigned char *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

uint64_t get_u64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

void put_double(unsigned char *p, double d) {
  uint64_t bits;
  memcpy(&bits, &d, sizeof(bits));
  put_u64(p, bits);
}

double get_double(const unsigned char *p) {
  const uint64_t bits = get_u64(p);
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}

// Cube coordinates are stored in native byte order (vsql_cube.cc memcpy's
// them), so they are read the same way.
double get_cube_coord(const unsigned char *p) {
  double d;
  memcpy(&d, p, sizeof(d));
  return d;
}

bool fail(char *err, uint32_t err_len, const char *fmt, ...) {
  if (err == nullptr || err_len == 0) return true;
  int n = snprintf(err, err_len, "cube rtree: ");
  if (n < 0 || static_cast<uint32_t>(n) >= err_len) return true;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err + n, err_len - n, fmt, ap);
  va_end(ap);
  return true;
}

bool fail_storage(char *err, uint32_t err_len, const char *what) {
  const std::string_view detail = vsql::preview_storage::last_error();
  return fail(err, err_len, "%s: %.*s", what, static_cast<int>(detail.size()),
              detail.data());
}

// An axis-aligned box: dimension i spans [lo[i], hi[i]].
struct Box {
  std::vector<double> lo;
  std::vector<double> hi;

  Box() = default;
  explicit Box(uint32_t dims) : lo(dims, 0.0), hi(dims, 0.0) {}

  uint32_t dims() const { return static_cast<uint32_t>(lo.size()); }
};

bool operator==(const Box &a, const Box &b) {
  return a.lo == b.lo && a.hi == b.hi;
}

double margin(const Box &b) {
  double m = 0.0;
  for (uint32_t i = 0; i < b.dims(); i++) m += b.hi[i] - b.lo[i];
  return m;
}

// Margin of the smallest box holding both a and b, which have equal dims.
double union_margin(const Box &a, const Box &b) {
  double m = 0.0;
  for (uint32_t i = 0; i < a.dims(); i++)
    m += std::max(a.hi[i], b.hi[i]) - std::min(a.lo[i], b.lo[i]);
  return m;
}

void extend(Box &a, const Box &b) {
  for (uint32_t i = 0; i < a.dims(); i++) {
    a.lo[i] = std::min(a.lo[i], b.lo[i]);
    a.hi[i] = std::max(a.hi[i], b.hi[i]);
  }
}

bool contains(const Box &outer, const Box &inner) {
  for (uint32_t i = 0; i < outer.dims(); i++) {
    if (!(outer.lo[i] <= inner.lo[i] && inner.hi[i] <= outer.hi[i]))
      return false;
  }
  return true;
}

// Number of coordinate slots in a persisted cube of len bytes, or -1 if len is
// not a valid cube length.
int cube_slots(uint32_t len) {
  if (len < kCubeHeaderLen || (len - kCubeHeaderLen) % (2 * sizeof(double)))
    return -1;
  const uint32_t n = (len - kCubeHeaderLen) / (2 * sizeof(double));
  return n > kCubeMaxDims ? -1 : static_cast<int>(n);
}

// Decodes a persisted cube into a box of dims dimensions, using the intervals
// vsql_cube.cc's cube_ll() and cube_ur() give: a point's upper corner is its
// lower corner, and every dimension at or past ndim is [0, 0]. Returns false if
// data is not a valid cube.
bool decode_cube(const unsigned char *data, uint32_t len, uint32_t dims,
                 Box &out) {
  const int slots = cube_slots(len);
  if (data == nullptr || slots < 0) return false;
  const uint32_t ndim =
      std::min<uint32_t>(get_u16(data), static_cast<uint32_t>(slots));
  const bool is_point = (data[2] & kCubeFlagIsPoint) != 0;
  out = Box(dims);
  for (uint32_t i = 0; i < dims && i < ndim; i++) {
    const double ll = get_cube_coord(data + kCubeHeaderLen + 8 * i);
    const double ur =
        is_point ? ll
                 : get_cube_coord(data + kCubeHeaderLen + 8 * slots + 8 * i);
    out.lo[i] = std::min(ll, ur);
    out.hi[i] = std::max(ll, ur);
  }
  return true;
}

// Distance between the query box q and box e under the metric of profile
// function fn. Dimensions missing from either box are [0, 0], so for a leaf
// entry the result equals the value of the SQL distance function, and for an
// internal entry it is a lower bound of every entry below it.
double distance(const Box &q, const Box &e, uint32_t fn) {
  const uint32_t n = std::max(q.dims(), e.dims());
  double acc = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    const double q_lo = i < q.dims() ? q.lo[i] : 0.0;
    const double q_hi = i < q.dims() ? q.hi[i] : 0.0;
    const double e_lo = i < e.dims() ? e.lo[i] : 0.0;
    const double e_hi = i < e.dims() ? e.hi[i] : 0.0;
    double gap = std::max(e_lo - q_hi, q_lo - e_hi);
    if (!(gap > 0.0)) gap = 0.0;
    switch (fn) {
      case kFnL1:
        acc += gap;
        break;
      case kFnLinf:
        if (gap > acc) acc = gap;
        break;
      default:
        acc += gap * gap;
        break;
    }
  }
  return fn == kFnL2 ? std::sqrt(acc) : acc;
}

bool compute_geometry(const Index &index, Space::Ref space, Geometry &geo,
                      char *err, uint32_t err_len) {
  if (index.get_num_key_cols() != 1)
    return fail(err, err_len, "an rtree index takes exactly one cube column");

  geo.key_len = index.get_max_col_len(0);
  const int slots = cube_slots(geo.key_len);
  if (slots <= 0)
    return fail(err, err_len, "unexpected cube key length %u", geo.key_len);
  geo.dims = static_cast<uint32_t>(slots);

  const uint32_t num_pk = index.get_primary_num_key_cols();
  if (num_pk == 0) return fail(err, err_len, "the table has no primary key");
  geo.pk_max.clear();
  uint64_t pk_len = 0;
  for (uint32_t i = 0; i < num_pk; i++) {
    const uint32_t max_len = index.get_primary_max_col_len(i);
    geo.pk_max.push_back(max_len);
    pk_len += 2 + uint64_t{max_len};
  }

  const uint64_t leaf_slot = kLeafKey + uint64_t{geo.key_len} + pk_len;
  const uint64_t node_slot = kNodeBox + 2 * uint64_t{geo.dims} * sizeof(double);
  const uint64_t page_data =
      Page::get_size(space) - Page::HEADER_SIZE - Page::TRAILER_SIZE;
  const uint64_t leaf_cap = (page_data - kNodeHeaderLen) / leaf_slot;
  const uint64_t node_cap = (page_data - kNodeHeaderLen) / node_slot;
  if (leaf_cap < kMinCapacity)
    return fail(err, err_len,
                "an index entry of %llu bytes is too large: a page must hold "
                "at least %u entries",
                static_cast<unsigned long long>(leaf_slot), kMinCapacity);
  if (node_cap < kMinCapacity)
    return fail(err, err_len, "cube(%u) is too wide for an rtree index",
                geo.dims);

  geo.leaf_slot = static_cast<uint32_t>(leaf_slot);
  geo.node_slot = static_cast<uint32_t>(node_slot);
  geo.leaf_cap = static_cast<uint32_t>(std::min<uint64_t>(leaf_cap, kMaxCapacity));
  geo.node_cap = static_cast<uint32_t>(std::min<uint64_t>(node_cap, kMaxCapacity));
  return false;
}

// A tree node copied out of its page.
struct Node {
  Page::Ref ref = Page::INVALID_REF;
  uint8_t level = 0;
  uint32_t count = 0;
  uint32_t slot_size = 0;
  Bytes slots;

  bool is_leaf() const { return level == 0; }
  const unsigned char *slot(uint32_t i) const {
    return slots.data() + size_t{i} * slot_size;
  }
  unsigned char *slot(uint32_t i) {
    return slots.data() + size_t{i} * slot_size;
  }
};

bool parse_node(const Page &page, const Geometry &geo, Node &node, char *err,
                uint32_t err_len) {
  const unsigned char *d = page.get_data() + Page::HEADER_SIZE;
  node.ref = page.get_ref();
  if (d[0] != kNodeMagic)
    return fail(err, err_len, "page %u is not an rtree node", node.ref);
  node.level = d[1];
  node.count = get_u16(d + 2);
  node.slot_size = node.is_leaf() ? geo.leaf_slot : geo.node_slot;
  const uint32_t cap = node.is_leaf() ? geo.leaf_cap : geo.node_cap;
  if (node.count > cap)
    return fail(err, err_len, "page %u holds %u entries, more than %u",
                node.ref, node.count, cap);
  const unsigned char *first = d + kNodeHeaderLen;
  node.slots.assign(first, first + size_t{node.count} * node.slot_size);
  return false;
}

// Copies node ref out of its page under a shared latch, in a mini-transaction
// that commits before returning.
bool read_node(const RtreeIndex &rt, Page::Ref ref, Node &node, char *err,
               uint32_t err_len) {
  MtrCtx mtr_ctx;
  const MtrCtx::Ref mtr = mtr_ctx.start();
  Page page;
  if (page.load(rt.space, ref, Page::Latch::SHARED, mtr) != Error::SUCCESS)
    return fail_storage(err, err_len, "load rtree node");
  const bool failed = parse_node(page, rt.geo, node, err, err_len);
  mtr_ctx.commit();
  return failed;
}

void write_node(Page &page, const Node &node, MtrCtx::Ref mtr) {
  Bytes image(kNodeHeaderLen + node.slots.size(), 0);
  image[0] = kNodeMagic;
  image[1] = node.level;
  put_u16(image.data() + 2, node.count);
  std::copy(node.slots.begin(), node.slots.end(),
            image.begin() + kNodeHeaderLen);
  page.write_string(data_off(0), image.data(), image.size(), mtr);
}

void write_slot(Page &page, const Node &node, uint32_t i, MtrCtx::Ref mtr) {
  page.write_string(data_off(kNodeHeaderLen + i * node.slot_size), node.slot(i),
                    node.slot_size, mtr);
}

void write_count(Page &page, const Node &node, MtrCtx::Ref mtr) {
  unsigned char buf[2];
  put_u16(buf, node.count);
  page.write_string(data_off(2), buf, sizeof(buf), mtr);
}

void write_meta(Page &meta, const RtreeIndex &rt, uint8_t height,
                Page::Ref root, MtrCtx::Ref mtr) {
  unsigned char image[kMetaLen] = {};
  image[0] = kMetaMagic;
  image[1] = kMetaVersion;
  image[2] = height;
  put_u32(image + 4, root);
  put_u32(image + 8, rt.geo.dims);
  put_u32(image + 12, rt.geo.key_len);
  meta.write_string(data_off(kMetaOff), image, sizeof(image), mtr);
}

Box node_entry_box(const Geometry &geo, const unsigned char *slot) {
  Box b(geo.dims);
  const unsigned char *p = slot + kNodeBox;
  for (uint32_t i = 0; i < geo.dims; i++) b.lo[i] = get_double(p + 8 * i);
  p += 8 * size_t{geo.dims};
  for (uint32_t i = 0; i < geo.dims; i++) b.hi[i] = get_double(p + 8 * i);
  return b;
}

void set_node_entry(const Geometry &geo, unsigned char *slot, Page::Ref child,
                    const Box &b) {
  put_u32(slot + kNodeChild, child);
  unsigned char *p = slot + kNodeBox;
  for (uint32_t i = 0; i < geo.dims; i++) put_double(p + 8 * i, b.lo[i]);
  p += 8 * size_t{geo.dims};
  for (uint32_t i = 0; i < geo.dims; i++) put_double(p + 8 * i, b.hi[i]);
}

Page::Ref node_entry_child(const unsigned char *slot) {
  return get_u32(slot + kNodeChild);
}

TrxRef leaf_ins_trx(const unsigned char *slot) {
  return get_u64(slot + kLeafInsTrx);
}

TrxRef leaf_del_trx(const unsigned char *slot) {
  return get_u64(slot + kLeafDelTrx);
}

bool leaf_entry_box(const Geometry &geo, const unsigned char *slot, Box &out) {
  return decode_cube(slot + kLeafKey, get_u16(slot + kLeafKeyLen), geo.dims,
                     out);
}

bool entry_box(const Geometry &geo, const Node &node, uint32_t i, Box &out) {
  if (node.is_leaf()) return leaf_entry_box(geo, node.slot(i), out);
  out = node_entry_box(geo, node.slot(i));
  return true;
}

// Builds the leaf entry for a key and primary key, with ins_trx as its
// inserting transaction and no delete mark. Entries are zero-padded, so two
// entries for the same key and primary key have equal bytes from kLeafKeyLen on.
bool make_leaf_entry(const Geometry &geo, TrxRef ins_trx, const KeyPartData &key,
                     const KeyPartData *pkey, Bytes &out, char *err,
                     uint32_t err_len) {
  if (key.length > geo.key_len)
    return fail(err, err_len, "cube value of %u bytes, expected at most %u",
                key.length, geo.key_len);
  if (pkey == nullptr) return fail(err, err_len, "missing primary key");
  out.assign(geo.leaf_slot, 0);
  put_u64(out.data() + kLeafInsTrx, ins_trx);
  put_u16(out.data() + kLeafKeyLen, key.length);
  if (key.length > 0) memcpy(out.data() + kLeafKey, key.data, key.length);
  size_t p = kLeafKey + size_t{geo.key_len};
  for (size_t i = 0; i < geo.pk_max.size(); i++) {
    if (pkey[i].length > geo.pk_max[i])
      return fail(err, err_len, "primary key part %zu of %u bytes is too long",
                  i, pkey[i].length);
    put_u16(out.data() + p, pkey[i].length);
    if (pkey[i].length > 0) memcpy(out.data() + p + 2, pkey[i].data, pkey[i].length);
    p += 2 + size_t{geo.pk_max[i]};
  }
  return false;
}

// True if leaf entry slot has the key and primary key of probe.
bool same_row_entry(const Geometry &geo, const unsigned char *slot,
                    const Bytes &probe) {
  return memcmp(slot + kLeafKeyLen, probe.data() + kLeafKeyLen,
                geo.leaf_slot - kLeafKeyLen) == 0;
}

// Picks the child of internal node n whose box grows least, by margin, to
// hold box; ties go to the child with the smaller box.
uint32_t choose_subtree(const Geometry &geo, const Node &n, const Box &box) {
  uint32_t best = 0;
  double best_grow = std::numeric_limits<double>::infinity();
  double best_margin = std::numeric_limits<double>::infinity();
  for (uint32_t i = 0; i < n.count; i++) {
    const Box b = node_entry_box(geo, n.slot(i));
    const double m = margin(b);
    const double grow = union_margin(b, box) - m;
    if (grow < best_grow || (grow == best_grow && m < best_margin)) {
      best = i;
      best_grow = grow;
      best_margin = m;
    }
  }
  return best;
}

// Guttman's quadratic split, with box margin in place of area: margins stay
// meaningful for points and in many dimensions, where volumes are 0 or
// overflow. Returns the group (0 or 1) of each box; each group gets at least
// min_fill boxes.
std::vector<uint8_t> quadratic_split(const std::vector<Box> &boxes,
                                     uint32_t min_fill) {
  const uint32_t n = static_cast<uint32_t>(boxes.size());
  std::vector<double> margins(n);
  for (uint32_t i = 0; i < n; i++) margins[i] = margin(boxes[i]);

  // Seeds: the pair that wastes most when put in one box.
  uint32_t seed[2] = {0, 1};
  double worst = -std::numeric_limits<double>::infinity();
  for (uint32_t i = 0; i < n; i++) {
    for (uint32_t j = i + 1; j < n; j++) {
      const double waste =
          union_margin(boxes[i], boxes[j]) - margins[i] - margins[j];
      if (waste > worst) {
        worst = waste;
        seed[0] = i;
        seed[1] = j;
      }
    }
  }

  constexpr uint8_t kUnassigned = 2;
  std::vector<uint8_t> group(n, kUnassigned);
  Box cover[2] = {boxes[seed[0]], boxes[seed[1]]};
  uint32_t count[2] = {1, 1};
  group[seed[0]] = 0;
  group[seed[1]] = 1;
  uint32_t remaining = n - 2;

  while (remaining > 0) {
    // A group that needs every remaining box to reach min_fill takes them.
    for (uint8_t g = 0; g < 2; g++) {
      if (count[g] + remaining <= min_fill) {
        for (uint32_t i = 0; i < n; i++) {
          if (group[i] == kUnassigned) group[i] = g;
        }
        return group;
      }
    }

    // Next: the box with the strongest preference for one group.
    uint32_t pick = n;
    double pick_diff = -1.0;
    double grow[2] = {0.0, 0.0};
    for (uint32_t i = 0; i < n; i++) {
      if (group[i] != kUnassigned) continue;
      const double g0 = union_margin(cover[0], boxes[i]) - margin(cover[0]);
      const double g1 = union_margin(cover[1], boxes[i]) - margin(cover[1]);
      const double diff = std::fabs(g0 - g1);
      if (pick == n || diff > pick_diff) {
        pick = i;
        pick_diff = diff;
        grow[0] = g0;
        grow[1] = g1;
      }
    }

    uint8_t g;
    if (grow[0] != grow[1]) {
      g = grow[0] < grow[1] ? 0 : 1;
    } else {
      const double m0 = margin(cover[0]);
      const double m1 = margin(cover[1]);
      if (m0 != m1)
        g = m0 < m1 ? 0 : 1;
      else
        g = count[0] <= count[1] ? 0 : 1;
    }
    group[pick] = g;
    extend(cover[g], boxes[pick]);
    count[g]++;
    remaining--;
  }
  return group;
}

Box cover_of(const std::vector<Box> &boxes, const std::vector<uint8_t> &group,
             uint8_t g) {
  Box cover;
  bool first = true;
  for (size_t i = 0; i < boxes.size(); i++) {
    if (group[i] != g) continue;
    if (first) {
      cover = boxes[i];
      first = false;
    } else {
      extend(cover, boxes[i]);
    }
  }
  return cover;
}

// Inserts leaf entry `entry`, whose cube is `box`, splitting full nodes on the
// way back up. All page changes are made in one mini-transaction. Caller holds
// rt.mutex exclusively.
bool tree_insert(RtreeIndex &rt, const Bytes &entry, const Box &box, char *err,
                 uint32_t err_len) {
  const Geometry &geo = rt.geo;
  MtrCtx mtr_ctx;
  const MtrCtx::Ref mtr = mtr_ctx.start();

  // The segment header on the metadata page allocates pages; latch it first.
  Page meta;
  if (meta.load(rt.space, rt.meta_page, Page::Latch::EXCLUSIVE, mtr) !=
      Error::SUCCESS)
    return fail_storage(err, err_len, "load rtree metadata");

  struct Step {
    std::unique_ptr<Page> page;
    Node node;
    // Entry of node that the insert descended through (internal nodes only).
    uint32_t child = 0;
  };
  std::vector<Step> path;
  Page::Ref ref = rt.root_page;
  for (;;) {
    if (path.size() >= kMaxHeight)
      return fail(err, err_len, "tree deeper than %zu levels", kMaxHeight);
    Step step;
    step.page = std::make_unique<Page>();
    if (step.page->load(rt.space, ref, Page::Latch::EXCLUSIVE, mtr) !=
        Error::SUCCESS)
      return fail_storage(err, err_len, "load rtree node");
    if (parse_node(*step.page, geo, step.node, err, err_len)) return true;
    const bool leaf = step.node.is_leaf();
    if (!leaf) {
      if (step.node.count == 0)
        return fail(err, err_len, "internal page %u is empty", ref);
      step.child = choose_subtree(geo, step.node, box);
      ref = node_entry_child(step.node.slot(step.child));
    }
    path.push_back(std::move(step));
    if (leaf) break;
  }

  // Splits run from the leaf up through every full node on the path. Allocate
  // their pages, and a new root if the root splits too, before changing any
  // page: a mini-transaction cannot be rolled back, so nothing may fail after
  // the first write. (The leaf, the only node whose entry boxes can fail to
  // decode, is also handled before any write.)
  size_t splits = 0;
  for (size_t level = path.size(); level-- > 0;) {
    const Node &node = path[level].node;
    if (node.count < (node.is_leaf() ? geo.leaf_cap : geo.node_cap)) break;
    splits++;
  }
  std::vector<std::unique_ptr<Page>> fresh;
  const size_t new_pages = splits + (splits == path.size() ? 1 : 0);
  if (new_pages > 0) {
    Segment::Ref seg = Segment::get_header(meta, 0);
    if (seg == nullptr) return fail(err, err_len, "segment header not found");
    for (size_t i = 0; i < new_pages; i++) {
      auto page = std::make_unique<Page>();
      if (page->load_new(seg, mtr) != Error::SUCCESS)
        return fail_storage(err, err_len, "allocate rtree page");
      fresh.push_back(std::move(page));
    }
  }
  size_t next_fresh = 0;

  // Walk back up. `pending` is an entry the current node must take: first the
  // new leaf entry, then the entry for the new page of each split.
  Bytes pending = entry;
  Box pending_box = box;
  bool has_pending = true;
  // Set when the child the insert went through was split, to the box of the
  // entries left on the child's page.
  bool child_split = false;
  Box child_box;

  for (size_t level = path.size(); level-- > 0;) {
    Step &step = path[level];
    Node &node = step.node;

    if (!node.is_leaf()) {
      // The child now also covers box, or, after a split, exactly child_box.
      const Box old_box = node_entry_box(geo, node.slot(step.child));
      Box new_box = old_box;
      if (child_split)
        new_box = child_box;
      else
        extend(new_box, box);
      if (!(new_box == old_box)) {
        set_node_entry(geo, node.slot(step.child),
                       node_entry_child(node.slot(step.child)), new_box);
        write_slot(*step.page, node, step.child, mtr);
      }
    }
    child_split = false;
    if (!has_pending) continue;

    const uint32_t cap = node.is_leaf() ? geo.leaf_cap : geo.node_cap;
    if (node.count < cap) {
      node.slots.insert(node.slots.end(), pending.begin(), pending.end());
      node.count++;
      write_slot(*step.page, node, node.count - 1, mtr);
      write_count(*step.page, node, mtr);
      has_pending = false;
      continue;
    }

    // Split the full node and the pending entry over this page and a new one.
    const uint32_t n = node.count + 1;
    std::vector<Box> boxes(n);
    for (uint32_t i = 0; i < node.count; i++) {
      if (!entry_box(geo, node, i, boxes[i]))
        return fail(err, err_len, "invalid cube value on page %u", node.ref);
    }
    boxes[node.count] = pending_box;
    const std::vector<uint8_t> group =
        quadratic_split(boxes, std::max<uint32_t>(1, n * 2 / 5));

    Node halves[2];
    for (Node &half : halves) {
      half.level = node.level;
      half.slot_size = node.slot_size;
    }
    for (uint32_t i = 0; i < n; i++) {
      const unsigned char *src = i < node.count ? node.slot(i) : pending.data();
      Node &half = halves[group[i]];
      half.slots.insert(half.slots.end(), src, src + node.slot_size);
      half.count++;
    }

    Page &sibling = *fresh[next_fresh++];
    halves[0].ref = node.ref;
    halves[1].ref = sibling.get_ref();
    write_node(*step.page, halves[0], mtr);
    write_node(sibling, halves[1], mtr);

    child_split = true;
    child_box = cover_of(boxes, group, 0);
    pending_box = cover_of(boxes, group, 1);
    pending.assign(geo.node_slot, 0);
    set_node_entry(geo, pending.data(), halves[1].ref, pending_box);
    has_pending = true;
  }

  if (has_pending) {
    // The root split: a new root takes the old root and its new sibling.
    Page &new_root = *fresh[next_fresh++];
    Node root;
    root.ref = new_root.get_ref();
    root.level = static_cast<uint8_t>(path.front().node.level + 1);
    root.slot_size = geo.node_slot;
    root.count = 2;
    root.slots.assign(2 * size_t{geo.node_slot}, 0);
    set_node_entry(geo, root.slot(0), path.front().node.ref, child_box);
    std::copy(pending.begin(), pending.end(), root.slots.begin() + geo.node_slot);
    write_node(new_root, root, mtr);
    const uint8_t height = static_cast<uint8_t>(rt.height + 1);
    write_meta(meta, rt, height, root.ref, mtr);
    mtr_ctx.commit();
    rt.root_page = root.ref;
    rt.height = height;
    return false;
  }

  mtr_ctx.commit();
  return false;
}

// Finds the leaf entry with the key and primary key of probe whose inserting
// and deleting transactions satisfy match(ins_trx, del_trx). Descends only into
// subtrees whose box contains box, the probe's cube. Caller holds rt.mutex.
template <typename Match>
bool find_entry(const RtreeIndex &rt, const Bytes &probe, const Box &box,
                Match match, Page::Ref *page, uint32_t *slot, bool *found,
                char *err, uint32_t err_len) {
  *found = false;
  std::vector<Page::Ref> stack{rt.root_page};
  while (!stack.empty()) {
    const Page::Ref ref = stack.back();
    stack.pop_back();
    Node node;
    if (read_node(rt, ref, node, err, err_len)) return true;
    for (uint32_t i = 0; i < node.count; i++) {
      const unsigned char *s = node.slot(i);
      if (!node.is_leaf()) {
        if (contains(node_entry_box(rt.geo, s), box))
          stack.push_back(node_entry_child(s));
        continue;
      }
      if (same_row_entry(rt.geo, s, probe) &&
          match(leaf_ins_trx(s), leaf_del_trx(s))) {
        *page = ref;
        *slot = i;
        *found = true;
        return false;
      }
    }
  }
  return false;
}

// Re-reads leaf entry `slot` of `page` under an exclusive latch and checks it
// still is the entry find_entry() returned. Caller holds rt.mutex exclusively,
// so it always is unless the tree is corrupt.
bool load_leaf_entry(const RtreeIndex &rt, Page &page, Page::Ref ref,
                     uint32_t slot, const Bytes &probe, Node &node,
                     MtrCtx::Ref mtr, char *err, uint32_t err_len) {
  if (page.load(rt.space, ref, Page::Latch::EXCLUSIVE, mtr) != Error::SUCCESS)
    return fail_storage(err, err_len, "load rtree leaf");
  if (parse_node(page, rt.geo, node, err, err_len)) return true;
  if (!node.is_leaf() || slot >= node.count ||
      !same_row_entry(rt.geo, node.slot(slot), probe))
    return fail(err, err_len, "entry moved on page %u", ref);
  return false;
}

bool set_delete_trx(const RtreeIndex &rt, Page::Ref ref, uint32_t slot,
                    const Bytes &probe, TrxRef del_trx, char *err,
                    uint32_t err_len) {
  MtrCtx mtr_ctx;
  const MtrCtx::Ref mtr = mtr_ctx.start();
  Page page;
  Node node;
  if (load_leaf_entry(rt, page, ref, slot, probe, node, mtr, err, err_len))
    return true;
  unsigned char buf[8];
  put_u64(buf, del_trx);
  page.write_string(
      data_off(kNodeHeaderLen + slot * node.slot_size + kLeafDelTrx), buf,
      sizeof(buf), mtr);
  mtr_ctx.commit();
  return false;
}

// Removes a leaf entry by moving the leaf's last entry into its slot. Bounding
// boxes above the leaf are left as they are: a box may be larger than its
// entries need, never smaller.
bool remove_entry(const RtreeIndex &rt, Page::Ref ref, uint32_t slot,
                  const Bytes &probe, char *err, uint32_t err_len) {
  MtrCtx mtr_ctx;
  const MtrCtx::Ref mtr = mtr_ctx.start();
  Page page;
  Node node;
  if (load_leaf_entry(rt, page, ref, slot, probe, node, mtr, err, err_len))
    return true;
  const uint32_t last = node.count - 1;
  if (slot != last) {
    memcpy(node.slot(slot), node.slot(last), node.slot_size);
    write_slot(page, node, slot, mtr);
  }
  node.count = last;
  write_count(page, node, mtr);
  mtr_ctx.commit();
  return false;
}

// Builds the probe entry and cube box for a DML call. *skip is set when the
// key is NULL, which the index does not store.
bool dml_probe(const RtreeIndex &rt, const KeyPartData *key_columns,
               const KeyPartData *pkey_columns, TrxRef ins_trx, Bytes &probe,
               Box &box, bool *skip, char *err, uint32_t err_len) {
  *skip = key_columns == nullptr || key_columns[0].data == nullptr;
  if (*skip) return false;
  if (make_leaf_entry(rt.geo, ins_trx, key_columns[0], pkey_columns, probe, err,
                      err_len))
    return true;
  if (!decode_cube(key_columns[0].data, key_columns[0].length, rt.geo.dims,
                   box))
    return fail(err, err_len, "invalid cube value");
  return false;
}

Index::StorageRef encode_storage_ref(Space::Ref space, Page::Ref meta) {
  return (static_cast<Index::StorageRef>(space) << 32) |
         static_cast<Index::StorageRef>(meta);
}

// One scan result: a copy of the leaf entry and its distance to the query.
struct Hit {
  double dist = 0.0;
  Bytes entry;
};

// Total order of scan results: by distance, then by entry bytes (primary key
// first), so a refill can resume strictly after the last result returned.
int compare_hits(const Geometry &geo, const Hit &a, const Hit &b) {
  if (a.dist < b.dist) return -1;
  if (a.dist > b.dist) return 1;
  const size_t pk = kLeafKey + size_t{geo.key_len};
  const int c = memcmp(a.entry.data() + pk, b.entry.data() + pk,
                       geo.leaf_slot - pk);
  if (c != 0) return c;
  return memcmp(a.entry.data(), b.entry.data(), pk);
}

struct Cursor {
  Cursor(RtreeIndex *rt_arg, const Index &index_arg)
      : rt(rt_arg), index(index_arg) {}

  RtreeIndex *rt;
  Index index;
  IndexScanDesc::ReadViewRef read_view = nullptr;
  bool locking = false;
  uint32_t fn = kFnL2;
  Box query;

  // Results of the current batch, in order; pos is the current one.
  std::vector<Hit> hits;
  size_t pos = 0;
  uint32_t batch = kDefaultBatch;
  // True once a batch came back short: nothing follows hits.
  bool exhausted = false;
  // Last result of the previous batch; the next batch starts after it.
  bool has_last = false;
  Hit last;
  // Number of hits in the batches before this one, so batch_start + pos
  // numbers every hit of the scan.
  uint64_t batch_start = 0;
  // Locking reads: the hit that validate() first accepted for each primary
  // key (keyed by the key's bytes in the entry). Several entries of one row
  // can pass validate(): entries left by a transaction that changed the row
  // to the same value more than once look identical to it. Only the first
  // may return the row.
  std::map<Bytes, uint64_t> validated;
};

bool entry_visible(const Cursor &c, TrxRef ins_trx, TrxRef del_trx) {
  if (c.locking) {
    // A locking read sees every entry except one whose delete has committed;
    // the server locks the row and validate() rechecks the entry.
    return del_trx == 0 || c.index.is_transaction_active(del_trx);
  }
  if (del_trx != 0 && c.index.is_trx_visible(c.read_view, del_trx))
    return false;
  return c.index.is_trx_visible(c.read_view, ins_trx);
}

// Collects the next c.batch results after c.last by best-first search: nodes
// and entries are taken in distance order from one queue, so each entry is
// returned once nothing still queued can be closer. The search restarts from
// the root for every batch and holds no page latch between batches.
bool refill(Cursor &c, char *err, uint32_t err_len) {
  const Geometry &geo = c.rt->geo;
  std::shared_lock<std::shared_mutex> lock(c.rt->mutex);

  struct Item {
    double dist;
    bool leaf;
    Page::Ref page;  // node to read, when !leaf
    size_t hit;      // index into pool, when leaf
  };
  std::vector<Hit> pool;
  // True when a comes after b; nodes come before entries at equal distance,
  // so every entry at that distance is queued before any is returned.
  auto later = [&](const Item &a, const Item &b) {
    if (a.dist != b.dist) return a.dist > b.dist;
    if (a.leaf != b.leaf) return a.leaf;
    if (!a.leaf) return a.page > b.page;
    return compare_hits(geo, pool[a.hit], pool[b.hit]) > 0;
  };
  std::vector<Item> queue;
  queue.push_back(Item{0.0, false, c.rt->root_page, 0});

  c.batch_start += c.hits.size();
  c.hits.clear();
  c.pos = 0;
  while (!queue.empty() && c.hits.size() < c.batch) {
    std::pop_heap(queue.begin(), queue.end(), later);
    const Item item = queue.back();
    queue.pop_back();

    if (item.leaf) {
      Hit &hit = pool[item.hit];
      if (c.has_last && compare_hits(geo, hit, c.last) <= 0) continue;
      c.hits.push_back(std::move(hit));
      continue;
    }

    Node node;
    if (read_node(*c.rt, item.page, node, err, err_len)) return true;
    for (uint32_t i = 0; i < node.count; i++) {
      const unsigned char *s = node.slot(i);
      Item next{0.0, node.is_leaf(), Page::INVALID_REF, 0};
      if (node.is_leaf()) {
        if (!entry_visible(c, leaf_ins_trx(s), leaf_del_trx(s))) continue;
        Box b;
        if (!leaf_entry_box(geo, s, b))
          return fail(err, err_len, "invalid cube value on page %u", node.ref);
        next.dist = distance(c.query, b, c.fn);
        next.hit = pool.size();
        pool.push_back(Hit{next.dist, Bytes(s, s + node.slot_size)});
      } else {
        next.dist = distance(c.query, node_entry_box(geo, s), c.fn);
        next.page = node_entry_child(s);
      }
      queue.push_back(next);
      std::push_heap(queue.begin(), queue.end(), later);
    }
  }

  c.exhausted = queue.empty();
  if (!c.hits.empty()) {
    c.has_last = true;
    c.last = c.hits.back();
  }
  return false;
}

}  // namespace

bool create(StorageCtx *ctx, const Index &index, Space::Ref space_ref,
            Segment::TrxRef trx_ref, char *err, uint32_t err_len) {
  RtreeIndex *rt = ctx->user();
  if (compute_geometry(index, space_ref, rt->geo, err, err_len)) return true;

  Page::Ref meta_page = Page::INVALID_REF;
  if (Segment::create(space_ref, 1, trx_ref, meta_page) != Error::SUCCESS)
    return fail_storage(err, err_len, "create rtree segment");
  rt->space = space_ref;
  rt->meta_page = meta_page;

  MtrCtx mtr_ctx;
  const MtrCtx::Ref mtr = mtr_ctx.start();
  Page meta;
  Page root;
  Segment::Ref seg = nullptr;
  bool failed = false;
  if (meta.load(space_ref, meta_page, Page::Latch::EXCLUSIVE, mtr) !=
      Error::SUCCESS) {
    failed = fail_storage(err, err_len, "load rtree metadata");
  } else if ((seg = Segment::get_header(meta, 0)) == nullptr) {
    failed = fail(err, err_len, "segment header not found");
  } else if (root.load_new(seg, mtr) != Error::SUCCESS) {
    failed = fail_storage(err, err_len, "allocate rtree root");
  } else {
    Node empty;
    empty.slot_size = rt->geo.leaf_slot;
    write_node(root, empty, mtr);
    write_meta(meta, *rt, 1, root.get_ref(), mtr);
  }
  mtr_ctx.commit();

  if (failed) {
    // Free the segment; the error being reported is the earlier one.
    (void)Segment::drop(space_ref, trx_ref, meta_page);
    return true;
  }
  rt->root_page = root.get_ref();
  rt->height = 1;
  ctx->set_ref(encode_storage_ref(space_ref, meta_page));
  return false;
}

bool drop(StorageCtx *ctx, const Index & /*index*/, Segment::TrxRef trx_ref,
          char *err, uint32_t err_len) {
  RtreeIndex *rt = ctx->user();
  std::unique_lock<std::shared_mutex> lock(rt->mutex);
  if (Segment::drop(rt->space, trx_ref, rt->meta_page) != Error::SUCCESS)
    return fail_storage(err, err_len, "drop rtree segment");
  rt->root_page = Page::INVALID_REF;
  rt->meta_page = Page::INVALID_REF;
  return false;
}

bool load(StorageCtx *ctx, const Index &index, Index::StorageRef storage_ref,
          char *err, uint32_t err_len) {
  RtreeIndex *rt = ctx->user();
  rt->space = static_cast<Space::Ref>(storage_ref >> 32);
  rt->meta_page = static_cast<Page::Ref>(storage_ref & 0xFFFFFFFFu);
  if (compute_geometry(index, rt->space, rt->geo, err, err_len)) return true;

  MtrCtx mtr_ctx;
  const MtrCtx::Ref mtr = mtr_ctx.start();
  Page meta;
  if (meta.load(rt->space, rt->meta_page, Page::Latch::SHARED, mtr) !=
      Error::SUCCESS)
    return fail_storage(err, err_len, "load rtree metadata");
  const unsigned char *m = meta.get_data() + Page::HEADER_SIZE + kMetaOff;
  const uint8_t magic = m[0];
  const uint8_t version = m[1];
  const uint8_t height = m[2];
  const Page::Ref root = get_u32(m + 4);
  const uint32_t dims = get_u32(m + 8);
  const uint32_t key_len = get_u32(m + 12);
  mtr_ctx.commit();

  if (magic != kMetaMagic || version != kMetaVersion)
    return fail(err, err_len, "page %u holds no rtree metadata", rt->meta_page);
  if (dims != rt->geo.dims || key_len != rt->geo.key_len)
    return fail(err, err_len,
                "index was built for cube(%u), the column is cube(%u)", dims,
                rt->geo.dims);
  if (height == 0 || height > kMaxHeight)
    return fail(err, err_len, "invalid tree height %u", height);
  rt->root_page = root;
  rt->height = height;
  ctx->set_ref(storage_ref);
  return false;
}

bool insert(StorageCtx *ctx, const Index & /*index*/, Segment::TrxRef trx_ref,
            IndexScanKey::KeyPartData *key_columns,
            IndexScanKey::KeyPartData *pkey_columns,
            IndexScanKey::KeyPartRef * /*key_ref*/, char *err,
            uint32_t err_len) {
  RtreeIndex *rt = ctx->user();
  Bytes entry;
  Box box;
  bool skip = false;
  if (dml_probe(*rt, key_columns, pkey_columns, trx_ref, entry, box, &skip, err,
                err_len))
    return true;
  if (skip) return false;
  std::unique_lock<std::shared_mutex> lock(rt->mutex);
  return tree_insert(*rt, entry, box, err, err_len);
}

bool mark_delete(StorageCtx *ctx, const Index & /*index*/,
                 Segment::TrxRef trx_ref, IndexScanKey::KeyPartRef * /*key_ref*/,
                 IndexScanKey::KeyPartData *key_columns,
                 IndexScanKey::KeyPartData *pkey_columns, bool delete_mark,
                 char *err, uint32_t err_len) {
  RtreeIndex *rt = ctx->user();
  Bytes probe;
  Box box;
  bool skip = false;
  if (dml_probe(*rt, key_columns, pkey_columns, 0, probe, box, &skip, err,
                err_len))
    return true;
  if (skip) return false;

  std::unique_lock<std::shared_mutex> lock(rt->mutex);
  Page::Ref page = Page::INVALID_REF;
  uint32_t slot = 0;
  bool found = false;
  if (delete_mark) {
    // A row has at most one entry for its current version: the one with no
    // delete mark.
    if (find_entry(
            *rt, probe, box, [](TrxRef, TrxRef del) { return del == 0; }, &page,
            &slot, &found, err, err_len))
      return true;
    if (!found) return fail(err, err_len, "no entry to delete-mark");
    return set_delete_trx(*rt, page, slot, probe, trx_ref, err, err_len);
  }

  // Rollback of trx_ref's delete mark. Undo runs newest change first, so it
  // must clear the mark trx_ref set last. When trx_ref marked several entries
  // with this key and primary key, an entry trx_ref inserted itself was marked
  // after any older entry for the row (the older one had to be marked before
  // trx_ref could insert the row again), so it is preferred. Entries that
  // trx_ref both inserted and marked are identical, so any of them will do.
  // Rollback cannot fail, so a missing entry is not an error.
  if (find_entry(
          *rt, probe, box,
          [trx_ref](TrxRef ins, TrxRef del) {
            return ins == trx_ref && del == trx_ref;
          },
          &page, &slot, &found, err, err_len))
    return true;
  if (!found &&
      find_entry(
          *rt, probe, box,
          [trx_ref](TrxRef, TrxRef del) { return del == trx_ref; }, &page,
          &slot, &found, err, err_len))
    return true;
  if (!found) return false;
  return set_delete_trx(*rt, page, slot, probe, 0, err, err_len);
}

bool purge(StorageCtx *ctx, const Index &index, Segment::TrxRef trx_ref,
           IndexScanKey::KeyPartRef * /*key_ref*/,
           IndexScanKey::KeyPartData *key_columns,
           IndexScanKey::KeyPartData *pkey_columns, char *err,
           uint32_t err_len) {
  RtreeIndex *rt = ctx->user();
  Bytes probe;
  Box box;
  bool skip = false;
  if (dml_probe(*rt, key_columns, pkey_columns, 0, probe, box, &skip, err,
                err_len))
    return true;
  if (skip) return false;

  // The server calls purge for two things: to roll back trx_ref's insert,
  // while trx_ref is still active, and to remove an entry that the committed
  // trx_ref delete-marked. Without a mapping B-tree both identify the entry
  // only by key and primary key, and a row deleted and re-inserted with the
  // same value by one transaction has one entry of each kind, so the
  // transaction state decides which one is meant. Asked before taking the
  // mutex: the check must not run under page latches.
  const bool rolling_back = index.is_transaction_active(trx_ref);

  std::unique_lock<std::shared_mutex> lock(rt->mutex);
  Page::Ref page = Page::INVALID_REF;
  uint32_t slot = 0;
  bool found = false;
  bool failed;
  if (rolling_back) {
    failed = find_entry(
        *rt, probe, box,
        [trx_ref](TrxRef ins, TrxRef del) { return ins == trx_ref && del == 0; },
        &page, &slot, &found, err, err_len);
  } else {
    failed = find_entry(
        *rt, probe, box,
        [trx_ref](TrxRef, TrxRef del) { return del == trx_ref; }, &page, &slot,
        &found, err, err_len);
  }
  if (failed) return true;
  // Already removed: purge can run again for the same record after a restart.
  if (!found) return false;
  return remove_entry(*rt, page, slot, probe, err, err_len);
}

bool begin(StorageCtx *ctx, const Index &index, MtrCtx::Ref /*mctx*/,
           const IndexScanDesc &scan_desc, Index::Cursor *cursor, bool *eof,
           char *err, uint32_t err_len) {
  *cursor = nullptr;
  *eof = true;
  if (scan_desc.reverse())
    return fail(err, err_len, "descending scans are not supported");
  if (scan_desc.num_keys() != 1)
    return fail(err, err_len, "expected one scan key, got %u",
                scan_desc.num_keys());
  const IndexScanKey key = scan_desc[0];
  const uint32_t fn = key.profile_function_id();
  if (fn != kFnL2 && fn != kFnL1 && fn != kFnLinf)
    return fail(err, err_len, "unknown profile function %u", fn);
  if (key.num_columns() != 1 || !key.is_bounded() || key[0].data == nullptr)
    return fail(err, err_len, "expected one cube in the scan key");

  std::unique_ptr<Cursor> c(new (std::nothrow) Cursor(ctx->user(), index));
  if (c == nullptr) return fail(err, err_len, "out of memory");
  const int query_dims = cube_slots(key[0].length);
  if (query_dims < 0 ||
      !decode_cube(key[0].data, key[0].length,
                   static_cast<uint32_t>(query_dims), c->query))
    return fail(err, err_len, "invalid cube in the scan key");
  c->fn = fn;
  c->read_view = scan_desc.read_view();
  c->locking = scan_desc.needs_trx_ref();

  // An exact limit is all the server reads. Otherwise rows past the limit may
  // be filtered out, so start larger; position() fetches more when needed.
  const uint32_t limit = scan_desc.limit();
  if (limit == 0)
    c->batch = kDefaultBatch;
  else if (scan_desc.is_limit_exact())
    c->batch = std::min(limit, kMaxBatch);
  else
    c->batch = std::min(std::max(limit, kDefaultBatch), kMaxBatch);

  if (refill(*c, err, err_len)) return true;
  *eof = c->hits.empty();
  *cursor = c.release();
  return false;
}

bool position(Index::Cursor cursor, Index::CursorOp op, bool *eof, char *err,
              uint32_t err_len) {
  auto *c = static_cast<Cursor *>(cursor);
  if (op != Index::CursorOp::Next)
    return fail(err, err_len, "only forward scans are supported");
  if (c->pos < c->hits.size()) c->pos++;
  if (c->pos >= c->hits.size() && !c->exhausted) {
    c->batch = std::min(c->batch * 2, kMaxBatch);
    if (refill(*c, err, err_len)) return true;
  }
  *eof = c->pos >= c->hits.size();
  return false;
}

bool fetch(Index::Cursor cursor, IndexScanKey::KeyPartRef * /*key_ref*/,
           IndexScanKey::KeyPartRef * /*key_column_refs*/,
           IndexScanKey::KeyPartData *key_columns,
           IndexScanKey::KeyPartData *pkey_columns, Segment::TrxRef *trx_ref,
           char *err, uint32_t err_len) {
  const auto *c = static_cast<const Cursor *>(cursor);
  if (c->pos >= c->hits.size())
    return fail(err, err_len, "fetch past the end of the scan");
  const Geometry &geo = c->rt->geo;
  const unsigned char *e = c->hits[c->pos].entry.data();

  // The pointers stay valid until the cursor moves: they point into the
  // cursor's own copy of the entry.
  key_columns[0] = IndexScanKey::KeyPartData{e + kLeafKey,
                                             get_u16(e + kLeafKeyLen)};
  const unsigned char *p = e + kLeafKey + geo.key_len;
  for (size_t i = 0; i < geo.pk_max.size(); i++) {
    pkey_columns[i] = IndexScanKey::KeyPartData{p + 2, get_u16(p)};
    p += 2 + size_t{geo.pk_max[i]};
  }
  if (trx_ref != nullptr) *trx_ref = c->locking ? leaf_ins_trx(e) : 0;
  return false;
}

bool validate(Index::Cursor cursor, Segment::TrxRef trx_ref, bool *valid,
              char *err, uint32_t err_len) {
  auto *c = static_cast<Cursor *>(cursor);
  *valid = false;
  if (c->pos >= c->hits.size())
    return fail(err, err_len, "validate past the end of the scan");
  const Bytes &probe = c->hits[c->pos].entry;
  Box box;
  if (!leaf_entry_box(c->rt->geo, probe.data(), box))
    return fail(err, err_len, "invalid cube value");

  {
    std::shared_lock<std::shared_mutex> lock(c->rt->mutex);
    Page::Ref page = Page::INVALID_REF;
    uint32_t slot = 0;
    if (find_entry(
            *c->rt, probe, box,
            [trx_ref](TrxRef ins, TrxRef del) {
              return ins == trx_ref && del == 0;
            },
            &page, &slot, valid, err, err_len)) {
      return true;
    }
  }
  if (!*valid) return false;

  // The row is current; return it only from the first hit that validated
  // it. The same hit may be validated again (after a lock wait).
  const uint64_t hit = c->batch_start + c->pos;
  const size_t pk = kLeafKey + size_t{c->rt->geo.key_len};
  const auto [it, inserted] =
      c->validated.emplace(Bytes(probe.begin() + pk, probe.end()), hit);
  if (!inserted && it->second != hit) *valid = false;
  return false;
}

bool save(Index::Cursor /*cursor*/, char * /*err*/, uint32_t /*err_len*/) {
  // The cursor holds no page latch between calls: nothing to detach.
  return false;
}

bool restore(Index::Cursor cursor, MtrCtx::Ref /*mctx*/, bool *eof,
             char * /*err*/, uint32_t /*err_len*/) {
  const auto *c = static_cast<const Cursor *>(cursor);
  *eof = c->pos >= c->hits.size();
  return false;
}

void end(Index::Cursor *cursor) {
  delete static_cast<Cursor *>(*cursor);
  *cursor = nullptr;
}

}  // namespace cube_rtree
