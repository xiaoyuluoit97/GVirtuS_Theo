# Written By: Theodoros Aslanidis <theodoros.aslanidis@ucdconnect.ie>,
#             School of Computer Science, University College Dublin
#
# This file has been modified to correctly link test executables against
# the refactored gVirtuS libraries and their dependencies.

enable_language(CUDA)
set(CMAKE_CUDA_STANDARD 17)

# --- Find or Fetch GoogleTest (Unchanged) ---
find_package(GTest QUIET)
if (NOT GTest_FOUND)
    message(STATUS "GTest not found, fetching with FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
      googlestest
      URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    )
    FetchContent_MakeAvailable(googletest)
    set(GTEST_INCLUDE_DIRS ${googletest_SOURCE_DIR}/include)
    set(GTEST_LIBRARIES gtest gtest_main)
endif()

# --- Define Test Sources (Unchanged) ---
set(TEST_SOURCES
    test_cudart.cu
    test_nvrtc.cu
    test_curand.cu
    test_cublas.cu
    test_cufft.cu
    test_cudnn.cu
    test_cudadr.cu
    test_cusolver.cu
    test_cusparse.cu
)

# --- Create Test Executables in a Loop (Linking Section is MODIFIED) ---
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    
    set_target_properties(${TEST_NAME} PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
    )

    # Include directories for GTest (Unchanged)
    target_include_directories(${TEST_NAME} PRIVATE
        ${GTEST_INCLUDE_DIRS}
    )

    # *** THIS IS THE FINAL, CORRECTED LINKING BLOCK FOR EACH TEST ***
    target_link_libraries(${TEST_NAME} PRIVATE
        # GoogleTest Libraries
        GTest::GTest
        GTest::Main

        # gVirtuS Core Libraries:
        # The test directly uses the frontend.
        gvirtus-frontend
        # The frontend and its dependencies require the common library.
        gvirtus-common
        
        # System-level Dependencies required by the gVirtuS stack:
        # For logging functionality used throughout the libraries.
        ${LIBLOG4CPLUS}
        # For std::filesystem used in frontend, backend, etc.
        stdc++fs
        # For std::thread, mutex, etc. used in the new async pipeline.
        Threads::Threads

        # CUDA Libraries required by the specific test (as before):
        cudart
        curand
        cublas
        cublasLt
        cufft
        cudnn
        cusolver
        cusparse
        nvrtc
        cuda # For CUDA Driver API
    )

    # Register the test with ctest (Unchanged)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()