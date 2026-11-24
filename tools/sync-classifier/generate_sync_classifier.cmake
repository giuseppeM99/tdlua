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

# Run as a script: this sets the policy baseline so IN_LIST below is the new
# operator on every supported CMake version, not only on recent defaults.
cmake_minimum_required(VERSION 3.10)

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

# 1. The complete body of is_synchronous_request(), matched as a whole.
#
#    The body is flattened to one line with single spaces, then reduced to its
#    exact scaffold. Nothing but the case labels may vary, so an extra
#    condition, a different return expression or a second default branch fails
#    instead of producing a plausible but wrong method list.
#
#    Accepted body:
#
#      switch (function->get_id()) {
#        case td_api::a::ID:
#        case td_api::b::ID:
#          return true;
#        case td_api::getOption::ID:
#          return OptionManager::is_synchronous_option(
#              static_cast<const td_api::getOption *>(function)->name_);
#        default:
#          return false;
#      }
file(STRINGS "${_sync_cpp}" _sync_body_lines)
set(_sync_body "")
set(_in_predicate FALSE)
foreach(_line IN LISTS _sync_body_lines)
    if(_line MATCHES "^bool SynchronousRequests::is_synchronous_request")
        set(_in_predicate TRUE)
        continue()
    endif()
    if(_in_predicate)
        if(_line MATCHES "^}")
            set(_in_predicate FALSE)
            continue()
        endif()
        string(APPEND _sync_body " ${_line}")
    endif()
endforeach()

if(_sync_body STREQUAL "")
    message(FATAL_ERROR
            "is_synchronous_request() was not found in '${_sync_cpp}'; "
            "the classifier source layout changed")
endif()

# A macro, not a function: the messages quote this file's own variables.
macro(_classifier_fail_message reason)
    message(FATAL_ERROR
            "is_synchronous_request() no longer matches the scaffold the "
            "synchronous classifier is generated from: ${reason}\n"
            "Review the TDLib predicate, then update "
            "tools/sync-classifier/generate_sync_classifier.cmake.")
endmacro()

# Line wrapping and indentation are not semantic, so they are normalised; the
# statements themselves must match exactly.
string(REGEX REPLACE "[ \t\r\n]+" " " _sync_body "${_sync_body}")
string(REGEX REPLACE "^ " "" _sync_body "${_sync_body}")
string(REGEX REPLACE " $" "" _sync_body "${_sync_body}")
string(REGEX REPLACE " ?([{}();:,*&]) ?" "\\1" _sync_body "${_sync_body}")
string(REGEX REPLACE " ?-> ?" "->" _sync_body "${_sync_body}")

set(_prefix "switch(function->get_id()){")
set(_mid_tail "case td_api::getOption::ID:return OptionManager::is_synchronous_option(static_cast<const td_api::getOption*>(function)->name_);")
set(_suffix "default:return false;}")

if(NOT _sync_body MATCHES "^switch\\(function->get_id\\(\\)\\)\\{")
    _classifier_fail_message(
        "the predicate no longer starts with 'switch (function->get_id()) {', so "
        "something runs before the classification: '${_sync_body}'")
endif()
string(LENGTH "${_prefix}" _prefix_length)
string(SUBSTRING "${_sync_body}" ${_prefix_length} -1 _sync_body)
string(LENGTH "${_suffix}" _suffix_length)
string(LENGTH "${_sync_body}" _sync_body_length)
math(EXPR _mid_length "${_sync_body_length} - ${_suffix_length}")
if(_mid_length LESS 1)
    _classifier_fail_message("the predicate is shorter than its scaffold")
endif()
string(SUBSTRING "${_sync_body}" ${_mid_length} -1 _sync_suffix)
string(SUBSTRING "${_sync_body}" 0 ${_mid_length} _sync_mid)
if(NOT _sync_suffix STREQUAL "${_suffix}")
    _classifier_fail_message(
        "the predicate no longer ends with 'default: return false; }': found "
        "'${_sync_suffix}'")
endif()

# The getOption branch is the last case and the only conditional one.
string(LENGTH "${_mid_tail}" _tail_length)
string(LENGTH "${_sync_mid}" _mid_full_length)
math(EXPR _cases_length "${_mid_full_length} - ${_tail_length}")
if(_cases_length LESS 1)
    _classifier_fail_message(
        "the predicate has no method cases before the getOption branch")
endif()
string(SUBSTRING "${_sync_mid}" ${_cases_length} -1 _sync_tail)
string(SUBSTRING "${_sync_mid}" 0 ${_cases_length} _sync_cases)
if(NOT _sync_tail STREQUAL "${_mid_tail}")
    _classifier_fail_message(
        "the getOption branch is not the exact scaffold expression, or it is not "
        "the last case: found '${_sync_tail}'")
endif()

# What is left is the method group: case labels followed by one return true.
string(REGEX REPLACE "return true;$" "" _sync_cases "${_sync_cases}")
if(_sync_cases MATCHES "return")
    _classifier_fail_message(
        "the method group contains a return other than one trailing "
        "'return true;': '${_sync_cases}'")
endif()

string(REGEX MATCHALL "case td_api::[A-Za-z0-9_]+::ID:" _case_labels "${_sync_cases}")
if(NOT _case_labels)
    _classifier_fail_message("no method cases were found")
endif()

# Removing every recognised label must leave nothing behind, so an extra
# statement cannot hide between the cases.
set(_case_residue "${_sync_cases}")
foreach(_label IN LISTS _case_labels)
    string(REPLACE "${_label}" "" _case_residue "${_case_residue}")
    string(REGEX REPLACE "^case td_api::([A-Za-z0-9_]+)::ID:$" "\\1" _case_name "${_label}")
    list(APPEND _cases "${_case_name}")
endforeach()
if(NOT _case_residue STREQUAL "")
    _classifier_fail_message(
        "the method group contains unrecognised text: '${_case_residue}'")
endif()

list(SORT _cases)
list(REMOVE_DUPLICATES _cases)

# getOption is synchronous only for its argument-dependent subset, and it does
# have a handler, so it belongs to the comparison below.
list(APPEND _cases "getOption")
list(SORT _cases)

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

list(SORT _overloads)
list(REMOVE_DUPLICATES _overloads)

if(NOT _cases STREQUAL _overloads)
    message(FATAL_ERROR
            "The synchronous request switch and the declared handlers disagree.\n"
            "Switch cases: ${_cases}\nDeclared handlers: ${_overloads}")
endif()


# 3. Synchronous options of OptionManager::is_synchronous_option(). getOption
#    depends on its argument, so the method list alone is not enough.
file(STRINGS "${_option_cpp}" _option_lines)

# The list itself, and the lookup that must be a membership test over it.
set(_in_options FALSE)
set(_options "")
set(_in_lookup FALSE)
set(_lookup_body "")
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

    if(_line MATCHES "^bool OptionManager::is_synchronous_option")
        set(_in_lookup TRUE)
        continue()
    endif()
    if(_in_lookup)
        if(_line MATCHES "^}")
            set(_in_lookup FALSE)
            continue()
        endif()
        string(APPEND _lookup_body " ${_line}")
    endif()
endforeach()

if(NOT _options)
    message(FATAL_ERROR
            "Could not read get_synchronous_options() from '${_option_cpp}'")
endif()

if(_lookup_body STREQUAL "")
    message(FATAL_ERROR
            "OptionManager::is_synchronous_option() was not found in "
            "'${_option_cpp}'")
endif()

string(REGEX REPLACE "[ \t\r\n]+" " " _lookup_body "${_lookup_body}")
string(REGEX REPLACE "^ " "" _lookup_body "${_lookup_body}")
string(REGEX REPLACE " $" "" _lookup_body "${_lookup_body}")
string(REGEX REPLACE " ?([{}();:,*&]) ?" "\\1" _lookup_body "${_lookup_body}")

# The whole body must be the membership test: a conditional return before it
# would change the classification while leaving the list intact.
set(_expected_lookup
    "return td::contains(get_synchronous_options(),name);")
if(NOT _lookup_body STREQUAL "${_expected_lookup}")
    message(FATAL_ERROR
            "OptionManager::is_synchronous_option() is no longer exactly "
            "'${_expected_lookup}'.\nFound: '${_lookup_body}'\n"
            "The option classifier must be reviewed in '${_option_cpp}'.")
endif()

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

# The scaffold this generator accepts is the only one it can recognise; a
# different body fails above instead of being emitted.
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/sync_classifier_generated.h" "${_names}")
file(WRITE "${OUTPUT_DIR}/sync_classifier_ids_generated.h" "${_ids}")
