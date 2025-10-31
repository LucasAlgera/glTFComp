cmake_minimum_required(VERSION 3.14)
project(glTFcomp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Python
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

message(STATUS "Python executable: ${Python3_EXECUTABLE}")
message(STATUS "Python version: ${Python3_VERSION}")
message(STATUS "Python include dirs: ${Python3_INCLUDE_DIRS}")
message(STATUS "Python libraries: ${Python3_LIBRARIES}")

# Add pybind11 and tinygltf and draco include directory

add_subdirectory(external/draco
		${CMAKE_BINARY_DIR}/draco)

add_subdirectory(external/miniz
		${CMAKE_BINARY_DIR}/miniz)


# Create the module with C++ files
Python3_add_library(glTFCompL MODULE 
	src/pybind_wrapper.cpp
	src/comp_func.cpp
	src/gltf_loader.cpp
)

target_include_directories(glTFCompL PRIVATE
    	external/pybind11/include
    	external/tinygltf
    	external/draco/src
    	${CMAKE_BINARY_DIR}/draco
	${CMAKE_BINARY_DIR}
	miniz
	${CMAKE_BINARY_DIR}/miniz
)

target_link_libraries(glTFCompL PRIVATE 
	Python3::Python
	draco
	miniz)

# Set properties 
set_target_properties(glTFCompL PROPERTIES 
    CXX_STANDARD 17 
    CXX_STANDARD_REQUIRED ON
    PREFIX ""  # Remove 'lib' prefix
)

# Platform-specific suffix handling (FROM CLAUDE)
if(WIN32)
    set_target_properties(glTFCompL PROPERTIES SUFFIX ".pyd")
else()
    set_target_properties(glTFCompL PROPERTIES SUFFIX ".so")
endif()

# Create __init__.py if it doesn't exist (FROM CLAUDE)
set(INIT_PY "${CMAKE_CURRENT_BINARY_DIR}/__init__.py")
if(NOT EXISTS ${INIT_PY})
    file(WRITE ${INIT_PY} "")
endif()

# .pyd file shows up in build directory but is also needed in scripts folder so we copy the file into /scripts:
# Copy the built module to the binary directory
add_custom_command(TARGET glTFCompL POST_BUILD 
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:glTFCompL>
        ${CMAKE_CURRENT_BINARY_DIR}/$<TARGET_FILE_NAME:glTFCompL>
)

add_custom_command(TARGET glTFCompL POST_BUILD 
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:glTFCompL>
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/$<TARGET_FILE_NAME:glTFCompL>
    COMMENT "Copying module to scripts folder"
)