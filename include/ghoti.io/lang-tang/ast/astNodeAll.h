/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2024-2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-tang.
 *
 * Ghoti.io Lang-tang is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-tang is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 * @stability free
 *
 * Include all AST node headers.
 */

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEALL_H
#define GHOTI_IO_GLTANG_AST_ASTNODEALL_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/ast/astNode.h>
#include <ghoti.io/lang-tang/ast/astNodeArray.h>
#include <ghoti.io/lang-tang/ast/astNodeAssign.h>
#include <ghoti.io/lang-tang/ast/astNodeBinary.h>
#include <ghoti.io/lang-tang/ast/astNodeBlock.h>
#include <ghoti.io/lang-tang/ast/astNodeBoolean.h>
#include <ghoti.io/lang-tang/ast/astNodeBreak.h>
#include <ghoti.io/lang-tang/ast/astNodeCast.h>
#include <ghoti.io/lang-tang/ast/astNodeContinue.h>
#include <ghoti.io/lang-tang/ast/astNodeDoWhile.h>
#include <ghoti.io/lang-tang/ast/astNodeFloat.h>
#include <ghoti.io/lang-tang/ast/astNodeFor.h>
#include <ghoti.io/lang-tang/ast/astNodeFunction.h>
#include <ghoti.io/lang-tang/ast/astNodeFunctionCall.h>
#include <ghoti.io/lang-tang/ast/astNodeGlobal.h>
#include <ghoti.io/lang-tang/ast/astNodeIdentifier.h>
#include <ghoti.io/lang-tang/ast/astNodeIfElse.h>
#include <ghoti.io/lang-tang/ast/astNodeIndex.h>
#include <ghoti.io/lang-tang/ast/astNodeInteger.h>
#include <ghoti.io/lang-tang/ast/astNodeLibrary.h>
#include <ghoti.io/lang-tang/ast/astNodeMap.h>
#include <ghoti.io/lang-tang/ast/astNodeParseError.h>
#include <ghoti.io/lang-tang/ast/astNodePeriod.h>
#include <ghoti.io/lang-tang/ast/astNodePrint.h>
#include <ghoti.io/lang-tang/ast/astNodeRangedFor.h>
#include <ghoti.io/lang-tang/ast/astNodeReturn.h>
#include <ghoti.io/lang-tang/ast/astNodeSlice.h>
#include <ghoti.io/lang-tang/ast/astNodeString.h>
#include <ghoti.io/lang-tang/ast/astNodeTernary.h>
#include <ghoti.io/lang-tang/ast/astNodeUnary.h>
#include <ghoti.io/lang-tang/ast/astNodeUse.h>
#include <ghoti.io/lang-tang/ast/astNodeWhile.h>

#endif // GHOTI_IO_GLTANG_AST_ASTNODEALL_H
