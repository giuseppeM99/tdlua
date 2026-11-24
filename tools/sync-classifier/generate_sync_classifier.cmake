# Copyright (c) 2018-2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause
#
# Generate the preventive synchronous-request classifier from the pinned TDLib
# sources.
#
# TDLib decides which requests it can run through ClientManager::execute in
# `SynchronousRequests::is_synchronous_request()`. That predicate is private to
# TDLib and its header is not part of the installed package, so TDLua cannot
# call it from every supported build. Instead this script reads the
# authoritative source and emits two headers:
#
#   sync_classifier_generated.h     method names and synchronous option names
#                                   (used by the JSON backend, which only sees
#                                   the encoded JSON request)
#   sync_classifier_ids_generated.h td_api::*::ID switch over the same method
#                                   set (used by the native backend, which only
#                                   sees the encoded td_api object)
#
# Both headers are generated from one parse of the same file, so the backends
# cannot disagree with each other, and neither is hand-maintained.
#
# Usage:
#   cmake -DTDLIB_SOURCE_DIR=<checkout>/td -DTDLIB_VERSION=1.8.71 \
#         -DOUTPUT_DIR=<dir> -P tools/sync-classifier/generate_sync_classifier.cmake
#
# Builds verify the committed headers against the pinned sources; see the
# `synchronous request classifier` block in CMakeLists.txt.

if(NOT DEFINED TDLIB_SOURCE_DIR OR TDLIB_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "TDLIB_SOURCE_DIR is required")
endif()
if(NOT DEFINED OUTPUT_DIR OR OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()
if(NOT DEFINED TDLIB_VERSION)
    set(TDLIB_VERSION "unknown")
endif()

set(_sync_cpp "${TDLIB_SOURCE_DIR}/td/telegram/SynchronousRequests.cpp")
set(_sync_h "${TDLIB_SOURCE_DIR}/td/telegram/SynchronousRequests.h")
set(_option_cpp "${TDLIB_SOURCE_DIR}/td/telegram/OptionManager.cpp")
foreach(_input IN ITEMS "${_sync_cpp}" "${_sync_h}" "${_option_cpp}")
    if(NOT EXISTS "${_input}")
        message(FATAL_ERROR
                "The synchronous classifier requires the pinned TDLib sources; "
                "'${_input}' is missing. Initialise the td submodule.")
    endif()
endforeach()

# 1. Cases of is_synchronous_request(). The switch is the only place where
#    TDLib enumerates the methods ClientManager::execute can run.
file(STRINGS "${_sync_cpp}" _sync_lines)
set(_in_predicate FALSE)
set(_cases "")
set(_saw_get_option FALSE)
foreach(_line IN LISTS _sync_lines)
    if(_line MATCHES "^bool SynchronousRequests::is_synchronous_request")
        set(_in_predicate TRUE)
        continue()
    endif()
    if(_in_predicate)
        if(_line MATCHES "^}")
            set(_in_predicate FALSE)
            continue()
        endif()
        if(_line MATCHES "case[ \t]+td_api::([A-Za-z0-9_]+)::ID:")
            set(_name "${CMAKE_MATCH_1}")
            list(APPEND _cases "${_name}")
            if(_name STREQUAL "getOption")
                set(_saw_get_option TRUE)
            endif()
        elseif(_line MATCHES "getOption")
            set(_saw_get_option TRUE)
        endif()
    endif()
endforeach()

if(NOT _cases)
    message(FATAL_ERROR
            "No is_synchronous_request cases were found in '${_sync_cpp}'; "
            "the classifier source layout changed")
endif()

# 2. Declared do_request overloads. Every synchronous method has exactly one;
#    the generic template is not tied to a td_api type. If the switch and the
#    handlers disagree, the parse is incomplete and the generated classifier
#    would be wrong, so fail instead of emitting it.
file(STRINGS "${_sync_h}" _header_lines)
set(_overloads "")
foreach(_line IN LISTS _header_lines)
    if(_line MATCHES "do_request\\((const )?td_api::([A-Za-z0-9_]+) &request\\);")
        list(APPEND _overloads "${CMAKE_MATCH_2}")
    endif()
endforeach()

list(SORT _cases)
list(REMOVE_DUPLICATES _cases)
list(SORT _overloads)
list(REMOVE_DUPLICATES _overloads)

if(NOT _cases STREQUAL _overloads)
    message(FATAL_ERROR
            "The synchronous request switch and the declared handlers disagree.\n"
            "Switch cases: ${_cases}\nDeclared handlers: ${_overloads}")
endif()

if(NOT _saw_get_option)
    message(FATAL_ERROR
            "is_synchronous_request() no longer special-cases getOption; the "
            "option classifier must be reviewed")
endif()

# 3. Synchronous options of OptionManager::is_synchronous_option(). getOption
#    depends on its argument, so the method list alone is not enough.
file(STRINGS "${_option_cpp}" _option_lines)
set(_in_options FALSE)
set(_options "")
foreach(_line IN LISTS _option_lines)
    if(_line MATCHES "get_synchronous_options\\(\\)")
        set(_in_options TRUE)
    endif()
    if(_in_options AND _line MATCHES "options\\{([^}]*)\\}")
        string(REGEX MATCHALL "\"([A-Za-z0-9_]+)\"" _quoted "${_line}")
        foreach(_literal IN LISTS _quoted)
            string(REGEX REPLACE "\"([A-Za-z0-9_]+)\"" "\\1" _name "${_literal}")
            list(APPEND _options "${_name}")
        endforeach()
        set(_in_options FALSE)
    endif()
endforeach()

list(SORT _options)
list(REMOVE_DUPLICATES _options)

foreach(_required IN ITEMS version commit_hash)
    if(NOT _required IN_LIST _options)
        message(FATAL_ERROR
                "Could not read the synchronous option '${_required}' from "
                "'${_option_cpp}'; the classifier source layout changed "
                "(parsed options: ${_options})")
    endif()
endforeach()

set(_banner "// Copyright (c) 2018-2026 Giuseppe Marino\n")
string(APPEND _banner "// SPDX-License-Identifier: BSD-3-Clause\n")
string(APPEND _banner "//\n")
string(APPEND _banner "// Generated from the pinned TDLib ${TDLIB_VERSION} sources by\n")
string(APPEND _banner "// tools/sync-classifier/generate_sync_classifier.cmake. Do not edit.\n")
string(APPEND _banner "// Source of truth: td/telegram/SynchronousRequests.cpp,\n")
string(APPEND _banner "// td/telegram/SynchronousRequests.h and td/telegram/OptionManager.cpp.\n")

# 4. Name-based classifier for the JSON backend.
set(_names "${_banner}")
string(APPEND _names "#pragma once\n\n")
string(APPEND _names "#include <string>\n\n")
string(APPEND _names "namespace tdlua {\nnamespace sync_classifier {\n\n")
string(APPEND _names
       "// The methods TDLib's SynchronousRequests declares executable.\n")
string(APPEND _names
       "inline bool isSynchronousMethod(const std::string &method)\n{\n")
string(APPEND _names "    static const char *const methods[] = {\n")
foreach(_name IN LISTS _cases)
    string(APPEND _names "        \"${_name}\",\n")
endforeach()
string(APPEND _names "    };\n")
string(APPEND _names "    for (const char *candidate : methods) {\n")
string(APPEND _names "        if (method == candidate) return true;\n")
string(APPEND _names "    }\n")
string(APPEND _names "    return false;\n")
string(APPEND _names "}\n\n")
string(APPEND _names
       "// getOption() is synchronous only for the option names TDLib declares.\n")
string(APPEND _names
       "inline bool isSynchronousOption(const std::string &name)\n{\n")
string(APPEND _names "    static const char *const options[] = {\n")
foreach(_name IN LISTS _options)
    string(APPEND _names "        \"${_name}\",\n")
endforeach()
string(APPEND _names "    };\n")
string(APPEND _names "    for (const char *candidate : options) {\n")
string(APPEND _names "        if (name == candidate) return true;\n")
string(APPEND _names "    }\n")
string(APPEND _names "    return false;\n")
string(APPEND _names "}\n\n")
string(APPEND _names "} // namespace sync_classifier\n")
string(APPEND _names "} // namespace tdlua\n")

# 5. ID-based classifier for the native backend, which only sees the encoded
#    td_api::Function. The case labels are resolved by the same td_api.h the
#    library was built against, so a renamed or removed method fails to compile
#    instead of silently changing the classification.
set(_ids "${_banner}")
string(APPEND _ids "#pragma once\n\n")
string(APPEND _ids "#include <cstdint>\n\n")
string(APPEND _ids "#include <td/telegram/td_api.h>\n\n")
string(APPEND _ids "namespace tdlua {\nnamespace sync_classifier {\n\n")
string(APPEND _ids
       "// The same set as isSynchronousMethod(), as td_api type ids.\n")
string(APPEND _ids
       "inline bool isSynchronousMethodId(std::int32_t id)\n{\n")
string(APPEND _ids "    switch (id) {\n")
foreach(_name IN LISTS _cases)
    string(APPEND _ids "    case td::td_api::${_name}::ID:\n")
endforeach()
string(APPEND _ids "        return true;\n")
string(APPEND _ids "    default:\n")
string(APPEND _ids "        return false;\n")
string(APPEND _ids "    }\n")
string(APPEND _ids "}\n\n")
string(APPEND _ids "} // namespace sync_classifier\n")
string(APPEND _ids "} // namespace tdlua\n")

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/sync_classifier_generated.h" "${_names}")
file(WRITE "${OUTPUT_DIR}/sync_classifier_ids_generated.h" "${_ids}")
