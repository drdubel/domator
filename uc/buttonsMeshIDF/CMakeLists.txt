# Top-level CMakeLists.txt for buttonsMeshIDF
cmake_minimum_required(VERSION 3.16)

# Set IDF path before including IDF project
set(IDF_PATH $ENV{IDF_PATH})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(buttonsMeshIDF)
