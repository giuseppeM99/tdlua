// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <string>

namespace tdlua {

// TDLib 1.8.70 has exactly two ways to say "this operation cannot run through
// td_execute/ClientManager::execute": the generic SynchronousRequests template
// and getOption for a non-synchronous option. Both are reported as error 400
// with these literal messages.
//
// No other 400 authorizes an asynchronous retry. In particular a method that
// really ran and rejected its input (parseMarkdown with empty text, for
// example) returns its own message and must be delivered as a normal TDLib
// error response, never re-executed.
inline bool isAsyncExecuteFallback(int code, const std::string &message)
{
    if (code != 400) return false;
    return message == "The method can't be executed synchronously" ||
           message == "The option can't be get synchronously";
}

} // namespace tdlua
