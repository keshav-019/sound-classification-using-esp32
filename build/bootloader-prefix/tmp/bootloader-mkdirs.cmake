# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/sumanshu/esp/v5.3.2/esp-idf/components/bootloader/subproject"
  "/home/sumanshu/projects/i2s_recorder/build/bootloader"
  "/home/sumanshu/projects/i2s_recorder/build/bootloader-prefix"
  "/home/sumanshu/projects/i2s_recorder/build/bootloader-prefix/tmp"
  "/home/sumanshu/projects/i2s_recorder/build/bootloader-prefix/src/bootloader-stamp"
  "/home/sumanshu/projects/i2s_recorder/build/bootloader-prefix/src"
  "/home/sumanshu/projects/i2s_recorder/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/sumanshu/projects/i2s_recorder/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/sumanshu/projects/i2s_recorder/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
