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

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Extension name
set(EXTENSION_NAME "vsql_cube")

# Find VillageSQL SDK (for VEB creation macro and build infrastructure)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(VillageSQL REQUIRED)

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "VillageSQL include dir: ${VillageSQL_INCLUDE_DIR}")

# Protocol 2 SDK: use internal server SDK headers when VillageSQL_SOURCE_DIR is set.
# This is required when building against a server that uses VEF Protocol 2
# (vef_vdf_args_t layout differs from the staged public SDK).
if(VillageSQL_SOURCE_DIR)
  set(_p2_src_include "${VillageSQL_SOURCE_DIR}/villagesql/sdk/include")
  set(_p2_build_include "${VillageSQL_BUILD_DIR}/sdk/include")
  if(EXISTS "${_p2_src_include}/villagesql/func_builder.h")
    set(VSQL_PROTOCOL2_INCLUDES "${_p2_src_include}" "${_p2_build_include}")
    message(STATUS "Using Protocol 2 SDK from: ${_p2_src_include}")
  else()
    message(WARNING "VillageSQL_SOURCE_DIR set but Protocol 2 SDK not found at ${_p2_src_include}")
  endif()
endif()

# Create the vsql_cube shared library
add_library(vsql_cube SHARED
    src/vsql_cube.cc
)

# Include directories: Protocol 2 headers first (if set), then staged SDK headers
target_include_directories(vsql_cube PRIVATE
    ${VSQL_PROTOCOL2_INCLUDES}
    ${VillageSQLExtensionFramework_INCLUDE_DIR}
)

# Create the VEB package
VEF_CREATE_VEB(
    NAME ${EXTENSION_NAME}
    LIBRARY_TARGET vsql_cube
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
