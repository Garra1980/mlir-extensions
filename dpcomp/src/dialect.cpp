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

#include "plier/dialect.hpp"

#include <mlir/IR/DialectImplementation.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/Transforms/InliningUtils.h>

#include <mlir/Dialect/StandardOps/IR/Ops.h>
#include <mlir/Dialect/MemRef/IR/MemRef.h>

#include <llvm/ADT/TypeSwitch.h>

#include "plier/transforms/const_utils.hpp"

namespace MemoryEffects = ::mlir::MemoryEffects;

namespace
{
struct PLierInlinerInterface : public mlir::DialectInlinerInterface
{
    using mlir::DialectInlinerInterface::DialectInlinerInterface;
    bool isLegalToInline(mlir::Region *, mlir::Region *, bool,
                         mlir::BlockAndValueMapping &) const final override
    {
        return true;
    }
    bool isLegalToInline(mlir::Operation *op, mlir::Region *, bool,
                         mlir::BlockAndValueMapping &) const final override
    {
        return !mlir::isa<plier::ArgOp>(op);
    }
};
}

namespace plier
{

llvm::StringRef attributes::getFastmathName()
{
    return "#plier.fastmath";
}

llvm::StringRef attributes::getJumpMarkersName()
{
    return "#plier.pipeline_jump_markers";
}

llvm::StringRef attributes::getParallelName()
{
    return "#plier.parallel";
}

llvm::StringRef attributes::getMaxConcurrencyName()
{
    return "#plier.max_concurrency";
}

llvm::StringRef attributes::getForceInlineName()
{
    return "#plier.force_inline";
}


namespace detail
{
struct PyTypeStorage : public mlir::TypeStorage
{
    using KeyTy = mlir::StringRef;

    PyTypeStorage(mlir::StringRef name): name(name) {}

    bool operator==(const KeyTy& key) const
    {
        return key == name;
    }

    static PyTypeStorage* construct(mlir::TypeStorageAllocator& allocator,
                                    const KeyTy& key)
    {
        return new(allocator.allocate<PyTypeStorage>())
            PyTypeStorage(allocator.copyInto(key));
    }

    mlir::StringRef name;
};
}

void PlierDialect::initialize()
{
    addOperations<
#define GET_OP_LIST
#include "plier/PlierOps.cpp.inc"
        >();
    addTypes<plier::PyType>();
    addInterfaces<PLierInlinerInterface>();
}

mlir::Type PlierDialect::parseType(mlir::DialectAsmParser &parser) const {
    parser.emitError(parser.getNameLoc(), "unknown type");
    return mlir::Type();
}

void PlierDialect::printType(mlir::Type type, mlir::DialectAsmPrinter &os) const {
    llvm::TypeSwitch<mlir::Type>(type)
        .Case<plier::PyType>([&](auto t){ os << "PyType<" << t.getName() << ">"; })
        .Default([](auto){ llvm_unreachable("unexpected type"); });
}

PyType PyType::get(mlir::MLIRContext* context, llvm::StringRef name)
{
    assert(!name.empty());
    return Base::get(context, name);
}

PyType PyType::getUndefined(mlir::MLIRContext* context)
{
    return Base::get(context, "");
}

PyType PyType::getNone(mlir::MLIRContext* context)
{
    return Base::get(context, "none");
}

llvm::StringRef PyType::getName() const
{
    return getImpl()->name;
}

void ArgOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                  unsigned index, mlir::StringRef name) {
    ArgOp::build(builder, state, PyType::getUndefined(state.getContext()),
                 index, name);
}

mlir::OpFoldResult ArgOp::fold(llvm::ArrayRef<mlir::Attribute> /*operands*/)
{
    auto func = getOperation()->getParentOfType<mlir::FuncOp>();
    if (func)
    {
        auto ind = index();
        if (ind < func.getNumArguments() &&
            func.getArgument(ind).getType() == getType())
        {
            return func.getArgument(ind);
        }
    }
    return nullptr;
}

void ConstOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,

                   mlir::Attribute val) {
    ConstOp::build(builder, state, PyType::getUndefined(state.getContext()),
                   val);
}

void GlobalOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                     mlir::StringRef name) {
    GlobalOp::build(builder, state, PyType::getUndefined(state.getContext()),
                    name);
}

void BinOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                  mlir::Value lhs, mlir::Value rhs, mlir::StringRef op) {
    BinOp::build(builder, state, PyType::getUndefined(state.getContext()), lhs,
                 rhs, op);
}

void UnaryOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                  mlir::Value value, mlir::StringRef op) {
    UnaryOp::build(builder, state, PyType::getUndefined(state.getContext()),
                   value, op);
}

mlir::OpFoldResult CastOp::fold(llvm::ArrayRef<mlir::Attribute> /*operands*/)
{
    auto op_type = getOperand().getType();
    auto ret_type = getType();
    if (op_type == ret_type && op_type != PyType::getUndefined(getContext()))
    {
        return getOperand();
    }
    return nullptr;
}

void PyCallOp::build(mlir::OpBuilder &builder, mlir::OperationState &state, mlir::Value func,
                     llvm::StringRef func_name, mlir::ValueRange args,
                     mlir::ArrayRef<std::pair<std::string, mlir::Value>> kwargs) {
    auto ctx = builder.getContext();
    mlir::SmallVector<mlir::Value, 16> all_args;
    all_args.reserve(args.size() + kwargs.size());
    std::copy(args.begin(), args.end(), std::back_inserter(all_args));
    auto kw_start = static_cast<uint32_t>(all_args.size());
    mlir::SmallVector<mlir::Attribute> kw_names;
    kw_names.reserve(kwargs.size());
    for (auto& a : kwargs)
    {
        kw_names.push_back(mlir::StringAttr::get(ctx, a.first));
        all_args.push_back(a.second);
    }
    PyCallOp::build(builder, state, PyType::getUndefined(state.getContext()),
        func, all_args, func_name, kw_start, mlir::ArrayAttr::get(ctx, kw_names));
}

void BuildTupleOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                         ::mlir::ValueRange args)
{
    BuildTupleOp::build(builder, state,
                        PyType::getUndefined(state.getContext()), args);
}

//mlir::LogicalResult BuildTupleOp::fold(
//    llvm::ArrayRef<mlir::Attribute> /*operands*/,
//    llvm::SmallVectorImpl<mlir::OpFoldResult> &results)
//{
//    auto res_types = getResultTypes();
//    auto args = getOperands();
//    if (res_types.size() == args.size())
//    {
//        std::copy(args.begin(), args.end(), std::back_inserter(results));
//        return mlir::success();
//    }
//    return mlir::failure();
//}

mlir::Value fold_build_tuple_getitem(mlir::Value val, mlir::Type type, llvm::ArrayRef<mlir::Attribute> operands)
{
    auto build_tuple = val.getDefiningOp<plier::BuildTupleOp>();
    if (build_tuple)
    {
        if (auto val = operands[1].dyn_cast_or_null<mlir::IntegerAttr>())
        {
            auto index = val.getInt();
            if (index >= 0 && index < build_tuple.getNumOperands())
            {
                auto op = build_tuple.getOperand(static_cast<unsigned>(index));
                if (op.getType() == type)
                {
                    return op;
                }
            }
        }
    }
    return {};
}

void GetItemOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                            ::mlir::Value value, ::mlir::Value index)
{
    GetItemOp::build(builder, state,
                     PyType::getUndefined(state.getContext()), value, index);
}

mlir::OpFoldResult GetItemOp::fold(llvm::ArrayRef<mlir::Attribute> operands)
{
    if (auto val = fold_build_tuple_getitem(value(), getType(), operands))
    {
        return val;
    }
    return nullptr;
}

void StaticGetItemOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                            ::mlir::Value value, ::mlir::Value index_var,
                            unsigned int index)
{
    StaticGetItemOp::build(builder, state,
                           PyType::getUndefined(state.getContext()),
                           value, index_var, index);
}

mlir::OpFoldResult StaticGetItemOp::fold(llvm::ArrayRef<mlir::Attribute> operands)
{
    if (auto val = fold_build_tuple_getitem(value(), getType(), operands))
    {
        return val;
    }
    return nullptr;
}

void GetiterOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                            ::mlir::Value value)
{
    GetiterOp::build(builder, state, PyType::getUndefined(state.getContext()),
                     value);
}

void IternextOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                            ::mlir::Value value)
{
    IternextOp::build(builder, state, PyType::getUndefined(state.getContext()),
                      value);
}

void PairfirstOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                            ::mlir::Value value)
{
    PairfirstOp::build(builder, state, PyType::getUndefined(state.getContext()),
                       value);
}

//mlir::OpFoldResult PairfirstOp::fold(llvm::ArrayRef<mlir::Attribute> /*operands*/)
//{
//    if (getNumOperands() == 2)
//    {
//        return getOperand(0);
//    }
//    return nullptr;
//}

void PairsecondOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                         ::mlir::Value value)
{
    PairsecondOp::build(builder, state,
                        PyType::getUndefined(state.getContext()), value);
}

//mlir::OpFoldResult PairsecondOp::fold(llvm::ArrayRef<mlir::Attribute> /*operands*/)
//{
//    if (getNumOperands() == 2)
//    {
//        return getOperand(1);
//    }
//    return nullptr;
//}

void GetattrOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                      mlir::Value value, mlir::StringRef name) {
    GetattrOp::build(builder, state, PyType::getUndefined(state.getContext()),
                     value, name);
}

namespace
{
struct GetattrGlobalRewrite : public mlir::OpRewritePattern<GetattrOp>
{
    using mlir::OpRewritePattern<GetattrOp>::OpRewritePattern;

    mlir::LogicalResult matchAndRewrite(
        GetattrOp op, mlir::PatternRewriter &rewriter) const override
    {
        auto prev_op = mlir::dyn_cast_or_null<plier::GlobalOp>(op.getOperand().getDefiningOp());
        if (prev_op)
        {
            auto new_name = llvm::Twine(prev_op.name() + "." + op.name()).str();
            auto new_op = rewriter.create<plier::GlobalOp>(op.getLoc(), op.getType(), new_name);
            rewriter.replaceOp(op, new_op.getResult());
            return mlir::success();
        }
        return mlir::failure();
    }
};
}

void GetattrOp::getCanonicalizationPatterns(
    ::mlir::OwningRewritePatternList &results, ::mlir::MLIRContext *context)
{
    results.insert<GetattrGlobalRewrite>(context);
}

void EnforceShapeOp::build(mlir::OpBuilder &builder, mlir::OperationState &state,
                            mlir::Value value, mlir::ValueRange shape) {
    EnforceShapeOp::build(builder, state, value.getType(), value, shape);
}

mlir::OpFoldResult EnforceShapeOp::fold(llvm::ArrayRef<mlir::Attribute> operands) {
    operands = operands.drop_front();
    auto num_dims = static_cast<unsigned>(operands.size());
    auto src_type = getType().cast<mlir::ShapedType>();
    llvm::SmallVector<int64_t> final_shape(num_dims, -1);
    if (src_type.hasRank())
    {
        auto shape = src_type.getShape();
        if (shape.size() != num_dims)
        {
            return nullptr;
        }
        final_shape.assign(shape.begin(), shape.end());
    }
    bool changed = false;
    for (unsigned i = 0; i < num_dims; ++i)
    {
        if (auto attr = operands[i].dyn_cast_or_null<mlir::IntegerAttr>())
        {
            auto val = attr.getInt();
            if (val != -1)
            {
                if (final_shape[i] != -1)
                {
                    if (final_shape[i] != val)
                    {
                        return nullptr;
                    }
                }
                else
                {
                    changed = true;
                    final_shape[i] = val;
                }
            }
        }
    }

    if (changed)
    {
        auto final_type = mlir::RankedTensorType::get(final_shape, src_type.getElementType());
        result().setType(final_type);
        return result();
    }
    return nullptr;
}

namespace
{
struct EnforceShapeDim : public mlir::OpRewritePattern<mlir::memref::DimOp>
{
    using mlir::OpRewritePattern<mlir::memref::DimOp>::OpRewritePattern;

    mlir::LogicalResult matchAndRewrite(
        mlir::memref::DimOp op, mlir::PatternRewriter &rewriter) const override
    {
        auto enforce_op = mlir::dyn_cast_or_null<plier::EnforceShapeOp>(op.memrefOrTensor().getDefiningOp());
        if (!enforce_op)
        {
            return mlir::failure();
        }
        auto const_ind = plier::getConstVal<mlir::IntegerAttr>(op.index());
        if (!const_ind)
        {
            return mlir::failure();
        }
        auto index = const_ind.getInt();
        if (index < 0 || index >= static_cast<int64_t>(enforce_op.sizes().size()))
        {
            return mlir::failure();
        }

        rewriter.replaceOp(op, enforce_op.sizes()[static_cast<unsigned>(index)]);
        return mlir::success();
    }
};
}

void EnforceShapeOp::getCanonicalizationPatterns(
    ::mlir::OwningRewritePatternList &results, ::mlir::MLIRContext *context)
{
    results.insert<EnforceShapeDim>(context);
}

mlir::LogicalResult ParallelOp::moveOutOfLoop(mlir::ArrayRef<mlir::Operation *> ops)
{
    for (mlir::Operation *op : ops)
    {
        op->moveBefore(*this);
    }
    return mlir::success();
}

mlir::Region &ParallelOp::getLoopBody() { return region(); }

bool ParallelOp::isDefinedOutsideOfLoop(mlir::Value value)
{
  return !region().isAncestor(value.getParentRegion());
}

void ParallelOp::build(
    mlir::OpBuilder &odsBuilder, mlir::OperationState &odsState,
    mlir::ValueRange lowerBounds, mlir::ValueRange upperBounds, mlir::ValueRange steps,
    mlir::function_ref<void(mlir::OpBuilder &, mlir::Location, mlir::ValueRange,
                            mlir::ValueRange, mlir::Value)> bodyBuilder) {
    assert(lowerBounds.size() == upperBounds.size());
    assert(lowerBounds.size() == steps.size());
    odsState.addOperands(lowerBounds);
    odsState.addOperands(upperBounds);
    odsState.addOperands(steps);
    odsState.addAttribute(
        ParallelOp::getOperandSegmentSizeAttr(),
        odsBuilder.getI32VectorAttr({static_cast<int32_t>(lowerBounds.size()),
                                     static_cast<int32_t>(upperBounds.size()),
                                     static_cast<int32_t>(steps.size())}));
    auto bodyRegion = odsState.addRegion();
    auto count = lowerBounds.size();
    mlir::OpBuilder::InsertionGuard guard(odsBuilder);
    llvm::SmallVector<mlir::Type> argTypes(count * 2 + 1, odsBuilder.getIndexType());
    auto *bodyBlock = odsBuilder.createBlock(bodyRegion, {}, argTypes);

    if (bodyBuilder)
    {
        odsBuilder.setInsertionPointToStart(bodyBlock);
        auto args = bodyBlock->getArguments();
        bodyBuilder(odsBuilder, odsState.location,
                    args.take_front(count),
                    args.drop_front(count).take_front(count),
                    args.back());
        ParallelOp::ensureTerminator(*bodyRegion, odsBuilder, odsState.location);
    }
}

}

#define GET_OP_CLASSES
#include "plier/PlierOps.cpp.inc"

#include "plier/PlierOpsEnums.cpp.inc"