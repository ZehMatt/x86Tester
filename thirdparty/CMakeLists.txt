# This file is automatically generated from cmake.toml - DO NOT EDIT
# See https://github.com/build-cpp/cmkr for more information

# Create a configure-time dependency on cmake.toml to improve IDE support
if(CMKR_ROOT_PROJECT)
	configure_file(cmake.toml cmake.toml COPYONLY)
endif()

# Options
option(INSTALL_GTEST "" OFF)
option(BUILD_GMOCK "" OFF)

include(FetchContent)

# Fix warnings about DOWNLOAD_EXTRACT_TIMESTAMP
if(POLICY CMP0135)
	cmake_policy(SET CMP0135 NEW)
endif()
message(STATUS "Fetching sfl (1.8.1)...")
FetchContent_Declare(sfl SYSTEM
	GIT_REPOSITORY
		"https://github.com/slavenf/sfl-library"
	GIT_TAG
		1.8.1
)
FetchContent_MakeAvailable(sfl)

message(STATUS "Fetching zydis (bd73bc03b0aacaa89c9c203b9b43cd08f1b1843b)...")
FetchContent_Declare(zydis SYSTEM
	GIT_REPOSITORY
		"https://github.com/zyantific/zydis"
	GIT_TAG
		bd73bc03b0aacaa89c9c203b9b43cd08f1b1843b
)
FetchContent_MakeAvailable(zydis)

message(STATUS "Fetching GTest (v1.15.2)...")
FetchContent_Declare(GTest SYSTEM
	GIT_REPOSITORY
		"https://github.com/google/googletest"
	GIT_TAG
		v1.15.2
)
FetchContent_MakeAvailable(GTest)
