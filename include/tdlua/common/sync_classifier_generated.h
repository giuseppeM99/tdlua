// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Generated from the pinned TDLib 1.8.71 sources by
// tools/sync-classifier/generate_sync_classifier.cmake. Do not edit.
// Source of truth: td/telegram/SynchronousRequests.cpp,
// td/telegram/SynchronousRequests.h and td/telegram/OptionManager.cpp.
#pragma once

#include <string>

namespace tdlua {
namespace sync_classifier {

// The methods TDLib's SynchronousRequests declares executable.
inline bool isSynchronousMethod(const std::string &method)
{
    static const char *const methods[] = {
        "addLogMessage",
        "checkQuickReplyShortcutName",
        "cleanFileName",
        "getChatFolderDefaultIconName",
        "getCountryFlagEmoji",
        "getFileExtension",
        "getFileMimeType",
        "getJsonString",
        "getJsonValue",
        "getLanguagePackString",
        "getLogStream",
        "getLogTagVerbosityLevel",
        "getLogTags",
        "getLogVerbosityLevel",
        "getMarkdownText",
        "getOption",
        "getPhoneNumberInfoSync",
        "getPushReceiverId",
        "getTextEmojiRanges",
        "getTextEntities",
        "getThemeParametersJsonString",
        "parseMarkdown",
        "parseTextEntities",
        "searchQuote",
        "searchStringsByPrefix",
        "setLogStream",
        "setLogTagVerbosityLevel",
        "setLogVerbosityLevel",
        "testReturnError",
    };
    for (const char *candidate : methods) {
        if (method == candidate) return true;
    }
    return false;
}

// getOption() is synchronous only for the option names TDLib declares.
inline bool isSynchronousOption(const std::string &name)
{
    static const char *const options[] = {
        "commit_hash",
        "version",
    };
    for (const char *candidate : options) {
        if (name == candidate) return true;
    }
    return false;
}

} // namespace sync_classifier
} // namespace tdlua
