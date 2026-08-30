function(CHECK_COMPILERS)
    message(STATUS "Checking compilers")

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        if(ENABLE_LIBCXX)
            find_library(LIBC++ c++)
            find_library(LIBC++ABI c++abi)
            if(NOT LIBC++ OR NOT LIBC++ABI)
                message(
                    FATAL_ERROR
                    "libc++ or libc++abi not found. Make sure you have libc++ and libc++abi installed and in your PATH"
                )
            endif()
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "12.0.0")
            message(FATAL_ERROR "GCC-12 or higher is required")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # VS 2022 17.0 == MSVC 19.30; C++20 support requires at least that.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.30")
            message(FATAL_ERROR "MSVC 19.30 (Visual Studio 2022) or higher is required")
        endif()
    else()
        message(WARNING "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID} ! Only Clang and GCC are supported")
    endif()
endfunction()
