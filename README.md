# VillageSQL Cube Extension

A port of PostgreSQL's `cube` extension for VillageSQL. Adds a `cube` custom type representing n-dimensional boxes and points, along with the full set of constructor, accessor, predicate, distance, and geometry functions.

## What This Is

The cube extension lets you store and query multi-dimensional geometric boxes and points in SQL. A cube value can represent a single point in n-dimensional space (`(1, 2, 3)`) or a box defined by two corner coordinates (`(1, 2, 3),(4, 5, 6)`). The extension supports all string input formats from the PostgreSQL cube extension and provides named functions covering the full operator and function API.

## Project Structure

```
vsql_cube/
├── manifest.json
├── CMakeLists.txt
├── build.sh
├── cmake/
│   └── FindVillageSQL.cmake
├── src/
│   └── vsql_cube.cc
└── test/
    ├── t/
    │   └── vsql_cube.test
    └── r/
        └── vsql_cube.result
```

## Prerequisites

- VillageSQL build directory (with completed build)
- VillageSQL server source directory (required for Protocol 2 SDK headers)
- CMake 3.16 or higher
- C++ compiler with C++17 support

📚 **Full Documentation**: Visit [villagesql.com/docs](https://villagesql.com/docs) for guides on building and installing extensions.

## Building the Extension

**Linux:**
```bash
mkdir build
cd build
cmake .. \
  -DVillageSQL_BUILD_DIR=$HOME/build/villagesql \
  -DVillageSQL_SOURCE_DIR=$HOME/code/villagesql-server
make -j $(($(getconf _NPROCESSORS_ONLN) - 2))
```

**macOS:**
```bash
mkdir build
cd build
cmake .. \
  -DVillageSQL_BUILD_DIR=~/build/villagesql \
  -DVillageSQL_SOURCE_DIR=~/code/villagesql-server
make -j $(($(sysctl -n hw.logicalcpu) - 2))
```

`VillageSQL_BUILD_DIR` points to your VillageSQL build directory. `VillageSQL_SOURCE_DIR` points to the VillageSQL server source — required to pick up Protocol 2 SDK headers.

To install the VEB:
```bash
make install
# or manually:
cp build/vsql_cube.veb $VillageSQL_BUILD_DIR/lib/veb/
```

## Using the Extension

```sql
INSTALL EXTENSION vsql_cube;
```

### The `cube` type

Declare a column with an explicit dimension parameter. Bare `cube` (without a dimension) is not
supported — always use `` `cube`(N) ``. The backticks go around `cube` only, not the parentheses.

```sql
-- cube(n) stores 8 + 2*n*8 bytes per row; n must be 1..100
-- Note: `cube` is a MySQL reserved word; backtick-quote it in DDL
CREATE TABLE locations (id INT PRIMARY KEY, region `cube`(3));

-- Insert using string literals (VDF constructors cannot be inserted directly into cube columns)
INSERT INTO locations VALUES (1, '(0,0,0),(10,10,10)');  -- 3D box
INSERT INTO locations VALUES (2, '(5,5,5)');              -- 3D point

-- cube(32) is the standard default column width (520 bytes per row)
CREATE TABLE legacy (id INT PRIMARY KEY, region `cube`(32));

-- Round-trip through string representation
SELECT id, cube_to_string(region) FROM locations;
```

Choose `n` to match your actual data. A `cube(3)` column uses 56 bytes per row instead of 520 for `cube(32)` — a 9× reduction for purely 3D data. The maximum is 100 dimensions.

### Constructors

| Function | Signature | Returns |
|----------|-----------|---------|
| `cube_point` | `(x REAL)` | 1D point |
| `cube_box` | `(lo REAL, hi REAL)` | 1D box |
| `cube_point_nd` | `(coords_csv STRING)` | n-D point from CSV string |
| `cube_box_nd` | `(lo_csv STRING, hi_csv STRING)` | n-D box from two CSV strings |
| `cube_add_dim` | `(c cube, lo REAL, hi REAL)` | cube with one more dimension appended |

```sql
SELECT cube_point(5.0);                -- (5)
SELECT cube_box(1.0, 3.0);            -- (1),(3)
SELECT cube_point_nd('1,2,3');         -- (1, 2, 3)
SELECT cube_box_nd('1,2,3', '4,5,6'); -- (1, 2, 3),(4, 5, 6)

-- cube_add_dim extends a stored cube with a new dimension
CREATE TABLE t (c `cube`(32) NOT NULL);
INSERT INTO t VALUES ('(1,2)');
SELECT cube_add_dim(c, 3.0, 4.0) FROM t;  -- (1, 2, 3),(1, 2, 4)
DROP TABLE t;
```

### Accessors

| Function | Signature | Returns |
|----------|-----------|---------|
| `cube_dim` | `(c cube)` | Number of dimensions (INT) |
| `cube_ll_coord` | `(c cube, n INT)` | Lower-left coordinate n, 1-indexed; NULL if out of range |
| `cube_ur_coord` | `(c cube, n INT)` | Upper-right coordinate n, 1-indexed; NULL if out of range |
| `cube_is_point` | `(c cube)` | 1 if point, 0 if box |
| `cube_coord` | `(c cube, n INT)` | Position n using ll-then-ur indexing; NULL if out of range |

```sql
CREATE TABLE t (c `cube`(3) NOT NULL);
INSERT INTO t VALUES ('(1,2,3),(3,4,5)');
SELECT cube_dim(c) FROM t;                -- 3
SELECT cube_ll_coord(c, 2) FROM t;        -- 2
SELECT cube_ur_coord(c, 2) FROM t;        -- 4
SELECT cube_is_point(c) FROM t;           -- 0 (it's a box)
DROP TABLE t;
```

All non-aggregate functions are deterministic and can be used in CHECK constraints:

```sql
-- Enforce that all stored cubes are 3-dimensional
CREATE TABLE vectors (v `cube`(3) NOT NULL CHECK (cube_dim(v) = 3));

-- Only allow points, not boxes
CREATE TABLE points (p `cube`(32) NOT NULL CHECK (cube_is_point(p) = 1));
```

### Predicates

These replace PostgreSQL's infix operators (`&&`, `@>`, `<@`).

| Function | Returns 1 when... |
|----------|-------------------|
| `cube_overlaps(a, b)` | a and b share any space (touching edges count) |
| `cube_contains(a, b)` | a fully contains b |
| `cube_contained_by(a, b)` | a is fully inside b |

```sql
CREATE TABLE t (a `cube`(1) NOT NULL, b `cube`(1) NOT NULL);
INSERT INTO t VALUES ('(0),(3)', '(2),(5)');
SELECT cube_overlaps(a, b) FROM t;   -- 1 (boxes share the range [2,3])
SELECT cube_contains(a, b) FROM t;   -- 0 (a=(0,3) does not contain b=(2,5))
DROP TABLE t;
```

### Distance Functions

These replace PostgreSQL's distance operators (`<->`, `<#>`, `<=>`). For box inputs, distance is 0 if they overlap — measured between nearest faces otherwise.

| Function | Distance metric |
|----------|----------------|
| `cube_distance(a, b)` | Euclidean (L2) |
| `cube_taxicab_distance(a, b)` | Manhattan (L1) |
| `cube_chebyshev_distance(a, b)` | Chebyshev (L-infinity) |

```sql
CREATE TABLE t (a `cube`(2) NOT NULL, b `cube`(2) NOT NULL);
INSERT INTO t VALUES ('(0,0)', '(3,4)');
SELECT cube_distance(a, b) FROM t;           -- 5
SELECT cube_taxicab_distance(a, b) FROM t;   -- 7
SELECT cube_chebyshev_distance(a, b) FROM t; -- 4
DROP TABLE t;
```

### String Conversion

| Function | Signature | Description |
|----------|-----------|-------------|
| `cube_from_string` | `(s STRING)` | Parse a string literal into a cube value |
| `cube_to_string` | `(c cube)` | Render a cube value as a string |

```sql
SELECT cube_from_string('(1, 2),(3, 4)');  -- (1, 2),(3, 4)
CREATE TABLE t (c `cube`(2) NOT NULL);
INSERT INTO t VALUES ('(1,2),(3,4)');
SELECT cube_to_string(c) FROM t;           -- (1, 2),(3, 4)
DROP TABLE t;
```

### Geometry Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `cube_union` | `(a, b cube)` | Smallest cube containing both |
| `cube_inter` | `(a, b cube)` | Intersection of two cubes |
| `cube_enlarge` | `(c cube, radius REAL, n_dims INT)` | Expand by radius along first n_dims dimensions |
| `cube_subset` | `(c cube, dims_csv STRING)` | Extract and optionally reorder dimensions |

```sql
CREATE TABLE t (a `cube`(3) NOT NULL, b `cube`(3) NOT NULL);
INSERT INTO t VALUES ('(1,1,1),(3,3,3)', '(2,2,2),(5,5,5)');
SELECT cube_union(a, b) FROM t;          -- (1, 1, 1),(5, 5, 5)
SELECT cube_inter(a, b) FROM t;          -- (2, 2, 2),(3, 3, 3)
SELECT cube_enlarge(a, 1.0, 2) FROM t;  -- (0, 0, 1),(4, 4, 3)
SELECT cube_subset(a, '3,1') FROM t;    -- (1, 1),(3, 3)
DROP TABLE t;
```

### Aggregate Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `cube_agg` | `(c cube)` | Bounding box of all cube values in the group |
| `cube_scalar_agg` | `(x REAL)` | Bounding box (1D interval) across all scalar values in the group |

```sql
-- Bounding box of a set of cubes
CREATE TABLE regions (r `cube`(32) NOT NULL);
INSERT INTO regions VALUES ('(1, 1),(3, 3)');
INSERT INTO regions VALUES ('(2, 2),(5, 4)');
SELECT cube_to_string(cube_agg(r)) FROM regions;  -- (1, 1),(5, 4)
DROP TABLE regions;

-- 1D interval spanning a set of scalars
SELECT cube_to_string(cube_scalar_agg(x)) FROM (
  SELECT 1.0 AS x UNION ALL SELECT 5.0 UNION ALL SELECT 3.0
) t;  -- (1),(5)
```

Both return NULL for empty groups.

### Input Format

All PostgreSQL cube string formats are accepted:

| Format | Example | Result |
|--------|---------|--------|
| Bare scalar | `'5'` | `(5)` — 1D point |
| Parenthesized | `'(5)'` | `(5)` — 1D point |
| Comma-separated | `'1,2,3'` | `(1, 2, 3)` — 3D point |
| Single corner | `'(1,2,3)'` | `(1, 2, 3)` — 3D point |
| Two corners | `'(1,2),(3,4)'` | `(1, 2),(3, 4)` — 2D box |
| Bracket notation | `'[(1,2),(3,4)]'` | `(1, 2),(3, 4)` — 2D box |
Corners are normalized so lower-left ≤ upper-right on each dimension. `'(3,4),(1,2)'` stores as `(1, 2),(3, 4)`.

NaN and Inf are rejected in all string-parsing paths — `cube_from_string`, `cube_point_nd`, and `cube_box_nd` return error 3200 if any coordinate parses to a non-finite value.

## Testing

See [TESTING.md](TESTING.md) for build, install, and test-run instructions.

## Known Limitations

These limitations reflect current VEF capabilities or explicit design choices in this extension. Each entry describes the constraint, what it means in practice, any workaround the code uses, and the VEF API that would resolve it.

### L1: Per-Row Variable-Length Storage

**Impact:** A `cube(n)` column stores exactly `8 + 2*n*8` bytes per row regardless of the cube's actual dimensionality. A 3-dim cube stored in a `cube(100)` column uses the same 1608 bytes as a 100-dim cube. Choose `n` to match your actual data to avoid waste.

VEF supports parameter-driven storage sizes via `resolve_params_fn()`: `cube(5)` columns use 88 bytes per row, `cube(32)` uses 520, `cube(100)` uses 1608. The maximum is 100 dimensions (matching PostgreSQL's cube extension). An explicit N is always required — bare `cube` without N is rejected by the server.

### L2: No Infix Operator Syntax

**Impact:** Queries use named functions instead of PostgreSQL's infix operators. The functionality is identical — only the syntax differs. Code ported from PostgreSQL needs mechanical substitution.

VEF does not provide a custom operator registration API. PostgreSQL's cube operators map to named functions:

| PostgreSQL operator | This extension |
|--------------------|----------------|
| `a && b` | `cube_overlaps(a, b)` |
| `a @> b` | `cube_contains(a, b)` |
| `a <@ b` | `cube_contained_by(a, b)` |
| `a <-> b` | `cube_distance(a, b)` |
| `a <#> b` | `cube_taxicab_distance(a, b)` |
| `a <=> b` | `cube_chebyshev_distance(a, b)` |

### L3: No Array Input Type Support

**Impact:** Constructors accept CSV strings instead of arrays. The workaround is straightforward — `'1.0,2.0,3.0'` instead of `ARRAY[1.0,2.0,3.0]`. Code ported from PostgreSQL needs string-formatting substitution at the call site.

VEF VDFs cannot accept array-typed parameters:

| PostgreSQL | vsql-cube |
|------------|----------------|
| `cube(ARRAY[1.0,2.0,3.0])` | `cube_point_nd('1.0,2.0,3.0')` |
| `cube(ARRAY[1.0], ARRAY[3.0])` | `cube_box_nd('1.0', '3.0')` |
| `cube_subset(c, ARRAY[2,1])` | `cube_subset(c, '2,1')` |

### L4: No GiST Indexing

**Impact:** Cube columns can't be indexed. Range queries and nearest-neighbor lookups (`ORDER BY cube_distance(...) LIMIT k`) require a full scan. For small tables or infrequent queries this is fine. For large tables with frequent spatial queries, this is a constraint.

## Troubleshooting

**Extension not found after installation:**
- Verify the VEB file is in the server's `veb_dir` (`SHOW VARIABLES LIKE 'veb_dir';`)
- Extension names use underscores: `INSTALL EXTENSION vsql_cube` (not `vsql-cube`)

**`cube` column DDL fails:**
- `cube` is a MySQL reserved word. Always backtick-quote it and include an explicit dimension: `` `cube`(32) ``
- Bare `` `cube` `` without a dimension parameter is not supported — the server requires an explicit N

**"cannot determine type parameters for vsql_cube.cube" error:**
- The server infers the cube's dimension parameter (`N` in `cube(N)`) from column metadata. When a cube value comes from a VDF constructor rather than a column, no metadata is available. Store the cube in a column and operate on it from there — this is the normal usage pattern. Example: `INSERT INTO t VALUES ('(1,2,3)'); SELECT cube_dim(c) FROM t;`

**Building against a newer server fails:**
- Ensure `VillageSQL_SOURCE_DIR` is set so cmake picks up Protocol 2 SDK headers
- Without it, cmake falls back to the staged SDK and VDFs returning custom types will return NULL

## Reporting Bugs and Requesting Features

File issues at [github.com/villagesql/villagesql-server/issues](https://github.com/villagesql/villagesql-server/issues). Include:
- A description of the problem or feature request
- Reproduction steps (SQL and version)
- Expected vs. actual output
- Your VillageSQL version (`SELECT VERSION();`)

## Contact

- **Discord**: [discord.gg/KSr6whd3Fr](https://discord.gg/KSr6whd3Fr)
- **GitHub Issues**: [github.com/villagesql/villagesql-server/issues](https://github.com/villagesql/villagesql-server/issues)
- **GitHub Discussions**: [github.com/villagesql/villagesql-server/discussions](https://github.com/villagesql/villagesql-server/discussions)

## License

GPL-2.0. See the license header in source files for details.
