//===--- CodeLens.cpp --------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CodeLens.h"
#include "AST.h"
#include "FindSymbols.h"
#include "XRefs.h"
#include "support/Logger.h"

namespace clang {
namespace clangd {
std::optional<Location> declToLocation(const Decl *D) {
  ASTContext &Ctx = D->getASTContext();
  auto &SM = Ctx.getSourceManager();
  auto &FM = SM.getFileManager();
  SourceLocation NameLoc = nameLocation(*D, Ctx.getSourceManager());
  auto FE = SM.getFileEntryRefForID(SM.getFileID(NameLoc));
  auto TU = SM.getFileEntryRefForID(SM.getMainFileID());
  if (!FE || !TU) {
    return std::nullopt;
  }
  auto FilePath = getCanonicalPath(*FE, FM);
  auto TUPath = getCanonicalPath(*TU, FM);
  if (!FilePath || !TUPath)
    return std::nullopt; // Not useful without a uri.

  Position NameBegin = sourceLocToPosition(SM, NameLoc);
  Position NameEnd = sourceLocToPosition(
      SM, Lexer::getLocForEndOfToken(NameLoc, 0, SM, Ctx.getLangOpts()));
  return Location{URIForFile::canonicalize(*FilePath, *TUPath),
                  {NameBegin, NameEnd}};
}

std::vector<Location> lookupIndex(const SymbolIndex *Index, uint32_t Limit,
                                  PathRef Path, Decl *D, RelationKind R) {
  std::vector<Location> Results;
  if (!Index)
    return Results;
  auto ID = getSymbolID(D);
  if (!ID)
    return Results;
  RelationsRequest Req;
  Req.Subjects.insert(ID);
  Req.Limit = Limit;
  Req.Predicate = R;
  Index->relations(Req, [&](const SymbolID &Subject, const Symbol &Object) {
    if (auto Loc = indexToLSPLocation(Object.CanonicalDeclaration, Path)) {
      Results.emplace_back(std::move(*Loc));
    }
  });
  return Results;
}

void visitDecl(ParsedAST &AST, const SymbolIndex *Index, uint32_t Limit,
               PathRef Path, Decl *D, std::set<SourceLocation> &Visited,
               std::vector<CodeLens> &Results) {
  auto &SM = AST.getSourceManager();
  // Skip symbols which do not originate from the main file.
  if (!isInsideMainFile(D->getLocation(), SM))
    return;
  if (D->isImplicit() || !isa<NamedDecl>(D) || D->getLocation().isMacroID())
    return;

  if (auto *Templ = llvm::dyn_cast<TemplateDecl>(D)) {
    if (auto *TD = Templ->getTemplatedDecl())
      D = TD;
  };

  if (Visited.find(D->getLocation()) != Visited.end()) {
    return;
  }

  Visited.emplace(D->getLocation());

  bool VisitChildren = true;
  if (auto *Func = llvm::dyn_cast<FunctionDecl>(D)) {
    if (auto *Info = Func->getTemplateSpecializationInfo()) {
      if (!Info->isExplicitInstantiationOrSpecialization())
        return;
    }
    VisitChildren = false;
  }
  // Handle template instantiations. We have three cases to consider:
  //   - explicit instantiations, e.g. 'template class std::vector<int>;'
  //     Visit the decl itself (it's present in the code), but not the
  //     children.
  //   - implicit instantiations, i.e. not written by the user.
  //     Do not visit at all, they are not present in the code.
  //   - explicit specialization, e.g. 'template <> class vector<bool> {};'
  //     Visit both the decl and its children, both are written in the code.
  if (auto *TemplSpec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(D)) {
    if (!TemplSpec->isExplicitInstantiationOrSpecialization()) {
      return;
    }
    VisitChildren = TemplSpec->isExplicitSpecialization();
  }

  auto Location = D->getLocation();
  Range Range = {
      sourceLocToPosition(SM, Location),
      sourceLocToPosition(
          SM, Lexer::getLocForEndOfToken(Location, 0, SM, AST.getLangOpts()))};

  // Namspaces are not indexed, so it's meaningless to provide codelens.
  if (!isa<NamespaceDecl, NamespaceAliasDecl>(D)) {
    CodeLensResolveData Data;
    Data.uri = std::string(Path);
    Results.emplace_back(CodeLens{Range, std::nullopt, Data});
  }

  // handle inheritance codelens directly
  CodeLensArgument Sub, Super;
  if (auto *CXXRD = dyn_cast<CXXRecordDecl>(D)) {
    if (!CXXRD->isEffectivelyFinal()) {
      Sub.locations = lookupIndex(Index, Limit, Path, D, RelationKind::BaseOf);
    }
    for (const auto *P : typeParents(CXXRD)) {
      if (auto Loc = declToLocation(P->getCanonicalDecl()))
        Super.locations.emplace_back(*Loc);
    }
  } else if (auto *CXXMD = dyn_cast<CXXMethodDecl>(D)) {
    if (CXXMD->isVirtual()) {
      Sub.locations =
          lookupIndex(Index, Limit, Path, D, RelationKind::OverriddenBy);
    }
    for (const auto *P : CXXMD->overridden_methods()) {
      if (auto Loc = declToLocation(P->getCanonicalDecl()))
        Super.locations.emplace_back(*Loc);
    }
  }

  if (auto Count = Super.locations.size()) {
    Super.position = Range.start;
    Super.uri = std::string(Path);
    Command Cmd;
    Cmd.command = std::string(CodeAction::SHOW_REFERENCES);
    Cmd.title = std::to_string(Count) + " base";
    Cmd.argument = std::move(Super);
    Results.emplace_back(CodeLens{Range, std::move(Cmd), std::nullopt});
  }

  if (auto Count = Sub.locations.size()) {
    Sub.position = Range.start;
    Sub.uri = std::string(Path);
    Command Cmd;
    Cmd.command = std::string(CodeAction::SHOW_REFERENCES);
    Cmd.title = std::to_string(Count) + " derived";
    Cmd.argument = std::move(Sub);
    Results.emplace_back(CodeLens{Range, std::move(Cmd), std::nullopt});
  }

  if (!VisitChildren) {
    return;
  }

  if (auto *Scope = dyn_cast<DeclContext>(D)) {
    for (auto *C : Scope->decls()) {
      visitDecl(AST, Index, Limit, Path, C, Visited, Results);
    }
  }
}

llvm::Expected<std::vector<CodeLens>>
getDocumentCodeLens(ParsedAST &AST, const SymbolIndex *Index, uint32_t Limit,
                    PathRef Path) {
  std::vector<CodeLens> Results;
  std::set<SourceLocation> Visited;
  Limit = Limit ? Limit : std::numeric_limits<uint32_t>::max();
  for (auto &TopLevel : AST.getLocalTopLevelDecls())
    visitDecl(AST, Index, Limit, Path, TopLevel, Visited, Results);
  return Results;
}

llvm::Expected<CodeLens> resolveCodeLens(ParsedAST &AST, const CodeLens &Params,
                                         uint32_t Limit,
                                         const SymbolIndex *Index,
                                         PathRef Path) {
  Command Cmd;
  Cmd.command = std::string(CodeAction::SHOW_REFERENCES);
  Position Pos = Params.range.start;
  if (Params.data) {
    CodeLensArgument Arg;
    Arg.uri = std::string(Path);
    Arg.position = Pos;
    auto Refs = findReferences(AST, Pos, Limit, Index).References;
    Arg.locations.reserve(Refs.size());
    for (auto &Ref : Refs) {
      Arg.locations.emplace_back(std::move(Ref.Loc));
    }
    Cmd.title = std::to_string(Refs.size()) + " references";
    Cmd.argument = std::move(Arg);
    return CodeLens{Params.range, std::move(Cmd), std::nullopt};
  }
  return error("failed to resolve codelens");
}
} // namespace clangd
} // namespace clang
