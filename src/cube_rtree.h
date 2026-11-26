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

// R-tree index for cube columns.
//
// The index serves exact nearest-neighbour scans:
//   ORDER BY cube_distance(c, q) LIMIT k            (profile cube_rtree_l2)
//   ORDER BY cube_taxicab_distance(c, q) LIMIT k    (profile cube_rtree_l1)
//   ORDER BY cube_chebyshev_distance(c, q) LIMIT k  (profile cube_rtree_linf)
//
// Storage: one segment. Its root page holds the index metadata (tree root page
// and height); every other page is a tree node. A leaf entry holds the cube
// value, the row's primary key and the inserting and deleting transaction ids.
// An internal entry holds a child page and the bounding box of that subtree.
// The index declares HAS_ROW_REF only, so the server keeps no mapping B-tree
// and the leaf entries are the only record of the indexed rows.
//
// Concurrency: one reader/writer mutex per index. DML takes it exclusive and
// scans take it shared. Every page access runs in a mini-transaction of its own
// that commits before the call returns, so the index holds no page latch
// between calls and never latches more than one page while reading.

#ifndef VSQL_CUBE_SRC_CUBE_RTREE_H
#define VSQL_CUBE_SRC_CUBE_RTREE_H

#include <villagesql/preview/index_builder.h>
#include <villagesql/preview/storage_api.h>

#include <cstdint>
#include <shared_mutex>
#include <vector>

namespace cube_rtree {

using vsql::preview_index_builder::Index;
using vsql::preview_index_builder::IndexScanDesc;
using vsql::preview_index_builder::IndexScanKey;
using vsql::preview_storage::MtrCtx;
using vsql::preview_storage::Page;
using vsql::preview_storage::Segment;
using vsql::preview_storage::Space;

// Profile function ids. Each profile binds one distance function, under its
// own id, so a scan knows its metric from the scan key's profile function id.
inline constexpr uint32_t kFnL2 = 1;
inline constexpr uint32_t kFnL1 = 2;
inline constexpr uint32_t kFnLinf = 3;

// Entry layout of one index, derived from the indexed column, the primary key
// and the page size.
struct Geometry {
  // Declared dimension count n of the cube(n) column.
  uint32_t dims = 0;
  // Persisted length of the cube(n) column: 8 + 2 * n * 8.
  uint32_t key_len = 0;
  // Maximum length of each primary key part.
  std::vector<uint32_t> pk_max;
  // Bytes per leaf entry and per internal entry.
  uint32_t leaf_slot = 0;
  uint32_t node_slot = 0;
  // Entries per leaf page and per internal page.
  uint32_t leaf_cap = 0;
  uint32_t node_cap = 0;
};

// Per-index state, allocated by the SDK in the index arena.
struct RtreeIndex {
  std::shared_mutex mutex;
  Space::Ref space = 0;
  // Segment root page, which holds the index metadata.
  Page::Ref meta_page = Page::INVALID_REF;
  // Tree root and the number of tree levels (1 when the root is a leaf).
  // Written under an exclusive lock on mutex, together with the metadata.
  Page::Ref root_page = Page::INVALID_REF;
  uint8_t height = 0;
  Geometry geo;
};

using StorageCtx = Index::StorageCtx<RtreeIndex>;

bool create(StorageCtx *ctx, const Index &index, Space::Ref space_ref,
            Segment::TrxRef trx_ref, char *err, uint32_t err_len);

bool drop(StorageCtx *ctx, const Index &index, Segment::TrxRef trx_ref,
          char *err, uint32_t err_len);

bool load(StorageCtx *ctx, const Index &index, Index::StorageRef storage_ref,
          char *err, uint32_t err_len);

bool insert(StorageCtx *ctx, const Index &index, Segment::TrxRef trx_ref,
            IndexScanKey::KeyPartData *key_columns,
            IndexScanKey::KeyPartData *pkey_columns,
            IndexScanKey::KeyPartRef *key_ref, char *err, uint32_t err_len);

bool mark_delete(StorageCtx *ctx, const Index &index, Segment::TrxRef trx_ref,
                 IndexScanKey::KeyPartRef *key_ref,
                 IndexScanKey::KeyPartData *key_columns,
                 IndexScanKey::KeyPartData *pkey_columns, bool delete_mark,
                 char *err, uint32_t err_len);

bool purge(StorageCtx *ctx, const Index &index, Segment::TrxRef trx_ref,
           IndexScanKey::KeyPartRef *key_ref,
           IndexScanKey::KeyPartData *key_columns,
           IndexScanKey::KeyPartData *pkey_columns, char *err,
           uint32_t err_len);

bool begin(StorageCtx *ctx, const Index &index, MtrCtx::Ref mctx,
           const IndexScanDesc &scan_desc, Index::Cursor *cursor, bool *eof,
           char *err, uint32_t err_len);

bool position(Index::Cursor cursor, Index::CursorOp op, bool *eof, char *err,
              uint32_t err_len);

bool fetch(Index::Cursor cursor, IndexScanKey::KeyPartRef *key_ref,
           IndexScanKey::KeyPartRef *key_column_refs,
           IndexScanKey::KeyPartData *key_columns,
           IndexScanKey::KeyPartData *pkey_columns, Segment::TrxRef *trx_ref,
           char *err, uint32_t err_len);

bool validate(Index::Cursor cursor, Segment::TrxRef trx_ref, bool *valid,
              char *err, uint32_t err_len);

bool save(Index::Cursor cursor, char *err, uint32_t err_len);

bool restore(Index::Cursor cursor, MtrCtx::Ref mctx, bool *eof, char *err,
             uint32_t err_len);

void end(Index::Cursor *cursor);

}  // namespace cube_rtree

#endif  // VSQL_CUBE_SRC_CUBE_RTREE_H
