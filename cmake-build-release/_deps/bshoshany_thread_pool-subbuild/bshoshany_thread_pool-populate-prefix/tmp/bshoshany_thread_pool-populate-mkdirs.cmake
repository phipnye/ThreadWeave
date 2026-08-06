# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-src"
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-build"
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix"
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/tmp"
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp"
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src"
  "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/philip/Documents/Cpp_projects/ThreadWeave/cmake-build-release/_deps/bshoshany_thread_pool-subbuild/bshoshany_thread_pool-populate-prefix/src/bshoshany_thread_pool-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
