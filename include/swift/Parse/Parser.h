//===--- Parser.h - Swift Language Parser -----------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines the Parser interface.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PARSER_H
#define SWIFT_PARSER_H

#include "swift/AST/AST.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/LocalContext.h"
#include "swift/Parse/PersistentParserState.h"
#include "swift/Parse/Token.h"
#include "swift/Parse/ParserResult.h"
#include "swift/Basic/Optional.h"
#include "swift/Basic/OptionSet.h"
#include "llvm/ADT/SetVector.h"

namespace llvm {
  template <typename PT1, typename PT2, typename PT3> class PointerUnion3;
}

namespace swift {
  class DiagnosticEngine;
  class Lexer;
  class ScopeInfo;
  struct TypeLoc;
  class TupleType;
  class SILParserState;
  class SourceManager;
  class PersistentParserState;
  class CodeCompletionCallbacks;
  class DelayedParsingCallbacks;
  
  struct EnumElementInfo;
  
  /// Different contexts in which BraceItemList are parsed.
  enum class BraceItemListKind {
    /// A statement list terminated by a closing brace. The default.
    Brace,
    /// A statement list in a variable getter or setter. The list
    /// is terminated by a closing brace or a 'get:' or 'set:' label.
    Variable,
    /// A statement list in a case block. The list is terminated
    /// by a closing brace or a 'case' or 'default' label.
    Case,
    /// The top-level of a file, when not in parse-as-library mode (i.e. the
    /// repl or a script).
    TopLevelCode,
    /// The top-level of a file, when in parse-as-library mode.
    TopLevelLibrary,
  };

class Parser {
  Parser(const Parser&) = delete;
  void operator=(const Parser&) = delete;

  bool IsInputIncomplete = false;

public:
  SourceManager &SourceMgr;
  const unsigned BufferID;
  DiagnosticEngine &Diags;
  SourceFile &SF;
  Lexer *L;
  SILParserState *SIL;    // Non-null when parsing a .sil file.
  PersistentParserState *State;
  std::unique_ptr<PersistentParserState> OwnedState;
  DeclContext *CurDeclContext;
  ASTContext &Context;
  CodeCompletionCallbacks *CodeCompletion = nullptr;
  std::vector<std::vector<VarDecl*>> AnonClosureVars;
  std::pair<const DeclContext *, ArrayRef<VarDecl *>> CurVars;
  enum {
    IVOLP_NotInVarOrVal,
    IVOLP_InVar,
    IVOLP_InVal
  } InVarOrValPattern = IVOLP_NotInVarOrVal;

  bool GreaterThanIsOperator = true;

  LocalContext *CurLocalContext = nullptr;

  DelayedParsingCallbacks *DelayedParseCB = nullptr;

  bool isDelayedParsingEnabled() const { return DelayedParseCB != nullptr; }

  void setDelayedParsingCallbacks(DelayedParsingCallbacks *DelayedParseCB) {
    this->DelayedParseCB = DelayedParseCB;
  }

  void setCodeCompletionCallbacks(CodeCompletionCallbacks *Callbacks) {
    CodeCompletion = Callbacks;
  }

  bool isCodeCompletionFirstPass() {
    return L->isCodeCompletion() && !CodeCompletion;
  }

  bool allowTopLevelCode() const {
    return SF.isScriptMode();
  }

  /// Returns true if the parser reached EOF with incomplete source input, due
  /// for example, a missing right brace.
  bool isInputIncomplete() const { return IsInputIncomplete; }

  void checkForInputIncomplete() {
    IsInputIncomplete = IsInputIncomplete || Tok.is(tok::eof);
  }

  /// \brief This is the current token being considered by the parser.
  Token Tok;

  /// \brief The location of the previous token.
  SourceLoc PreviousLoc;
  
  /// A RAII object for temporarily changing CurDeclContext.
  class ContextChange {
  protected:
    Parser &P;
    DeclContext *OldContext; // null signals that this has been popped
    LocalContext *OldLocal;

    ContextChange(const ContextChange &) = delete;
    ContextChange &operator=(const ContextChange &) = delete;

  public:
    ContextChange(Parser &P, DeclContext *DC,
                  LocalContext *newLocal = nullptr)
      : P(P), OldContext(P.CurDeclContext), OldLocal(P.CurLocalContext) {
      assert(DC && "pushing null context?");
      P.CurDeclContext = DC;
      P.CurLocalContext = newLocal;
    }

    /// Prematurely pop the DeclContext installed by the constructor.
    /// Makes the destructor a no-op.
    void pop() {
      assert(OldContext && "already popped context!");
      popImpl();
      OldContext = nullptr;
    }

    ~ContextChange() {
      if (OldContext) popImpl();
    }

  private:
    void popImpl() {
      P.CurDeclContext = OldContext;
      P.CurLocalContext = OldLocal;
    }
  };

  /// A RAII object for parsing a new local context.
  class ParseFunctionBody : public LocalContext {
  private:
    ContextChange CC;
  public:
    ParseFunctionBody(Parser &P, DeclContext *DC) : CC(P, DC, this) {
      assert(!isa<TopLevelCodeDecl>(DC) &&
             "top-level code should be parsed using TopLevelCodeContext!");
    }

    void pop() {
      CC.pop();
    }
  };

  /// A RAII object for temporarily changing whether an operator starting with
  /// '>' is an operator.
  class GreaterThanIsOperatorRAII {
    Parser &P;
    bool OldValue;

  public:
    GreaterThanIsOperatorRAII(Parser &p, bool newValue)
      : P(p), OldValue(p.GreaterThanIsOperator)
    {
      P.GreaterThanIsOperator = newValue;
    }

    ~GreaterThanIsOperatorRAII() {
      P.GreaterThanIsOperator = OldValue;
    }
  };

  /// Describes the kind of a lexical structure marker, indicating
  /// what kind of structural element we started parsing at a
  /// particular location.
  enum class StructureMarkerKind : unsigned char {
    /// The start of a declaration.
    Declaration,
    /// The start of a statement.
    Statement,
    /// An open parentheses.
    OpenParen,
    /// An open brace.
    OpenBrace,
    /// An open square bracket.
    OpenSquare
  };

  /// A structure marker, which identifies the location at which the
  /// parser saw an entity it is parsing.
  struct StructureMarker {
    /// The location at which the marker occurred.
    SourceLoc Loc;

    /// The kind of marker.
    StructureMarkerKind Kind;

    /// The leading whitespace for this marker, if it has already been
    /// computed.
    Optional<StringRef> LeadingWhitespace;
  };

  /// An RAII object that notes when we have seen a structure marker.
  class StructureMarkerRAII {
    Parser &P;

  public:
    StructureMarkerRAII(Parser &parser, SourceLoc loc,
                               StructureMarkerKind kind)
      : P(parser) {
      P.StructureMarkers.push_back({loc, kind, Nothing});
    }

    StructureMarkerRAII(Parser &parser, const Token &tok);

    ~StructureMarkerRAII() {
      P.StructureMarkers.pop_back();
    }
  };
  friend class StructureMarkerRAII;

  /// The stack of structure markers indicating the locations of
  /// structural elements actively being parsed, including the start
  /// of declarations, statements, and opening operators of various
  /// kinds.
  ///
  /// This vector is managed by \c StructureMarkerRAII objects.
  llvm::SmallVector<StructureMarker, 16> StructureMarkers;

public:
  Parser(unsigned BufferID, SourceFile &SF, SILParserState *SIL,
         PersistentParserState *PersistentState = nullptr);
  ~Parser();

  bool isInSILMode() const { return SIL != nullptr; }

  //===--------------------------------------------------------------------===//
  // Routines to save and restore parser state.

  class ParserPosition {
  public:
    ParserPosition() = default;
    ParserPosition &operator=(const ParserPosition &) = default;

  private:
    ParserPosition(Lexer::State LS, SourceLoc PreviousLoc):
        LS(LS), PreviousLoc(PreviousLoc)
    {}
    Lexer::State LS;
    SourceLoc PreviousLoc;
    friend class Parser;
  };

  ParserPosition getParserPosition() {
    return ParserPosition(L->getStateForBeginningOfToken(Tok),
                          PreviousLoc);
  }

  ParserPosition getParserPosition(const PersistentParserState::ParserPos &Pos){
    return ParserPosition(L->getStateForBeginningOfTokenLoc(Pos.Loc),
                          Pos.PrevLoc);
  }

  void restoreParserPosition(ParserPosition PP) {
    L->restoreState(PP.LS);

    // We might be at tok::eof now, so ensure that consumeToken() does not
    // assert about lexing past eof.
    Tok.setKind(tok::unknown);
    consumeToken();

    PreviousLoc = PP.PreviousLoc;
  }

  void backtrackToPosition(ParserPosition PP) {
    L->backtrackToState(PP.LS);

    // We might be at tok::eof now, so ensure that consumeToken() does not
    // assert about lexing past eof.
    Tok.setKind(tok::unknown);
    consumeToken();

    PreviousLoc = PP.PreviousLoc;
  }

  /// RAII object that, when it is destructed, restores the parser and lexer to
  /// their positions at the time the object was constructed.  Will not jump
  /// forward in the token stream.
  class BacktrackingScope {
    Parser &P;
    ParserPosition PP;

  public:
    BacktrackingScope(Parser &P) : P(P), PP(P.getParserPosition()) {}

    ~BacktrackingScope() {
      P.backtrackToPosition(PP);
    }
  };

  /// RAII object that, when it is destructed, restores the parser and lexer to
  /// their positions at the time the object was constructed.
  struct ParserPositionRAII {
  private:
    Parser &P;
    ParserPosition PP;

  public:
    ParserPositionRAII(Parser &P) : P(P), PP(P.getParserPosition()) {}

    ~ParserPositionRAII() {
      P.restoreParserPosition(PP);
    }
  };

  //===--------------------------------------------------------------------===//
  // Utilities

  /// \brief Return the next token that will be installed by \c consumeToken.
  const Token &peekToken();

  SourceLoc consumeToken();
  SourceLoc consumeToken(tok K) {
    assert(Tok.is(K) && "Consuming wrong token kind");
    return consumeToken();
  }

  /// \brief Retrieve the location just past the end of the previous
  /// source location.
  SourceLoc getEndOfPreviousLoc();

  /// \brief If the current token is the specified kind, consume it and
  /// return true.  Otherwise, return false without consuming it.
  bool consumeIf(tok K) {
    if (Tok.isNot(K)) return false;
    consumeToken(K);
    return true;
  }
  
  bool consumeIfNotAtStartOfLine(tok K) {
    if (Tok.isAtStartOfLine()) return false;
    return consumeIf(K);
  }
  
  /// \brief Read tokens until we get to one of the specified tokens, then
  /// return without consuming it.  Because we cannot guarantee that the token
  /// will ever occur, this skips to some likely good stopping point.
  void skipUntil(tok T1) {
    skipUntil(T1, tok::unknown);
  }
  void skipUntil(tok T1, tok T2);
  void skipUntilAnyOperator();

  /// \brief Skip until a token that starts with '>'.  Applies heuristics that
  /// are suitable when trying to find the end of a list of generic parameters,
  /// generic arguments, or list of types in a protocol composition.
  void skipUntilGreaterInTypeList();

  /// skipUntilDeclStmtRBrace - Skip to the next decl or '}'.
  void skipUntilDeclRBrace();

  void skipUntilDeclStmtRBrace(tok T1);

  /// \brief Skip to the next decl, statement or '}'.
  void skipUntilDeclStmtRBrace() {
    skipUntilDeclStmtRBrace(tok::unknown);
  }

  void skipUntilDeclRBrace(tok T1, tok T2 = tok::unknown);

  /// Skip a single token, but match parentheses, braces, and square brackets.
  ///
  /// Note: this does \em not match angle brackets ("<" and ">")! These are
  /// matched in the source when they refer to a generic type,
  /// but not when used as comparison operators.
  void skipSingle();

  /// Determine whether the given token is a continuation.
  bool isContinuation(const Token &tok) {
    // If this token isn't at the start of the line, it's a continuation.
    if (!tok.isAtStartOfLine())
      return true;

    return isContinuationSlow(tok);
  }

  /// Determine whether the given token is a continuation; slow path.
  bool isContinuationSlow(const Token &tok);

public:
  InFlightDiagnostic diagnose(SourceLoc Loc, Diagnostic Diag) {
    if (Diags.isDiagnosticPointsToFirstBadToken(Diag.getID()) &&
        Loc == Tok.getLoc() && Tok.isAtStartOfLine())
      Loc = Lexer::getLocForEndOfToken(SourceMgr, PreviousLoc);
    return Diags.diagnose(Loc, Diag);
  }

  InFlightDiagnostic diagnose(Token Tok, Diagnostic Diag) {
    return diagnose(Tok.getLoc(), Diag);
  }

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  InFlightDiagnostic diagnose(SourceLoc Loc, Diag<DiagArgTypes...> DiagID,
                              ArgTypes... Args) {
    return diagnose(Loc, Diagnostic(DiagID, Args...));
  }

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  InFlightDiagnostic diagnose(Token Tok, Diag<DiagArgTypes...> DiagID,
                              ArgTypes... Args) {
    return diagnose(Tok.getLoc(), Diagnostic(DiagID, Args...));
  }
  
  void diagnoseRedefinition(ValueDecl *Prev, ValueDecl *New);
     
  /// \brief Check whether the current token starts with '<'.
  bool startsWithLess(Token Tok) {
    return Tok.isAnyOperator() && Tok.getText()[0] == '<';
  }

  /// \brief Check whether the current token starts with '>'.
  bool startsWithGreater(Token Tok) {
    return Tok.isAnyOperator() && Tok.getText()[0] == '>';
  }

  /// \brief Consume the starting '<' of the current token, which may either
  /// be a complete '<' token or some kind of operator token starting with '<',
  /// e.g., '<>'.
  SourceLoc consumeStartingLess();

  /// \brief Consume the starting '>' of the current token, which may either
  /// be a complete '>' token or some kind of operator token starting with '>',
  /// e.g., '>>'.
  SourceLoc consumeStartingGreater();

  swift::ScopeInfo &getScopeInfo() { return State->getScopeInfo(); }

  /// \brief Add the given Decl to the current scope.
  void addToScope(ValueDecl *D) {
    getScopeInfo().addToScope(D, *this);
  }

  ValueDecl *lookupInScope(Identifier Name) {
    return getScopeInfo().lookupValueName(Name);
  }

  //===--------------------------------------------------------------------===//
  // Primitive Parsing

  /// \brief Consume an identifier (but not an operator) if present and return
  /// its name in \p Result.  Otherwise, emit an error.
  ///
  /// \returns false on success, true on error.
  bool parseIdentifier(Identifier &Result, SourceLoc &Loc, const Diagnostic &D);

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseIdentifier(Identifier &Result, Diag<DiagArgTypes...> ID,
                       ArgTypes... Args) {
    SourceLoc L;
    return parseIdentifier(Result, L, Diagnostic(ID, Args...));
  }

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseIdentifier(Identifier &Result, SourceLoc &L,
                       Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    return parseIdentifier(Result, L, Diagnostic(ID, Args...));
  }
  

  /// \brief Consume an identifier or operator if present and return its name
  /// in \p Result.  Otherwise, emit an error and return true.
  bool parseAnyIdentifier(Identifier &Result, SourceLoc &Loc,
                          const Diagnostic &D);

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseAnyIdentifier(Identifier &Result, Diag<DiagArgTypes...> ID,
                          ArgTypes... Args) {
    SourceLoc L;
    return parseAnyIdentifier(Result, L, Diagnostic(ID, Args...));
  }

  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseAnyIdentifier(Identifier &Result, SourceLoc &L,
                          Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    return parseAnyIdentifier(Result, L, Diagnostic(ID, Args...));
  }

  /// \brief The parser expects that \p K is next token in the input.  If so,
  /// it is consumed and false is returned.
  ///
  /// If the input is malformed, this emits the specified error diagnostic.
  bool parseToken(tok K, SourceLoc &TokLoc, const Diagnostic &D);
  
  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseToken(tok K, Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    SourceLoc L;
    return parseToken(K, L, Diagnostic(ID, Args...));
  }
  template<typename ...DiagArgTypes, typename ...ArgTypes>
  bool parseToken(tok K, SourceLoc &L,
                  Diag<DiagArgTypes...> ID, ArgTypes... Args) {
    return parseToken(K, L, Diagnostic(ID, Args...));
  }
  
  /// \brief Parse the specified expected token and return its location
  /// on success.  On failure, emit the specified error diagnostic, and
  /// a note at the specified note location.
  bool parseMatchingToken(tok K, SourceLoc &TokLoc, Diag<> ErrorDiag,
                          SourceLoc OtherLoc);

  /// \brief Parse the list of statements, expressions, or declarations.
  ParserStatus parseList(tok RightK, SourceLoc LeftLoc, SourceLoc &RightLoc,
                         tok SeparatorK, bool OptionalSep,
                         bool AllowSepAfterLast, Diag<> ErrorDiag,
                         std::function<ParserStatus()> callback);

  void consumeTopLevelDecl(ParserPosition BeginParserPosition,
                           TopLevelCodeDecl *TLCD);

  ParserStatus parseBraceItems(SmallVectorImpl<ASTNode> &Decls,
                               BraceItemListKind Kind =
                                   BraceItemListKind::Brace);
  ParserResult<BraceStmt> parseBraceItemList(Diag<> ID);

  void parseTopLevelCodeDeclDelayed();

  //===--------------------------------------------------------------------===//
  // Decl Parsing
  static bool isStartOfDecl(const Token &Tok, const Token &Tok2);

  bool parseTopLevel();

  /// Flags that control the parsing of declarations.
  enum ParseDeclFlags {
    PD_Default              = 0,
    PD_AllowTopLevel        = 1 << 1,
    PD_DisallowStoredInstanceVar = 1 << 2,
    PD_HasContainerType     = 1 << 3,
    PD_DisallowNominalTypes = 1 << 4,
    PD_DisallowInit         = 1 << 5,
    PD_DisallowTypeAliasDef = 1 << 6,
    PD_AllowDestructor      = 1 << 7,
    PD_AllowEnumElement     = 1 << 8,
    PD_InProtocol           = 1 << 9,
    PD_InClass              = 1 << 10,
    PD_InExtension          = 1 << 11,
  };

  /// Options that control the parsing of declarations.
  typedef OptionSet<ParseDeclFlags> ParseDeclOptions;

  /// Skips the current token if it is '}', and emits a diagnostic.
  ///
  /// \returns true if any tokens were skipped.
  bool skipExtraTopLevelRBraces();

  void consumeDecl(ParserPosition BeginParserPosition, ParseDeclOptions Flags,
                   bool IsTopLevel);
  ParserStatus parseDecl(SmallVectorImpl<Decl*> &Entries, ParseDeclOptions Flags);
  void parseDeclDelayed();

  ParserResult<TypeDecl> parseDeclTypeAlias(bool WantDefinition,
                                            bool isAssociatedType,
                                            DeclAttributes &Attributes);

  void setLocalDiscriminator(ValueDecl *D);

  bool parseDeclAttributeList(DeclAttributes &Attributes) {
    if (Tok.is(tok::at_sign))
      return parseDeclAttributeListPresent(Attributes);
    return false;
  }
  bool parseDeclAttributeListPresent(DeclAttributes &Attributes);
  bool parseDeclAttribute(DeclAttributes &Attributes);
  
  bool parseTypeAttributeList(TypeAttributes &Attributes) {
    if (Tok.is(tok::at_sign))
      return parseTypeAttributeListPresent(Attributes);
    return false;
  }
  bool parseTypeAttributeListPresent(TypeAttributes &Attributes);
  bool parseTypeAttribute(TypeAttributes &Attributes);
  
  
  ParserResult<ImportDecl> parseDeclImport(ParseDeclOptions Flags,
                                           DeclAttributes &Attributes);
  ParserStatus parseInheritance(SmallVectorImpl<TypeLoc> &Inherited);
  ParserResult<ExtensionDecl> parseDeclExtension(ParseDeclOptions Flags,
                                                 DeclAttributes &Attributes);
  ParserResult<EnumDecl> parseDeclEnum(ParseDeclOptions Flags,
                                       DeclAttributes &Attributes);
  ParserStatus parseDeclEnumCase(ParseDeclOptions Flags, DeclAttributes &Attributes,
                                 SmallVectorImpl<Decl *> &decls);
  bool parseNominalDeclMembers(SmallVectorImpl<Decl *> &memberDecls,
                               SourceLoc LBLoc, SourceLoc &RBLoc,
                               Diag<> ErrorDiag, ParseDeclOptions flags);
  ParserResult<StructDecl>
  parseDeclStruct(ParseDeclOptions Flags, DeclAttributes &Attributes);
  ParserResult<ClassDecl>
  parseDeclClass(ParseDeclOptions Flags, DeclAttributes &Attributes);
  ParserStatus parseDeclVar(ParseDeclOptions Flags, DeclAttributes &Attributes,
                            SmallVectorImpl<Decl *> &Decls,
                            SourceLoc StaticLoc,
                            StaticSpellingKind StaticSpelling);
  bool parseGetSet(ParseDeclOptions Flags,
                   Pattern *Indices, TypeLoc ElementTy,
                   FuncDecl *&Get, FuncDecl *&Set, FuncDecl *&WillSet,
                   FuncDecl *&DidSet, SourceLoc &LastValidLoc,
                   SourceLoc StaticLoc, SmallVectorImpl<Decl *> &Decls);
  VarDecl *parseDeclVarGetSet(Pattern &pattern, ParseDeclOptions Flags,
                              SourceLoc StaticLoc,
                              SmallVectorImpl<Decl *> &Decls);
  
  void consumeAbstractFunctionBody(AbstractFunctionDecl *AFD,
                                   const DeclAttributes &Attrs);
  ParserResult<FuncDecl> parseDeclFunc(SourceLoc StaticLoc,
                                       StaticSpellingKind StaticSpelling,
                                       SourceLoc MutatingLoc,
                                       ParseDeclOptions Flags,
                                       DeclAttributes &Attributes);
  bool parseAbstractFunctionBodyDelayed(AbstractFunctionDecl *AFD);
  ParserResult<ProtocolDecl> parseDeclProtocol(ParseDeclOptions Flags,
                                               DeclAttributes &Attributes);
  
  ParserStatus parseDeclSubscript(ParseDeclOptions Flags,
                                  DeclAttributes &Attributes,
                                  SmallVectorImpl<Decl *> &Decls);

  ParserResult<ConstructorDecl>
  parseDeclConstructor(ParseDeclOptions Flags, DeclAttributes &Attributes);
  ParserResult<DestructorDecl>
  parseDeclDestructor(ParseDeclOptions Flags, DeclAttributes &Attributes);

  void addPatternVariablesToScope(ArrayRef<Pattern *> Patterns);

  ParserResult<OperatorDecl> parseDeclOperator(bool AllowTopLevel,
                                               DeclAttributes &Attributes);
  ParserResult<OperatorDecl> parseDeclPrefixOperator(SourceLoc OperatorLoc,
                                                     SourceLoc PrefixLoc,
                                                     Identifier Name,
                                                     SourceLoc NameLoc);
  ParserResult<OperatorDecl> parseDeclPostfixOperator(SourceLoc OperatorLoc,
                                                      SourceLoc PostfixLoc,
                                                      Identifier Name,
                                                      SourceLoc NameLoc);
  ParserResult<OperatorDecl> parseDeclInfixOperator(SourceLoc OperatorLoc,
                                                    SourceLoc InfixLoc,
                                                    Identifier Name,
                                                    SourceLoc NameLoc);

  //===--------------------------------------------------------------------===//
  // SIL Parsing.

  bool parseDeclSIL();
  bool parseDeclSILStage();
  bool parseSILVTable();
  bool parseSILGlobal();
  bool parseSILWitnessTable();

  //===--------------------------------------------------------------------===//
  // Type Parsing
  
  ParserResult<TypeRepr> parseType();
  ParserResult<TypeRepr> parseType(Diag<> MessageID);

  /// \brief Parse any type, but diagnose all types except type-identifier.
  ///
  /// In some places the grammar allows type-identifier, but when it is not
  /// ambiguous, we want to parse any type for recovery purposes.
  ///
  /// \param MessageID a generic diagnostic for a syntax error in the type
  /// \param NonIdentifierTypeMessageID a diagnostic for a non-identifier type
  ///
  /// \returns null, IdentTypeRepr or ErrorTypeRepr.
  ParserResult<TypeRepr>
  parseTypeIdentifierWithRecovery(Diag<> MessageID,
                                  Diag<TypeLoc> NonIdentifierTypeMessageID);

  ParserResult<TypeRepr> parseTypeAnnotation();
  ParserResult<TypeRepr> parseTypeAnnotation(Diag<> ID);
  ParserResult<TypeRepr> parseTypeSimple();
  ParserResult<TypeRepr> parseTypeSimple(Diag<> MessageID);
  bool parseGenericArguments(SmallVectorImpl<TypeRepr*> &Args,
                             SourceLoc &LAngleLoc,
                             SourceLoc &RAngleLoc);
  ParserResult<IdentTypeRepr> parseTypeIdentifier();

  ParserResult<ProtocolCompositionTypeRepr> parseTypeComposition();
  ParserResult<TupleTypeRepr> parseTypeTupleBody();
  ParserResult<ArrayTypeRepr> parseTypeArray(TypeRepr *Base);
  ParserResult<OptionalTypeRepr> parseTypeOptional(TypeRepr *Base);

  TypeRepr *applyAttributeToType(TypeRepr *Ty, const TypeAttributes &Attr);

  //===--------------------------------------------------------------------===//
  // Pattern Parsing

  /// A structure for collecting information about the default
  /// arguments of a context.
  struct DefaultArgumentInfo {
    llvm::SmallVector<DefaultArgumentInitializer *, 4> ParsedContexts;
    unsigned NextIndex = 0;

    /// Claim the next argument index.  It's important to do this for
    /// all the arguments, not just those that have default arguments.
    unsigned claimNextIndex() { return NextIndex++; }

    /// Set the parsed context for all the initializers to the given
    /// function.
    void setFunctionContext(DeclContext *DC);
  };

  ParserStatus parseFunctionArguments(Identifier functionName,
                                      SmallVectorImpl<Pattern*> &ArgPatterns,
                                      SmallVectorImpl<Pattern*> &BodyPatterns,
                                      DefaultArgumentInfo &defaultArgs,
                                      bool &HasSelectorStyleSignature);
  ParserStatus parseFunctionSignature(Identifier functionName,
                                      SmallVectorImpl<Pattern *> &argPatterns,
                                      SmallVectorImpl<Pattern *> &bodyPatterns,
                                      DefaultArgumentInfo &defaultArgs,
                                      TypeRepr *&retType,
                                      bool &HasSelectorStyleSignature);
  ParserStatus parseConstructorArguments(Pattern *&ArgPattern,
                                         Pattern *&BodyPattern,
                                         DefaultArgumentInfo &defaultArgs,
                                         bool &HasSelectorStyleSignature);

  ParserResult<Pattern> parsePattern(bool isVal);

  /// \brief Determine whether the parser is in a state that can start a
  /// binding name, identifier or the special discard-value binding '_'.
  bool isAtStartOfBindingName();

  /// \brief Parse a tuple pattern element.
  ///
  /// \code
  ///   pattern-tuple-element:
  ///     pattern ('=' expr)?
  /// \endcode
  ///
  /// \param args If non-null, collect default arguments here.
  ///
  /// \returns The tuple pattern element, if successful.
  std::pair<ParserStatus, Optional<TuplePatternElt>>
  parsePatternTupleElement(bool isVal, bool isArgumentList,
                           Pattern *ImplicitName, DefaultArgumentInfo *args);
  ParserResult<Pattern> parsePatternTuple(bool isVal, bool isArgumentList,
                                          DefaultArgumentInfo *args);
  ParserResult<Pattern> parsePatternTupleAfterLP(bool isVal,bool isArgumentList,
                                                 SourceLoc LPLoc,
                                                 DefaultArgumentInfo *args);
  ParserResult<Pattern> parsePatternAtom(bool isVal);
  ParserResult<Pattern> parsePatternIdentifier(bool isVal);
  ParserResult<Pattern> parsePatternVarOrVal();
  
  Pattern *createBindingFromPattern(SourceLoc loc, Identifier name, bool isVal);
  
  //===--------------------------------------------------------------------===//
  // Pattern Parsing

  // TODO: Depending on how robust our distributive-var design works out, we
  // may be able to integrate exhaustive pattern parsing in var/func decls
  // with matching pattern parsing when it matures.
  
  ParserResult<Pattern> parseMatchingPattern();
  ParserResult<Pattern> parseMatchingPatternVarOrVal();
  ParserResult<Pattern> parseMatchingPatternIs();
  
  /// \brief Determine whether this token can only start a matching pattern
  /// production and not an expression.
  bool isOnlyStartOfMatchingPattern();

  //===--------------------------------------------------------------------===//
  // Speculative type list parsing
  //===--------------------------------------------------------------------===//
  
  /// Returns true if we can parse a generic argument list at the current
  /// location in expression context. This parses types without generating
  /// AST nodes from the '<' at the current location up to a matching '>'. If
  /// the type list parse succeeds, and the closing '>' is followed by one
  /// of the following tokens:
  ///   lparen_following rparen lsquare_following rsquare lbrace rbrace
  ///   period_following comma semicolon
  /// then this function returns true, and the expression will parse as a
  /// generic parameter list. If the parse fails, or the closing '>' is not
  /// followed by one of the above tokens, then this function returns false,
  /// and the expression will parse with the '<' as an operator.
  bool canParseAsGenericArgumentList();
  bool canParseAttributes();

  bool canParseType();
  bool canParseTypeIdentifier();
  bool canParseTypeComposition();
  bool canParseTypeTupleBody();
  bool canParseTypeArray();
  bool canParseGenericArguments();

  //===--------------------------------------------------------------------===//
  // Expression Parsing
  ParserResult<Expr> parseExpr(Diag<> ID) {
    return parseExprImpl(ID, /*isExprBasic=*/false);
  }
  ParserResult<Expr> parseExprBasic(Diag<> ID) {
    return parseExprImpl(ID, /*isExprBasic=*/true);
  }
  ParserResult<Expr> parseExprImpl(Diag<> ID, bool isExprBasic = false);
  ParserResult<Expr> parseExprIs();
  ParserResult<Expr> parseExprAs();
  ParserResult<Expr> parseExprSequence(Diag<> ID, bool isExprBasic);
  ParserResult<Expr> parseExprPostfix(Diag<> ID, bool isExprBasic);
  ParserResult<Expr> parseExprUnary(Diag<> ID, bool isExprBasic);
  ParserResult<Expr> parseExprNew();
  ParserResult<Expr> parseExprSuper();
  Expr *parseExprStringLiteral();
  
  Expr *parseExprIdentifier();

  /// \brief Parse a closure expression after the opening brace.
  ///
  /// \verbatim
  ///   expr-closure:
  ///     '{' closure-signature? brace-item-list* '}'
  ///
  ///   closure-signature:
  ///     '|' closure-signature-arguments? '|' closure-signature-result?
  ///
  ///   closure-signature-arguments:
  ///     pattern-tuple-element (',' pattern-tuple-element)*
  ///
  ///   closure-signature-result:
  ///     '->' type
  /// \endverbatim
  ParserResult<Expr> parseExprClosure();

  /// \brief Parse the closure signature, if present.
  ///
  /// \verbatim
  ///   closure-signature:
  ///     pattern-tuple func-signature-result? 'in'
  ///     identifier (',' identifier)* func-signature-result? 'in'
  /// \endverbatim
  ///
  /// \param params The parsed parameter list, or null if none was provided.
  /// \param arrowLoc The location of the arrow, if present.
  /// \param explicitResultType The explicit result type, if specified.
  /// \param inLoc The location of the 'in' keyword, if present.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool parseClosureSignatureIfPresent(Pattern *&params,
                                      SourceLoc &arrowLoc,
                                      TypeRepr *&explicitResultType,
                                      SourceLoc &inLoc);

  Expr *parseExprAnonClosureArg();
  ParserResult<Expr> parseExprList(tok LeftTok, tok RightTok);
  bool hasExprCallSuffix(bool isExprBasic);
  ParserResult<Expr> parseExprCallSuffix(ParserResult<Expr> fn,
                                         Identifier firstSelectorPiece
                                           = Identifier());
  ParserResult<Expr> parseExprCollection();
  ParserResult<Expr> parseExprArray(SourceLoc LSquareLoc, Expr *FirstExpr);
  ParserResult<Expr> parseExprDictionary(SourceLoc LSquareLoc, Expr *FirstKey);

  UnresolvedDeclRefExpr *parseExprOperator();
  Expr *actOnIdentifierExpr(Identifier Text, SourceLoc Loc);

  //===--------------------------------------------------------------------===//
  // Statement Parsing

  static bool isStartOfStmt(const Token &Tok);
  ParserResult<Stmt> parseStmt();
  ParserStatus parseExprOrStmt(ASTNode &Result);
  ParserResult<Stmt> parseStmtReturn();
  ParserStatus parseStmtCondition(StmtCondition &Result, Diag<> ID);
  ParserResult<Stmt> parseStmtIf();
  ParserResult<Stmt> parseStmtWhile();
  ParserResult<Stmt> parseStmtDoWhile();
  ParserResult<Stmt> parseStmtFor();
  ParserResult<Stmt> parseStmtForCStyle(SourceLoc ForLoc);
  ParserResult<Stmt> parseStmtForEach(SourceLoc ForLoc);
  ParserResult<Stmt> parseStmtSwitch();
  ParserResult<CaseStmt> parseStmtCase();
  ParserStatus parseStmtCaseLabels(SmallVectorImpl<CaseLabel*> &labels,
                                   SmallVectorImpl<Decl *> &boundDecls);

  //===--------------------------------------------------------------------===//
  // Generics Parsing

  GenericParamList *parseGenericParameters();
  GenericParamList *parseGenericParameters(SourceLoc LAngleLoc);
  GenericParamList *maybeParseGenericParams();
  bool parseGenericWhereClause(SourceLoc &WhereLoc,
                               SmallVectorImpl<RequirementRepr> &Requirements);
};

} // end namespace swift

#endif
