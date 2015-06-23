//===--- MacroParenthesesCheck.cpp - clang-tidy----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MacroParenthesesCheck.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

namespace clang {
namespace tidy {

namespace {
class MacroParenthesesPPCallbacks : public PPCallbacks {
public:
  explicit MacroParenthesesPPCallbacks(Preprocessor *PP,
                                       MacroParenthesesCheck *Check)
      : PP(PP), Check(Check) {}

  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *MD) override {
    replacementList(MacroNameTok, MD->getMacroInfo());
    argument(MacroNameTok, MD->getMacroInfo());
  }

private:
  /// Replacement list with calculations should be enclosed in parentheses.
  void replacementList(const Token &MacroNameTok, const MacroInfo *MI);

  /// Arguments should be enclosed in parentheses.
  void argument(const Token &MacroNameTok, const MacroInfo *MI);

  Preprocessor *PP;
  MacroParenthesesCheck *Check;
};

/// Is argument surrounded properly with parentheses/braces/squares/commas?
bool isSurroundedLeft(const Token &T) {
  return T.isOneOf(tok::l_paren, tok::l_brace, tok::l_square, tok::comma,
                   tok::semi);
}

/// Is argument surrounded properly with parentheses/braces/squares/commas?
bool isSurroundedRight(const Token &T) {
  return T.isOneOf(tok::r_paren, tok::r_brace, tok::r_square, tok::comma,
                   tok::semi);
}

/// Is given TokenKind a keyword?
bool isKeyword(const Token &T) {
  /// \TODO better matching of keywords to avoid false positives
  return T.isOneOf(tok::kw_case, tok::kw_const, tok::kw_struct);
}

/// Warning is written when one of these operators are not within parentheses.
bool isWarnOp(const Token &T) {
   /// \TODO This is an initial list of operators. It can be tweaked later to
   /// get more positives or perhaps avoid some false positive.
  return T.isOneOf(tok::plus, tok::minus, tok::star, tok::slash, tok::percent,
                   tok::amp, tok::pipe, tok::caret);
}
}

void MacroParenthesesPPCallbacks::replacementList(const Token &MacroNameTok,
                                                  const MacroInfo *MI) {
  // Count how deep we are in parentheses/braces/squares.
  int Count = 0;

  // SourceLocation for error
  SourceLocation Loc;

  for (auto TI = MI->tokens_begin(), TE = MI->tokens_end(); TI != TE; ++TI) {
    const Token &Tok = *TI;
    // Replacement list contains keywords, don't warn about it.
    if (isKeyword(Tok))
      return;
    // When replacement list contains comma/semi don't warn about it.
    if (Count == 0 && Tok.isOneOf(tok::comma, tok::semi))
      return;
    if (Tok.isOneOf(tok::l_paren, tok::l_brace, tok::l_square)) {
      ++Count;
    } else if (Tok.isOneOf(tok::r_paren, tok::r_brace, tok::r_square)) {
      --Count;
      // If there are unbalanced parentheses don't write any warning
      if (Count < 0)
        return;
    } else if (Count == 0 && isWarnOp(Tok)) {
      // Heuristic for macros that are clearly not intended to be enclosed in
      // parentheses, macro starts with operator. For example:
      // #define X     *10
      if (TI == MI->tokens_begin() && (TI + 1) != TE &&
          !Tok.isOneOf(tok::plus, tok::minus))
        return;
      // Don't warn about this macro if the last token is a star. For example:
      // #define X    void *
      if ((TE - 1)->is(tok::star))
        return;

      Loc = Tok.getLocation();
    }
  }
  if (Loc.isValid()) {
    const Token &Last = *(MI->tokens_end() - 1);
    Check->diag(Loc, "macro replacement list should be enclosed in parentheses")
        << FixItHint::CreateInsertion(MI->tokens_begin()->getLocation(), "(")
        << FixItHint::CreateInsertion(Last.getLocation().getLocWithOffset(
                                          PP->getSpelling(Last).length()),
                                      ")");
  }
}

void MacroParenthesesPPCallbacks::argument(const Token &MacroNameTok,
                                           const MacroInfo *MI) {

  for (auto TI = MI->tokens_begin(), TE = MI->tokens_end(); TI != TE; ++TI) {
    // First token.
    if (TI == MI->tokens_begin())
      continue;

    // Last token.
    if ((TI + 1) == MI->tokens_end())
      continue;

    const Token &Prev = *(TI - 1);
    const Token &Next = *(TI + 1);

    const Token &Tok = *TI;

    // Only interested in identifiers.
    if (!Tok.isOneOf(tok::identifier, tok::raw_identifier))
      continue;

    // Only interested in macro arguments.
    if (MI->getArgumentNum(Tok.getIdentifierInfo()) < 0)
      continue;

    // Argument is surrounded with parentheses/squares/braces/commas.
    if (isSurroundedLeft(Prev) && isSurroundedRight(Next))
      continue;

    // Don't warn after hash/hashhash or before hashhash.
    if (Prev.isOneOf(tok::hash, tok::hashhash) || Next.is(tok::hashhash))
      continue;

    // Argument is a struct member.
    if (Prev.isOneOf(tok::period, tok::arrow, tok::coloncolon))
      continue;

    // String concatenation.
    if (isStringLiteral(Prev.getKind()) || isStringLiteral(Next.getKind()))
      continue;

    // Type/Var.
    if (isAnyIdentifier(Prev.getKind()) || isKeyword(Prev) ||
        isAnyIdentifier(Next.getKind()) || isKeyword(Next))
      continue;

    // Initialization.
    if (Next.is(tok::l_paren))
      continue;

    // Cast.
    if (Prev.is(tok::l_paren) && Next.is(tok::star) &&
        TI + 2 != MI->tokens_end() && (TI + 2)->is(tok::r_paren))
      continue;

    // Assignment/return, i.e. '=x;' or 'return x;'.
    if (Prev.isOneOf(tok::equal, tok::kw_return) && Next.is(tok::semi))
      continue;

    Check->diag(Tok.getLocation(), "macro argument should be enclosed in "
                                   "parentheses")
        << FixItHint::CreateInsertion(Tok.getLocation(), "(")
        << FixItHint::CreateInsertion(Tok.getLocation().getLocWithOffset(
                                          PP->getSpelling(Tok).length()),
                                      ")");
  }
}

void MacroParenthesesCheck::registerPPCallbacks(CompilerInstance &Compiler) {
  Compiler.getPreprocessor().addPPCallbacks(
      llvm::make_unique<MacroParenthesesPPCallbacks>(
          &Compiler.getPreprocessor(), this));
}

} // namespace tidy
} // namespace clang