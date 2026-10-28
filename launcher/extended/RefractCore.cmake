# SPDX-License-Identifier: GPL-3.0-only
# Refract Launcher: optional bundling of the Refract core (the Node side of the bridge) next to the executable.
#   -DREFRACT_CORE_DIR=<DevKit checkout>  installs its src/, schemas/ and package.json into refract-core/
#   -DREFRACT_NODE_EXE=<path to node.exe>  also installs that Node runtime as refract-core/node(.exe)
# Without these options nothing is installed and the bridge looks for PRISM_EXT_NODE / PRISM_EXT_CORE or node on
# PATH (CoreClient::discover).
set(REFRACT_CORE_DIR "" CACHE PATH "Refract core (DevKit) checkout to bundle into refract-core/")
set(REFRACT_NODE_EXE "" CACHE FILEPATH "Node.js runtime to bundle as refract-core/node")
# BINARY_DEST_DIR is "." on Windows; "./refract-core" would trip CMake policy CMP0177 (path normalization).
if(BINARY_DEST_DIR STREQUAL "." OR BINARY_DEST_DIR STREQUAL "")
    set(REFRACT_CORE_DEST "refract-core")
else()
    set(REFRACT_CORE_DEST "${BINARY_DEST_DIR}/refract-core")
endif()

if(REFRACT_CORE_DIR)
    if(NOT EXISTS "${REFRACT_CORE_DIR}/src/ipc-server.mjs")
        message(FATAL_ERROR "REFRACT_CORE_DIR does not contain src/ipc-server.mjs: ${REFRACT_CORE_DIR}")
    endif()
    install(DIRECTORY "${REFRACT_CORE_DIR}/src/" DESTINATION "${REFRACT_CORE_DEST}/src" FILES_MATCHING PATTERN "*.mjs")
    install(DIRECTORY "${REFRACT_CORE_DIR}/schemas/" DESTINATION "${REFRACT_CORE_DEST}/schemas" FILES_MATCHING PATTERN "*.json")
    install(FILES "${REFRACT_CORE_DIR}/package.json" DESTINATION "${REFRACT_CORE_DEST}")
endif()

if(REFRACT_NODE_EXE)
    if(NOT EXISTS "${REFRACT_NODE_EXE}")
        message(FATAL_ERROR "REFRACT_NODE_EXE does not exist: ${REFRACT_NODE_EXE}")
    endif()
    if(WIN32)
        install(PROGRAMS "${REFRACT_NODE_EXE}" DESTINATION "${REFRACT_CORE_DEST}" RENAME node.exe)
    else()
        install(PROGRAMS "${REFRACT_NODE_EXE}" DESTINATION "${REFRACT_CORE_DEST}" RENAME node)
    endif()
endif()
