// Copyright 2021 Intel Corporation
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

#pragma once

#include <functional>

#include "plier/dialect.hpp"

#include <mlir/IR/PatternMatch.h>

namespace mlir
{
class TypeConverter;
}

namespace plier
{
struct CallOpLowering : public mlir::OpRewritePattern<plier::PyCallOp>
{
    using resolver_t = std::function<mlir::LogicalResult(plier::PyCallOp, llvm::StringRef, llvm::ArrayRef<mlir::Value>, llvm::ArrayRef<std::pair<llvm::StringRef, mlir::Value>> , mlir::PatternRewriter&)>;

    CallOpLowering(mlir::TypeConverter &typeConverter,
                   mlir::MLIRContext *context,
                   resolver_t resolver);

    mlir::LogicalResult matchAndRewrite(
        plier::PyCallOp op, mlir::PatternRewriter &rewriter) const override;

private:
    resolver_t resolver;
};
}