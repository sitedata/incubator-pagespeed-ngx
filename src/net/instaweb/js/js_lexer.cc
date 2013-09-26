// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jmarantz@google.com
//
// Implements a permissive javascript lexer.  This is not designed to reject
// all illegal javascript programs, but it attempts to accept all legal ones.
// No attempt is made to decode unicode characters.  Comments and whitepace
// are outputs of the tokenization process.  The token-stream can be used
// to easily reconstruct a byte-identical version of the javascript file.
//
// There are likely a few inaccuracies -- e.g. incorrect tokenization of
// multi-character operators.  The regex recognition is heuristic and might
// be incorrect in some cases.
//
// This has only been unit-tested, and has not yet been subjected to "street
// javascript" by scanning the web.
//
// This is based on third_party/libpagespeed/src/pagespeed/js/js_minify.cc by
// mdsteele@google.com

#include "net/instaweb/js/public/js_lexer.h"

#include <cstring>  // for strchr

#include "base/logging.h"
#include "pagespeed/kernel/js/js_keywords.h"
#include "net/instaweb/util/public/string_util.h"

using pagespeed::JsKeywords;

namespace net_instaweb {

#define CALL_MEMBER_FN(object, ptrToMember) ((object)->*(ptrToMember))

JsLexer::JsLexer() {
  // Initialize the keywords from HtmlName into a reverse table.  This
  // could have been generated by gperf, but it isn't.  It's easy
  // enough to build it given an iterator.
  //
  // TODO(jmarantz): make a static Init/Terminate routine to avoid doing this
  // on every lexer instantiation.
  keyword_vector_.resize(JsKeywords::num_keywords() + 1);
  DCHECK_EQ(JsKeywords::num_keywords(), JsKeywords::kNotAKeyword);
  for (JsKeywords::Iterator iter; !iter.AtEnd(); iter.Next()) {
    keyword_vector_[iter.keyword()] = iter.name();
  }
  keyword_vector_[JsKeywords::kNotAKeyword] = NULL;
}

JsKeywords::Type JsLexer::IdentifierOrKeyword(const StringPiece& name) {
  JsKeywords::Flag flag;
  JsKeywords::Type type = JsKeywords::Lookup(name, &flag);
  if (type == JsKeywords::kNotAKeyword) {
    last_token_may_end_value_ = true;
    return JsKeywords::kIdentifier;
  }
  last_token_may_end_value_ = (flag == JsKeywords::kIsValue);
  return type;
}

JsKeywords::Type JsLexer::NumberOrDot(const StringPiece& number_or_dot) {
  if (number_or_dot == ".") {
    last_token_may_end_value_ = false;
    return JsKeywords::kOperator;
  }
#ifndef NDEBUG
  int num_dots = 0;
  for (int i = 0, n = number_or_dot.size(); i < n; ++i) {
    if (number_or_dot[i] == '.') {
      ++num_dots;
    }
  }
  DCHECK_GE(1, num_dots);
#endif
  last_token_may_end_value_ = true;
  return JsKeywords::kNumber;
}

void JsLexer::Consume(LexicalPredicate predicate,
                      bool include_last_char,
                      bool ok_to_terminate_with_eof,
                      StringPiece* token) {
  DCHECK_LT(index_, static_cast<int>(input_.size()));
  const char* start = input_.data() + index_;
  const char* end = input_.data() + input_.size();
  prev_char_ = *start;
  const char* p = start + 1;
  for (int i = 1; (p < end) && CALL_MEMBER_FN(this, predicate)(*p, i);
       ++p, ++i) {
    prev_char_ = *p;
  }

  int size = p - start;
  if (p == end) {
    error_ |= !ok_to_terminate_with_eof;
  } else if (include_last_char) {
    ++size;
  }
  index_ += size;
  *token = StringPiece(start, size);
}

bool JsLexer::IsSpace(uint8 ch, int index) {
  return (strchr(" \t\f", ch) != NULL);
}

bool JsLexer::IsLineSeparator(uint8 ch, int index) {
  return (strchr("\n\r", ch) != NULL);
}

bool JsLexer::IsNumber(uint8 ch, int index) {
  // TODO(jmarantz): deal with hex/octal?
  //
  // Note that '.' by itself is not a number but its own token.  Thus
  // the callback method called for numbers is NumberOrDot which figures
  // out what to do given the context of the whole token.
  if (ch == '.') {
    if (seen_a_dot_) {
      return false;
    }
    seen_a_dot_ = true;
  }
  return (((ch >= '0') && (ch <= '9')) || (ch == '.'));
}

bool JsLexer::InBlockComment(uint8 ch, int index) {
  bool at_end = ((prev_char_ == '*') && (ch == '/'));
  return !at_end;
}

bool JsLexer::InSingleLineComment(uint8 ch, int index) {
  bool at_end = (ch == '\n') || (ch == '\r');
  return !at_end;
}

bool JsLexer::ProcessBackslash(uint8 ch) {
  if (backslash_mode_) {
    backslash_mode_ = false;
    return true;
  }
  if (ch == '\\') {
    backslash_mode_ = true;
    return true;
  }
  return false;
}

// See http://www.ecma-international.org/publications/files/ECMA-ST/Ecma-262.pdf
// page 17.
//
// Note this this algorithm errs on the side of allowing invalid characters into
// an identifier.
bool JsLexer::IdentifierStart(uint8 ch) {
  // Note that backslashes can appear in identifiers due to unicode escape
  // sequences (e.g. \u03c0).  We still terminate the identifier using
  // the same rules and make no attempt to decode the escape sequence.
  if (ProcessBackslash(ch)) {
    return true;
  }
  return ((ch >= 'a' && ch <= 'z') ||
          (ch >= 'A' && ch <= 'Z') ||
          (ch == '_') || (ch == '$') || (ch >= 127));
}

bool JsLexer::InIdentifier(uint8 ch, int index) {
  return (IdentifierStart(ch) || (ch >= '0' && ch <= '9'));
}

bool JsLexer::InOperator(uint8 ch, int index) {
  // TODO(jmarantz): add missing token-types !=, ==, ===, and others
  // listed in the ecmascript doc.
  if ((((token_start_ == '+') || (token_start_ == '-')) &&  // ++ --
       (ch == token_start_)) ||
      ((ch == '=') &&                                       // += -= *= /=
       ((token_start_ == '+') || (token_start_ == '-') ||
        (token_start_ == '/') || (token_start_ == '*')))) {
    // Treat -- and ++ as a single token.
    token_start_ = '\0';  // don't make a triple-plus or triple-minus.
    return true;
  }
  last_token_may_end_value_ = (strchr(")]}", ch) != NULL);
  return false;
}

bool JsLexer::InString(uint8 ch, int index) {
  if (ProcessBackslash(ch)) {
    return true;
  }
  return (token_start_ != ch);
}

bool JsLexer::InRegex(uint8 ch, int index) {
  if (ProcessBackslash(ch)) {
    return true;
  }
  if (ch == '/') {
    // Slashes within brackets are implicitly escaped.
    if (!within_brackets_) {
      return false;
    }
  } else if (ch == '[') {
    // Regex brackets don't nest, so we don't need a stack -- just a bool.
    within_brackets_ = true;
  } else if (ch == ']') {
    within_brackets_ = false;
  } else if (ch == '\n') {
    error_ = true;
    return false;
  }
  return true;
}

void JsLexer::Lex(const StringPiece& input) {
  input_ = input;
  index_ = 0;
  error_ = false;
  last_token_may_end_value_ = false;
  prev_char_ = '\0';
  token_start_ = '\0';
  backslash_mode_ = false;
  within_brackets_ = false;
  token_start_index_ = -1;
  seen_a_dot_ = false;
}

JsKeywords::Type JsLexer::NextToken(StringPiece* token) {
  bool end_of_input = (index_ >= static_cast<int>(input_.size()));
  if (end_of_input || error_) {
    return JsKeywords::kEndOfInput;
  }

  uint8 ch = input_[index_];
  token_start_ = ch;
  token_start_index_ = index_;

  // TODO(jmarantz): we can make a jump-table to map characters to an
  // enum to be used in a switch-statement.  If we care about speed
  // this would probably be the first thing to do.
  if (IsSpace(ch, 0)) {
    Consume(&JsLexer::IsSpace, false, true, token);
    return JsKeywords::kWhitespace;
    // last_token_may_end_value_ does not change.
  } else if (IsLineSeparator(ch, 0)) {
    Consume(&JsLexer::IsLineSeparator, false, true, token);
    return JsKeywords::kLineSeparator;
  } else if (IsNumber(ch, 0)) {
    seen_a_dot_ = (ch == '.');
    Consume(&JsLexer::IsNumber, false, true, token);
    seen_a_dot_ = false;
    return NumberOrDot(*token);
  } else if (ch == '/') {
    // Decide whether this is a comment or a regex.
    return ConsumeSlash(token);
  } else if ((ch == '"') || (ch == '\'')) {
    token_start_ = ch;
    Consume(&JsLexer::InString, true, false, token);
    last_token_may_end_value_ = true;
    return JsKeywords::kStringLiteral;
  } else if (IdentifierStart(ch)) {
    Consume(&JsLexer::InIdentifier, false, true, token);
    return IdentifierOrKeyword(*token);
  } else if (input_.substr(index_, 4) == "<!--") {
    Consume(&JsLexer::InSingleLineComment, false, true, token);
    return JsKeywords::kComment;
  }
  // all other punctuation is a token.
  token_start_ = ch;
  Consume(&JsLexer::InOperator, false, true, token);
  return JsKeywords::kOperator;
}

JsKeywords::Type JsLexer::ConsumeSlash(StringPiece* token) {
  // A slash could herald a line comment, a block comment, a regex literal,
  // or a mere division operator; we need to figure out which it is.
  // Differentiating between division and regexes is mostly impossible
  // without parsing, so we do our best based on the previous token.
  if (index_ < static_cast<int>(input_.size() - 1)) {
    char next = input_[index_ + 1];
    if (next == '/') {
      Consume(&JsLexer::InSingleLineComment, false, true, token);
      return JsKeywords::kComment;
    } else if (next == '*') {
      Consume(&JsLexer::InBlockComment, true, false, token);
      return JsKeywords::kComment;
    } else if (last_token_may_end_value_) {
      last_token_may_end_value_ = false;
    } else {
      within_brackets_ = false;
      Consume(&JsLexer::InRegex, true, false, token);
      return JsKeywords::kRegex;
    }
  }
  Consume(&JsLexer::InOperator, false, true, token);
  last_token_may_end_value_ = false;
  return JsKeywords::kOperator;
}

}  // namespace net_instaweb