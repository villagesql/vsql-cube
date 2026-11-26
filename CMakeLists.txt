# Copyright (c) 2025 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(vsql_cube)

# Default to an optimized build. An empty CMAKE_BUILD_TYPE yields -O0 (no
# -O flag applied), which builds and runs but leaves the code unoptimized.
# Override with -DCMAKE_BUILD_TYPE=.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Extension name
set(EXTENSION_NAME "vsql_cube")

# The R-tree index uses the preview index and storage capabilities, so an
# extension built with it installs only when vsql_allow_preview_extensions is ON.
# Those capabilities change in place, and only the dev ABI headers carry their
# current form, so the R-tree build compiles against the dev ABI.
option(VSQL_CUBE_RTREE "Build the cube R-tree index (preview)" OFF)
if(VSQL_CUBE_RTREE)
  set(VSQL_USE_DEV_ABI ON)
endif()

# Find VillageSQL SDK (for VEB creation macro and build infrastructure)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(VillageSQL REQUIRED)

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "VillageSQL include dir: ${VillageSQL_INCLUDE_DIR}")

set(VSQL_CUBE_SOURCES src/vsql_cube.cc)
if(VSQL_CUBE_RTREE)
  list(APPEND VSQL_CUBE_SOURCES src/cube_rtree.cc)
endif()

# Create the vsql_cube shared library
add_library(vsql_cube SHARED ${VSQL_CUBE_SOURCES})

target_include_directories(vsql_cube PRIVATE
    ${VillageSQLExtensionFramework_INCLUDE_DIR}
)

if(VSQL_CUBE_RTREE)
  target_compile_definitions(vsql_cube PRIVATE VSQL_CUBE_RTREE)
endif()

# Create the VEB package
VEF_CREATE_VEB(
    NAME ${EXTENSION_NAME}
    LIBRARY_TARGET vsql_cube
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
