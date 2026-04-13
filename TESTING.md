# Testing vsql_cube

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `VillageSQL_BUILD_DIR` | Yes | Path to your VillageSQL build directory |
| `VillageSQL_SOURCE_DIR` | Yes | Path to the VillageSQL server source directory (needed for Protocol 2 SDK headers) |

**How to find these values:**
- `VillageSQL_BUILD_DIR`: the directory where you ran `cmake` and `make` to build VillageSQL. It contains `runtime_output_directory/mysqld`.
- `VillageSQL_SOURCE_DIR`: the directory where you cloned `villagesql-server`.

## Build and Install

```bash
cd vsql_cube/

mkdir build
cd build

# Linux
cmake .. \
  -DVillageSQL_BUILD_DIR=$HOME/build/villagesql \
  -DVillageSQL_SOURCE_DIR=$HOME/code/villagesql-server
make -j $(($(getconf _NPROCESSORS_ONLN) - 2))

# macOS
cmake .. \
  -DVillageSQL_BUILD_DIR=~/build/villagesql \
  -DVillageSQL_SOURCE_DIR=~/code/villagesql-server
make -j $(($(sysctl -n hw.logicalcpu) - 2))

# Install the VEB
make install
# or manually:
cp vsql_cube.veb $VillageSQL_BUILD_DIR/lib/veb/
```

## Running Tests

Tests use the MySQL Test Runner (MTR). Run from the server's `mysql-test/` directory.

The extension must be installed (VEB in `veb_dir`) before running tests. MTR installs and uninstalls the extension automatically within each test file.

**Linux:**
```bash
cd $HOME/build/villagesql/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql_cube/mysql-test
```

**macOS:**
```bash
cd ~/build/villagesql/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql_cube/mysql-test
```

**With parallel execution:**
```bash
perl mysql-test-run.pl --suite=/path/to/vsql_cube/mysql-test --parallel=auto
```

## Regenerating Result Files

If you modify test cases or the extension behavior changes, regenerate expected results:

```bash
cd $VillageSQL_BUILD_DIR/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql_cube/mysql-test --record
```

This overwrites `mysql-test/r/*.result`. Review the diff before committing.

## Test Files

| Test file | What it covers |
|-----------|---------------|
| `t/vsql_cube.test` | Constructors, string I/O, accessors (including out-of-range NULL returns), predicates, distance functions (point-to-point and box-to-box), geometry functions, aggregates (`cube_agg`, `cube_scalar_agg`), NULL handling |

## Verifying the Server is Running

```bash
$VillageSQL_BUILD_DIR/runtime_output_directory/mysql -u root -e "SELECT VERSION();"
```

If the server is not running, start it:
```bash
$VillageSQL_BUILD_DIR/runtime_output_directory/mysqld \
  --datadir=/path/to/data \
  --socket=/tmp/mysql.sock \
  --port=3306 \
  --daemonize
```

## Quick Smoke Test

After building and installing, verify the extension works:

```sql
INSTALL EXTENSION vsql_cube;
SELECT cube_to_string(cube_point(5.0));                              -- (5)
SELECT cube_to_string(cube_box_nd('1,2,3', '4,5,6'));               -- (1, 2, 3),(4, 5, 6)
SELECT cube_distance(cube_point_nd('0,0'), cube_point_nd('3,4'));   -- 5
UNINSTALL EXTENSION vsql_cube;
```
