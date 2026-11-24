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

# 1. The complete body of is_synchronous_request(). Only one scaffold is
#    accepted, and every non-case line must match it exactly: an upstream change
#    of the return expressions, of the default branch or of an argument-dependent
#    condition therefore fails instead of producing a plausible but wrong list.
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
#
#    The getOption case is the only case that does not return true, and only the
#    default branch may return anything else.
file(STRINGS "${_sync_cpp}" _sync_lines)
set(_predicate "")
set(_in_predicate FALSE)
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
        # Blank lines would not survive as list elements, so they are dropped;
        # every other statement must match exactly after stripping.
        string(STRIP "${_line}" _stripped)
        string(LENGTH "${_stripped}" _len)
        if(_len GREATER 0)
            list(APPEND _predicate "${_stripped}")
        endif()
    endif()
endforeach()

if(NOT _predicate)
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

set(_index 0)
set(_count 0)
foreach(_line IN LISTS _predicate)
    math(EXPR _count "${_count} + 1")
endforeach()

if(NOT _index LESS _count)
    _classifier_fail_message("empty body")
endif()
list(GET _predicate ${_index} _line)
if(NOT _line MATCHES "^switch[ \t]*\\([ \t]*function->get_id\\(\\)[ \t]*\\)[ \t]*\\{[ \t]*$")
    _classifier_fail_message("the first statement is not 'switch (function->get_id()) {': '${_line}'")
endif()
math(EXPR _index "${_index} + 1")

# The body is a flat token sequence. The method cases form one group that must
# end with the only `return true;` in the predicate. The getOption case is
# separate and returns an OptionManager call, and the default branch must
# return false.
set(_case_open FALSE)
set(_cases "")
set(_saw_true FALSE)
set(_saw_get_option FALSE)
set(_saw_default FALSE)
set(_closed FALSE)

while(_index LESS _count)
    list(GET _predicate ${_index} _line)
    math(EXPR _index "${_index} + 1")

    # Blank lines are already gone; CMake lists drop them, but a stray empty
    # element is still skipped so the scaffold check sees only statements.
    if(NOT _line MATCHES "[^ \t]")
        continue()
    endif()

    if(_closed)
        _classifier_fail_message("unexpected trailing statement '${_line}'")
    endif()

    set(_line_is_case FALSE)
    if(_line MATCHES "^case[ \t]+td_api::([A-Za-z0-9_]+)::ID:$")
        # `_name` is also the loop variable of the generation step below, so it
        # must not survive this block.
        set(_case_name "${CMAKE_MATCH_1}")
        if(_line MATCHES "^case[ \t]+td_api::getOption::ID:$")
            if(_case_open)
                _classifier_fail_message(
                    "a case follows a case group that does not return true")
            endif()
            if(_saw_get_option)
                _classifier_fail_message("getOption appears twice")
            endif()
            set(_saw_get_option TRUE)
        else()
            # Every method case belongs to the group that ends with return true.
            if(_saw_true)
                _classifier_fail_message(
                    "method case '${_case_name}' follows 'return true;'")
            endif()
            set(_case_open TRUE)
            list(APPEND _cases "${_case_name}")
        endif()
        set(_line_is_case TRUE)
    endif()
    if(_line_is_case)
        continue()
    endif()

    if(_line MATCHES "^return true")
        if(NOT _case_open)
            _classifier_fail_message("'return true;' without a preceding case")
        endif()
        if(_saw_true)
            _classifier_fail_message("a second 'return true;'")
        endif()
        set(_case_open FALSE)
        set(_saw_true TRUE)
        continue()
    endif()

    if(_line MATCHES "^return OptionManager::is_synchronous_option")
        if(_saw_get_option)
            continue()
        endif()
        _classifier_fail_message(
            "an OptionManager::is_synchronous_option return without a getOption case")
    endif()

    if(_line MATCHES "^default:$")
        if(_case_open)
            _classifier_fail_message("a case has no return before the default branch")
        endif()
        if(NOT _saw_get_option)
            _classifier_fail_message("the switch has no getOption case")
        endif()
        set(_saw_default TRUE)
        continue()
    endif()

    if(_line MATCHES "^return false")
        if(NOT _saw_default)
            _classifier_fail_message(
                "'return false;' outside the default branch")
        endif()
        continue()
    endif()

    if(_line MATCHES "^\\}$")
        if(NOT _saw_default)
            _classifier_fail_message("the switch has no default branch")
        endif()
        set(_closed TRUE)
        continue()
    endif()

    _classifier_fail_message("unrecognised statement '${_line}'")
endwhile()

if(NOT _closed)
    _classifier_fail_message("the switch is not closed")
endif()
if(_case_open)
    _classifier_fail_message("the method case group does not end with 'return true;'")
endif()
if(NOT _saw_true)
    _classifier_fail_message("the method case group never returns true")
endif()
if(NOT _saw_get_option)
    _classifier_fail_message("the switch has no getOption case")
endif()
if(NOT _saw_default)
    _classifier_fail_message("there is no default branch")
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

# getOption is synchronous only for its argument-dependent subset, but it does
# have a handler, so it belongs to the comparison.
list(APPEND _cases "getOption")
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

if(NOT _options)
    message(FATAL_ERROR
            "Could not read get_synchronous_options() from '${_option_cpp}'")
endif()

# The list alone does not prove the lookup: is_synchronous_option() must still
# be a membership test over get_synchronous_options().
set(_in_lookup FALSE)
set(_saw_contains FALSE)
foreach(_line IN LISTS _option_lines)
    if(_line MATCHES "^bool OptionManager::is_synchronous_option")
        set(_in_lookup TRUE)
        continue()
    endif()
    if(_in_lookup)
        if(_line MATCHES "^}")
            set(_in_lookup FALSE)
            continue()
        endif()
        if(_line MATCHES "return[ \t]+td::contains\\([ \t]*get_synchronous_options\\(\\)[ \t]*,[ \t]*name[ \t]*\\);[ \t]*$")
            set(_saw_contains TRUE)
        endif()
    endif()
endforeach()

if(NOT _saw_contains)
    message(FATAL_ERROR
            "OptionManager::is_synchronous_option() is no longer a membership "
            "test over get_synchronous_options(); the option classifier must be "
            "reviewed in '${_option_cpp}'.")
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
