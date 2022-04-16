//===--- CodeLens.h ----------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_CODELENS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_CODELENS_H

#include "ParsedAST.h"
#include "Protocol.h"

namespace clang {
namespace clangd {
llvm::Expected<std::vector<CodeLens>>
getDocumentCodeLens(ParsedAST &AST, const SymbolIndex *Index, uint32_t Limit,
                    PathRef Path);

llvm::Expected<CodeLens> resolveCodeLens(ParsedAST &AST, const CodeLens &Params,
                                         uint32_t Limit,
                                         const SymbolIndex *Index,
                                         PathRef Path);
} // namespace clangd
} // namespace clang
#endif
