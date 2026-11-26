# AGENTS.md

This file provides guidance to AI coding assistants when working with the vsql_cube extension.

**Note**: Also check `AGENTS.local.md` for machine-specific path overrides when present.

## Project Overview

vsql_cube is a VillageSQL extension implementing the `cube` custom type — an n-dimensional box/point
representation compatible with PostgreSQL's cube extension. It uses the Protocol V5 stable SDK and the
typed builder API. The type and its functions live in `src/vsql_cube.cc`; the optional R-tree index
lives in `src/cube_rtree.{h,cc}` (see "R-tree index" below).

## Key Facts

- **Extension name**: `vsql_cube`
- **Custom type name**: `cube` (MySQL reserved word — backtick-quote in DDL: `` `cube` ``)
- **Source**: `src/vsql_cube.cc`
- **Storage**: `8 + 2*n*8` bytes per value where n is the column's declared dimension parameter
  - `cube(n)` → `8 + 2*n*8` bytes (e.g., `cube(3)` = 56 bytes, `cube(32)` = 520 bytes)
  - A column must declare a length. `` `cube` `` on its own is refused with
    `Type 'vsql_cube.cube' requires a length specification`
  - Maximum: `cube(100)` = 1608 bytes (kMaxStorageSize)
- **Max dimensions**: 100 absolute maximum (kAbsoluteMaxDims); default column width is 32 (kDefaultMaxDims)
- **Protocol**: V5 stable (uses staged SDK via `VillageSQL_BUILD_DIR`; `VillageSQL_SOURCE_DIR` not required)

## R-tree index

- Built only with `-DVSQL_CUBE_RTREE=ON`, which compiles `src/cube_rtree.cc` and defines
  `VSQL_CUBE_RTREE`. It uses the preview index and storage capabilities, so that build installs
  only with `vsql_allow_preview_extensions = ON`; the default build registers no preview
  capability. The option also sets `VSQL_USE_DEV_ABI`, since only the SDK's `include-dev/` headers
  carry the current preview index ABI (the stable `include/` copy lacks `scan_validate` and the
  read view).
- Index type `rtree`, profiles `cube_rtree_l2` (default), `cube_rtree_l1`, `cube_rtree_linf`, one
  per distance function. The server passes the profile's ORDER function id to the scan, so one
  profile cannot serve several metrics.
- `HAS_ROW_REF` only: no mapping B-tree, leaf entries hold the cube, the primary key and the
  inserting and deleting trx ids. purge() serves both rollback of an insert (trx still active) and
  purge of a delete mark (trx committed); the trx state picks the entry.
- One `std::shared_mutex` per index (DML exclusive, scans shared). Every page access runs in its own
  mini-transaction; scans ignore the server's mctx and hold no latch between calls.
- KNN scans are exact best-first searches in batches; each refill restarts at the root and resumes
  after the last result returned.
- Test: `mysql-test/t/cube_rtree.test`. Both tests tell the builds apart by installing with preview
  extensions off: only the R-tree build fails (error 3219). `cube_rtree` skips on the default
  build; `vsql_cube` turns preview on for the R-tree build and gives the same result for both.
- The server cannot yet build a custom index over existing rows (CREATE INDEX on a populated table,
  ALTER ADD INDEX, table rebuild): with no mapping B-tree the DDL loader asserts on the missing root
  page. `cube_rtree` creates every index on an empty table; `cube_rtree_build` pins the build cases
  and is marked `not_implemented.inc` (empty `.result`).
- Reopening a table with the index from the data dictionary (always after a restart) asserts in a
  debug server without a fix to `Custom_index::load_storage` (it checked the root page while it was
  still the placeholder 0 from `dd_fill_one_dict_index`). `cube_rtree_restart` needs that fix.

## Build

```bash
mkdir build && cd build
cmake .. \
  -DVillageSQL_BUILD_DIR=/path/to/villagesql/build
make -j$(getconf _NPROCESSORS_ONLN)
make install
```

## Architecture

### CubeData struct

In-memory representation (always fully allocated for up to 100 dims):

```cpp
struct CubeData {
  uint16_t ndim;     // actual dimensions stored (0–n)
  uint8_t  flags;    // kFlagIsPoint = 0x01
  uint8_t  _pad[5];  // reserved
  double   ll[100];  // lower-left coordinates (only ndim used)
  double   ur[100];  // upper-right coordinates (only ndim used)
};
```

On disk a `cube(n)` column occupies `8 + 2*n*8` bytes per row whatever its actual ndim: ndim
doubles each for ll and ur, zero-padded out to the declared n slots. Serialization uses
little-endian byte order for ndim.

### Key constants

```cpp
constexpr int    kAbsoluteMaxDims   = 100;   // compile-time ceiling (matches PostgreSQL)
constexpr int    kDefaultMaxDims    = 32;    // params fallback, not a usable bare column
constexpr size_t kDefaultStorageSize = 520;  // 8 + 2*32*8, persisted_length fallback
constexpr size_t kMaxStorageSize     = 1608; // 8 + 2*100*8, buffer_size for VDFs
constexpr size_t kMaxDecodeLen       = 5200; // output buffer for cube_to_string
```

### Function inventory (24 total)

- **Constructors**: `cube_point`, `cube_box`, `cube_point_nd`, `cube_box_nd`, `cube_add_dim`
- **String I/O**: `cube_from_string`, `cube_to_string`
- **Accessors**: `cube_dim`, `cube_ll_coord`, `cube_ur_coord`, `cube_is_point`, `cube_coord`
- **Predicates**: `cube_overlaps`, `cube_contains`, `cube_contained_by`
- **Distance**: `cube_distance`, `cube_taxicab_distance`, `cube_chebyshev_distance`
- **Geometry**: `cube_union`, `cube_inter`, `cube_enlarge`, `cube_subset`
- **Aggregates**: `cube_agg`, `cube_scalar_agg`

## VEF API Patterns

This extension uses the Protocol V5 stable SDK throughout. Key patterns:

### Custom type registration

`cube_int_to_params` and `cube_resolve_params` are registered as separate VDFs (using
`make_int_to_params<>` and `make_resolve_params<>`) **before** the type, then referenced by
name in the type builder. This uses the string-based API in `type_builder.h`.

`persisted_length(-1)` is required to enable `cube(N)` parameterized DDL syntax. A non-negative
value causes the server to treat the type as fixed-length and reject `TYPE(N)`. With `-1` and
`int_to_params` registered, bare `cube` (without explicit N) is also rejected — always use
`` `cube`(N) `` in DDL (e.g., `` `cube`(32) ``).

```cpp
// Register parameter VDFs BEFORE the type
.func(make_int_to_params<&cube_int_to_params>("cube::int_to_params"))
.func(make_resolve_params<&cube_resolve_params>("cube::resolve_params"))

.type(make_type(CUBE)
  .persisted_length(-1)  // required: enables cube(N) syntax; bare cube rejected
  .max_decode_buffer_length(static_cast<int64_t>(kMaxDecodeLen))
  .params<CubeParams, &CubeParams::parse>()
  .int_to_params("cube::int_to_params")    // string name referencing the VDF above
  .resolve_params("cube::resolve_params")  // string name referencing the VDF above
  .encode(&cube_encode)
  .decode(&cube_decode)
  .compare(&cube_compare)
  .intrinsic_default_str("(0)")
  .build())
```

`intrinsic_default_str` is required. At type initialization the server calls `encode()` on it.
Without this, the server falls back to `encode("")`, which this extension rejects (empty strings
are invalid cube syntax).

`.params<>()` must appear before `.resolve_params<>()`. `cube_int_to_params` converts the bare
integer syntax `cube(32)` into the `{"n": "32"}` map that `cube_resolve_params` receives.

### buffer_size is required for all CUBE-returning VDFs

The default buffer is 256 bytes. Every VDF returning CUBE must set `.buffer_size(kMaxStorageSize)`
(1608 bytes — the maximum possible cube size). The framework uses this as the allocation ceiling;
actual bytes written is controlled by `result->actual_len` in `set_cube_result`. Forgetting this
causes NULL returns with no error.

```cpp
.func(make_func<&cube_point_impl>("cube_point")
  .returns(CUBE).param(REAL).buffer_size(kMaxStorageSize).deterministic().build())
```

The `from_string<>` and `to_string<>` builder shortcuts hardcode `buffer_size=0` (256-byte
default) and cannot be used for this type. Use explicit VDF implementations instead.

### Determinism

All non-aggregate functions are registered with `.deterministic()`. This allows them to appear
in CHECK constraints. Aggregate functions (`cube_agg`, `cube_scalar_agg`) are not marked
deterministic — CHECK constraints operate on individual rows, not groups, so this is not a
limitation in practice.

### NaN and Inf are rejected

`parse_double` and `parse_coord_csv` both call `std::isfinite()` after `strtod`, and
`gather_coords` checks it again for a coordinate that arrives as a number, so NaN and
Inf are rejected on every path into a cube. The type encode
path (`cube_encode → cube_parse → parse_double`) inherits this rejection. Direct REAL inputs
to `cube_point` and `cube_box` are passed through from the server and are not checked — users
who need to guard against NaN in those paths should validate at the application layer.

### cube_compare ordering for invalid buffers

If `cube_compare` receives a buffer with an invalid layout (not `8 + 2*n*8` bytes for any valid n),
it uses a defined total order: invalid buffers sort before valid ones. Both invalid → equal
(return 0); one invalid → the invalid one is smaller.

### Aggregate functions: make_aggregate_func with CustomResultWith

Use `make_aggregate_func<State, &result_fn>()` for all aggregate VDFs. The result function
takes `void(const State&, CustomResultWith<CubeParams>)`. Prerun/postrun are auto-generated;
no `.state<>()` call is needed.

```cpp
void cube_agg_result(const CubeAggState &state, CustomResultWith<CubeParams> out) {
  if (!state.has_value()) { out.set_null(); return; }
  set_cube_result_typed(*state, out);
}

.func(make_aggregate_func<CubeAggState, &cube_agg_result>("cube_agg")
  .returns(CUBE).param(CUBE)
  .clear<&cube_agg_clear>()
  .accumulate<&cube_agg_accumulate>()
  .buffer_size(kMaxStorageSize).build())
```

### Typed aggregate arg wrappers

`cube_agg` accumulates `villagesql::CustomArg` (CUBE inputs); `cube_scalar_agg` accumulates
`villagesql::RealArg` (REAL inputs). Both are in the `villagesql::` namespace directly — there
is no `villagesql::func_types::` sub-namespace.

```cpp
void cube_agg_accumulate(CubeAggState &state, villagesql::CustomArg arg) {
  if (arg.is_null()) return;
  auto span = arg.value();  // Span<const unsigned char>
  ...
}

void cube_scalar_agg_accumulate(CubeScalarAggState &state, villagesql::RealArg arg) {
  double v = arg.value();  // server skips NULL rows before calling accumulate for RealArg
  ...
}
```

### Result constants

Use `VEF_RESULT_VALUE`, `VEF_RESULT_NULL`, `VEF_RESULT_ERROR` — not `IS_VALUE`, `IS_NULL`,
`IS_ERROR` (those are from an older API version).

## Testing

Tests are in `mysql-test/t/vsql_cube.test`. See TESTING.md for run commands.

**Critical**: `cube` is a MySQL reserved word. DDL must backtick-quote the type name, and an
explicit dimension parameter is always required — bare `` `cube` `` without N is rejected:

```sql
CREATE TABLE t_cube (c `cube`(32) NOT NULL);
```

Unquoted `cube` in column type position fails with a parser error. This affects aggregate tests
that require persistent tables — derived-table workarounds don't work because custom type values
don't survive UNION ALL materialization.

The fixed-arity constructors (`cube_point`, `cube_box`, etc.) return the base `cube` type without
column parameters. The varargs constructors (`cube_point_nd`, `cube_box_nd`, `cube_add_dim`,
`cube_subset`) resolve `cube(n)` when every coordinate is an INT or REAL expression or a constant,
so `INSERT ... SELECT cube_point_nd(1.0, 2.0, 3.0)` into a `` `cube`(3) `` column works. Inserting a fixed-arity constructor's result into a `` `cube`(N) `` column fails
with a type-key mismatch ("Cannot implicitly cast from vsql_cube.cube to vsql_cube.cube(N)").
Use string literals for those INSERTs — they go through `cube_encode`, which has the column's storage context.

## Known Limitations

See README.md → Known Limitations for the full list. The main constraints:

- **L1**: Per-row variable-length storage not supported — `cube(n)` always uses `8+2n*8` bytes regardless of actual ndim. Choose n to match your data. The remaining limitation is per-row compaction (e.g., 3-dim cube in cube(10) column still uses 168 bytes, not 56).
- **L2**: No custom operator syntax (`&&`, `@>`, etc.) — named functions only
- **L3**: No array input type — the constructors take the coordinates as separate arguments instead. A single argument may still be a comma-separated string, which is the only way to build a cube from coordinates held in a column.
- **L4**: No GiST indexing
- **L5**: No `ALTER EXTENSION ... UPGRADE` — planned for a future VillageSQL release
