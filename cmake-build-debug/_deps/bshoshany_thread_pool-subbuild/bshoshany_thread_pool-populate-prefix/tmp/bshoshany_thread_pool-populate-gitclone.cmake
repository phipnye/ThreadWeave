# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

if(EXISTS "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitclone-lastrun.txt" AND EXISTS "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitinfo.txt" AND
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitclone-lastrun.txt" IS_NEWER_THAN "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitinfo.txt")
  message(STATUS
    "Avoiding repeated git clone, stamp file is up to date: "
    "'/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitclone-lastrun.txt'"
  )
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-src"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: '/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-src'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "/usr/bin/git"
            clone --no-checkout --config "advice.detachedHead=false" "https://github.com/bshoshany/thread-pool.git" "bshoshany_thread_pool-src"
    WORKING_DIRECTORY "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps"
    RESULT_VARIABLE error_code
  )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(STATUS "Had to git clone more than once: ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/bshoshany/thread-pool.git'")
endif()

execute_process(
  COMMAND "/usr/bin/git"
          checkout "v5.1.0" --
  WORKING_DIRECTORY "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-src"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: 'v5.1.0'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "/usr/bin/git" 
            submodule update --recursive --init 
    WORKING_DIRECTORY "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-src"
    RESULT_VARIABLE error_code
  )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: '/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-src'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitinfo.txt" "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: '/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-debug/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/bshoshany_thread_pool-populate-gitclone-lastrun.txt'")
endif()
