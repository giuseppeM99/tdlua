// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Generated from the pinned TDLib 1.8.71 sources by
// tools/sync-classifier/generate_sync_classifier.cmake. Do not edit.
// Source of truth: td/telegram/SynchronousRequests.cpp,
// td/telegram/SynchronousRequests.h and td/telegram/OptionManager.cpp.
#pragma once

#include <cstdint>

#include <td/telegram/td_api.h>

namespace tdlua {
namespace sync_classifier {

// The same set as isSynchronousMethod(), as td_api type ids.
inline bool isSynchronousMethodId(std::int32_t id)
{
    switch (id) {
    case td::td_api::addLogMessage::ID:
    case td::td_api::checkQuickReplyShortcutName::ID:
    case td::td_api::cleanFileName::ID:
    case td::td_api::getChatFolderDefaultIconName::ID:
    case td::td_api::getCountryFlagEmoji::ID:
    case td::td_api::getFileExtension::ID:
    case td::td_api::getFileMimeType::ID:
    case td::td_api::getJsonString::ID:
    case td::td_api::getJsonValue::ID:
    case td::td_api::getLanguagePackString::ID:
    case td::td_api::getLogStream::ID:
    case td::td_api::getLogTagVerbosityLevel::ID:
    case td::td_api::getLogTags::ID:
    case td::td_api::getLogVerbosityLevel::ID:
    case td::td_api::getMarkdownText::ID:
    case td::td_api::getOption::ID:
    case td::td_api::getPhoneNumberInfoSync::ID:
    case td::td_api::getPushReceiverId::ID:
    case td::td_api::getTextEmojiRanges::ID:
    case td::td_api::getTextEntities::ID:
    case td::td_api::getThemeParametersJsonString::ID:
    case td::td_api::parseMarkdown::ID:
    case td::td_api::parseTextEntities::ID:
    case td::td_api::searchQuote::ID:
    case td::td_api::searchStringsByPrefix::ID:
    case td::td_api::setLogStream::ID:
    case td::td_api::setLogTagVerbosityLevel::ID:
    case td::td_api::setLogVerbosityLevel::ID:
    case td::td_api::testReturnError::ID:
        return true;
    default:
        return false;
    }
}

} // namespace sync_classifier
} // namespace tdlua
