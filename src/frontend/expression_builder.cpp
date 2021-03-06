#include "frontend/expression_builder.h"
#include <cassert>
#include "frontend/function_builder.h"
#include "frontend/wrapped_llvm_context.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "parser/ast.h"

namespace Frontend
{
ExpressionBuilder::ExpressionBuilder(FunctionBuilder* func_builder) : m_func_builder(func_builder)
{
}

ExpressionBuilder::~ExpressionBuilder()
{
}

WrappedLLVMContext* ExpressionBuilder::GetContext() const
{
  return m_func_builder->GetContext();
}

llvm::IRBuilder<>& ExpressionBuilder::GetIRBuilder() const
{
  return m_func_builder->GetCurrentIRBuilder();
}

bool ExpressionBuilder::IsValid() const
{
  return (m_result_ptr || m_result_value);
}

bool ExpressionBuilder::IsPointer() const
{
  return (m_result_ptr != nullptr);
}

llvm::Value* ExpressionBuilder::GetResultPtr()
{
  return m_result_ptr;
}

llvm::Value* ExpressionBuilder::GetResultValue()
{
  if (!m_result_value && m_result_ptr)
  {
    // Load pointer
    m_result_value = GetIRBuilder().CreateLoad(m_result_ptr);
  }

  return m_result_value;
}

bool ExpressionBuilder::Visit(AST::Node* node)
{
  assert(0 && "Fallback handler executed");
  return false;
}

bool ExpressionBuilder::Visit(AST::IntegerLiteralExpression* node)
{
  llvm::Type* llvm_type = GetContext()->GetLLVMType(node->GetType());
  assert(llvm_type);

  m_result_value = llvm::ConstantInt::get(llvm_type, static_cast<uint64_t>(node->GetValue()), true);
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::BooleanLiteralExpression* node)
{
  llvm::Type* llvm_type = GetContext()->GetLLVMType(node->GetType());
  assert(llvm_type);

  m_result_value = llvm::ConstantInt::get(llvm_type, node->GetValue() ? 1 : 0);
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::FloatLiteralExpression* node)
{
  llvm::Type* llvm_type = GetContext()->GetLLVMType(node->GetType());
  assert(llvm_type);

  m_result_value = llvm::ConstantFP::get(llvm_type, double(node->GetValue()));
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::IdentifierExpression* node)
{
  // TODO: This should check the type (pointer or value).
  if (node->GetReferencedDeclaration()->IsConstant())
    m_result_value = m_func_builder->GetVariable(node->GetReferencedDeclaration());
  else
    m_result_ptr = m_func_builder->GetVariable(node->GetReferencedDeclaration());

  return IsValid();
}

bool ExpressionBuilder::Visit(AST::IndexExpression* node)
{
  // We could skip this if it is an identifier/constant..
  // LLVM will probably take care of that, though.
  ExpressionBuilder array_builder(m_func_builder);
  ExpressionBuilder index_builder(m_func_builder);
  if (!node->GetArrayExpression()->Accept(&array_builder) || !array_builder.IsValid() ||
      !node->GetIndexExpression()->Accept(&index_builder) || !index_builder.IsValid())
  {
    return false;
  }

  llvm::Type* array_llvm_type = GetContext()->GetLLVMType(node->GetArrayExpression()->GetType());
  m_result_ptr = GetIRBuilder().CreateInBoundsGEP(array_llvm_type, array_builder.GetResultPtr(),
                                                  {GetIRBuilder().getInt32(0), index_builder.GetResultValue()});
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::CommaExpression* node)
{
  // Evaluate both sides, discard the left, and return the right.
  ExpressionBuilder eb_lhs(m_func_builder);
  ExpressionBuilder eb_rhs(m_func_builder);
  if (!node->GetLHSExpression()->Accept(&eb_lhs) || !eb_lhs.IsValid() || !node->GetLHSExpression()->Accept(&eb_rhs) ||
      !eb_rhs.IsValid())
  {
    return false;
  }

  if (eb_rhs.IsPointer())
    m_result_ptr = eb_rhs.GetResultPtr();
  else
    m_result_value = eb_rhs.GetResultValue();

  return IsValid();
}

bool ExpressionBuilder::Visit(AST::AssignmentExpression* node)
{
  // Evaluate the expression, and return the result
  ExpressionBuilder eb(m_func_builder);
  if (!node->GetInnerExpression()->Accept(&eb) || !eb.IsValid())
    return false;

  ExpressionBuilder lb(m_func_builder);
  if (!node->GetLValueExpression()->Accept(&lb) || !lb.IsValid())
    return false;

  assert(lb.IsPointer() && "assigning to an lvalue/pointer");

  // TODO: Implicit type conversions
  m_result_ptr = lb.GetResultPtr();
  GetIRBuilder().CreateStore(eb.GetResultValue(), m_result_ptr);
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::UnaryExpression* node)
{
  // Evaluate both lhs and rhs first (left-to-right?)
  ExpressionBuilder eb(m_func_builder);
  if (!node->GetRHSExpression()->Accept(&eb) || !eb.IsValid())
    return false;

  // Increment ops need a pointer
  if (node->GetOperator() >= AST::UnaryExpression::PreIncrement &&
      node->GetOperator() <= AST::UnaryExpression::PostDecrement && !eb.IsPointer())
  {
    assert(0 && "Increment/decrement require a pointer");
    return false;
  }

  // TODO: Float operands
  if (node->GetType()->IsInt() || node->GetType()->IsAPInt())
  {
    // NSW - overflow handling
    llvm::Value* temp;
    switch (node->GetOperator())
    {
    case AST::UnaryExpression::PostIncrement:
      m_result_value = eb.GetResultValue();
      temp = GetIRBuilder().CreateNSWAdd(m_result_value, GetIRBuilder().getInt32(1));
      GetIRBuilder().CreateStore(temp, eb.GetResultPtr());
      break;
    case AST::UnaryExpression::PostDecrement:
      m_result_value = eb.GetResultValue();
      temp = GetIRBuilder().CreateNSWSub(m_result_value, GetIRBuilder().getInt32(1));
      GetIRBuilder().CreateStore(temp, eb.GetResultPtr());
      break;
    case AST::UnaryExpression::PreIncrement:
      m_result_value = GetIRBuilder().CreateNSWAdd(eb.GetResultValue(), GetIRBuilder().getInt32(1));
      GetIRBuilder().CreateStore(m_result_value, eb.GetResultPtr());
      break;
    case AST::UnaryExpression::PreDecrement:
      m_result_value = GetIRBuilder().CreateNSWSub(eb.GetResultValue(), GetIRBuilder().getInt32(1));
      GetIRBuilder().CreateStore(m_result_value, eb.GetResultPtr());
      break;
    default:
      assert(0 && "unknown op");
      break;
    }
  }

  if (node->GetType()->IsFloat())
  {
    assert(0 && "implement me");
  }

  return IsValid();
}

bool ExpressionBuilder::Visit(AST::BinaryExpression* node)
{
  // Evaluate both lhs and rhs first (left-to-right?)
  ExpressionBuilder eb_lhs(m_func_builder);
  ExpressionBuilder eb_rhs(m_func_builder);
  if (!node->GetLHSExpression()->Accept(&eb_lhs) || !eb_lhs.IsValid() || !node->GetRHSExpression()->Accept(&eb_rhs) ||
      !eb_rhs.IsValid())
  {
    return false;
  }

  // TODO: Float operands
  if (node->GetType()->IsInt() || node->GetType()->IsAPInt())
  {
    // TODO: Type conversion where LHS type != RHS type
    assert(*node->GetLHSExpression()->GetType() == *node->GetType() &&
           *node->GetRHSExpression()->GetType() == *node->GetType());

    llvm::Value* lhs_val = eb_lhs.GetResultValue();
    llvm::Value* rhs_val = eb_rhs.GetResultValue();
    switch (node->GetOperator())
    {
    case AST::BinaryExpression::Add:
      m_result_value = GetIRBuilder().CreateNSWAdd(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Subtract:
      m_result_value = GetIRBuilder().CreateNSWSub(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Multiply:
      m_result_value = GetIRBuilder().CreateNSWMul(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Divide:
      m_result_value = GetIRBuilder().CreateSDiv(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Modulo:
      m_result_value = GetIRBuilder().CreateSRem(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::BitwiseAnd:
      m_result_value = GetIRBuilder().CreateAnd(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::BitwiseOr:
      m_result_value = GetIRBuilder().CreateOr(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::BitwiseXor:
      m_result_value = GetIRBuilder().CreateXor(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::LeftShift:
      m_result_value = GetIRBuilder().CreateShl(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::RightShift:
      m_result_value = GetIRBuilder().CreateAShr(lhs_val, rhs_val);
      break;
    default:
      assert(0 && "not reachable");
      break;
    }
  }

  if (node->GetType()->IsFloat())
  {
    // TODO: Type conversion where LHS type != RHS type
    assert(*node->GetLHSExpression()->GetType() == *node->GetType() &&
           *node->GetRHSExpression()->GetType() == *node->GetType());

    llvm::Value* lhs_val = eb_lhs.GetResultValue();
    llvm::Value* rhs_val = eb_rhs.GetResultValue();
    switch (node->GetOperator())
    {
    case AST::BinaryExpression::Add:
      m_result_value = GetIRBuilder().CreateFAdd(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Subtract:
      m_result_value = GetIRBuilder().CreateFSub(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Multiply:
      m_result_value = GetIRBuilder().CreateFMul(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Divide:
      m_result_value = GetIRBuilder().CreateFDiv(lhs_val, rhs_val);
      break;
    case AST::BinaryExpression::Modulo:
      m_result_value = GetIRBuilder().CreateFRem(lhs_val, rhs_val);
      break;
    default:
      assert(0 && "not reachable");
      break;
    }
  }

  return IsValid();
}

bool ExpressionBuilder::Visit(AST::RelationalExpression* node)
{
  // Evaluate both lhs and rhs first (left-to-right?)
  ExpressionBuilder eb_lhs(m_func_builder);
  ExpressionBuilder eb_rhs(m_func_builder);
  if (!node->GetLHSExpression()->Accept(&eb_lhs) || !eb_lhs.IsValid() || !node->GetRHSExpression()->Accept(&eb_rhs) ||
      !eb_rhs.IsValid())
  {
    return false;
  }

  // TODO: Float operands
  if (node->GetIntermediateType()->IsInt() || node->GetIntermediateType()->IsAPInt())
  {
    // TODO: Type conversion where LHS type != RHS type
    assert(*node->GetLHSExpression()->GetType() == *node->GetIntermediateType() &&
           *node->GetRHSExpression()->GetType() == *node->GetIntermediateType());

    llvm::Value* lhs_val = eb_lhs.GetResultValue();
    llvm::Value* rhs_val = eb_rhs.GetResultValue();
    switch (node->GetOperator())
    {
    case AST::RelationalExpression::Less:
      m_result_value = GetIRBuilder().CreateICmpSLT(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::LessEqual:
      m_result_value = GetIRBuilder().CreateICmpSLE(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::Greater:
      m_result_value = GetIRBuilder().CreateICmpSGT(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::GreaterEqual:
      m_result_value = GetIRBuilder().CreateICmpSGE(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::Equal:
      m_result_value = GetIRBuilder().CreateICmpEQ(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::NotEqual:
      m_result_value = GetIRBuilder().CreateICmpNE(lhs_val, rhs_val);
      break;
    default:
      assert(0 && "not reachable");
      break;
    }
  }

  if (node->GetIntermediateType()->IsFloat())
  {
    // TODO: Type conversion where LHS type != RHS type
    assert(*node->GetLHSExpression()->GetType() == *node->GetIntermediateType() &&
           *node->GetRHSExpression()->GetType() == *node->GetIntermediateType());

    // ordered = check for NaN, unordered = no check
    // C uses ordered, so we'll follow these semantics.
    llvm::Value* lhs_val = eb_lhs.GetResultValue();
    llvm::Value* rhs_val = eb_rhs.GetResultValue();
    switch (node->GetOperator())
    {
    case AST::RelationalExpression::Less:
      m_result_value = GetIRBuilder().CreateFCmpOLT(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::LessEqual:
      m_result_value = GetIRBuilder().CreateFCmpOLE(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::Greater:
      m_result_value = GetIRBuilder().CreateFCmpOGT(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::GreaterEqual:
      m_result_value = GetIRBuilder().CreateFCmpOGE(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::Equal:
      m_result_value = GetIRBuilder().CreateFCmpOEQ(lhs_val, rhs_val);
      break;
    case AST::RelationalExpression::NotEqual:
      m_result_value = GetIRBuilder().CreateFCmpONE(lhs_val, rhs_val);
      break;
    default:
      assert(0 && "not reachable");
      break;
    }
  }

  return IsValid();
}

bool ExpressionBuilder::Visit(AST::LogicalExpression* node)
{
  // Evaluate left-hand side first, always, in the parent block
  ExpressionBuilder eb_lhs(m_func_builder);
  if (!node->GetLHSExpression()->Accept(&eb_lhs) || !eb_lhs.IsValid())
    return false;

  // Both should be boolean types.
  assert(node->GetLHSExpression()->GetType()->IsBoolean() && node->GetRHSExpression()->GetType()->IsBoolean());

  // Create a new block for the right-hand side of the expression
  llvm::BasicBlock* lhs_end_bb = m_func_builder->NewBasicBlock();
  ExpressionBuilder eb_rhs(m_func_builder);
  if (!node->GetRHSExpression()->Accept(&eb_rhs) || !eb_rhs.IsValid())
    return false;

  llvm::BasicBlock* rhs_end_bb = m_func_builder->NewBasicBlock();
  llvm::BasicBlock* merge_bb = m_func_builder->GetCurrentBasicBlock();

  // Logical AND
  if (node->GetOperator() == AST::LogicalExpression::And)
  {
    // Branch to right-hand side if the left-hand side is true, otherwise branch to end
    llvm::IRBuilder<>(lhs_end_bb).CreateCondBr(eb_lhs.GetResultValue(), rhs_end_bb, merge_bb);

    // Branch to merge point after evaluating the right-hand side
    llvm::IRBuilder<>(rhs_end_bb).CreateBr(merge_bb);

    // Create phi node from result
    llvm::PHINode* phi = GetIRBuilder().CreatePHI(GetContext()->GetLLVMType(node->GetType()), 2, "");
    phi->addIncoming(GetIRBuilder().getInt1(0), lhs_end_bb);
    phi->addIncoming(eb_rhs.GetResultValue(), rhs_end_bb);
    m_result_value = phi;
  }

  // Logical OR
  else if (node->GetOperator() == AST::LogicalExpression::Or)
  {
    // Branch to merge point if left-hand side is true, otherwise branch to right-hand side
    llvm::IRBuilder<>(lhs_end_bb).CreateCondBr(eb_lhs.GetResultValue(), merge_bb, rhs_end_bb);

    // Branch to merge point after evaluating the right-hand side
    llvm::IRBuilder<>(rhs_end_bb).CreateBr(merge_bb);

    // Create phi node from result
    llvm::PHINode* phi = GetIRBuilder().CreatePHI(GetContext()->GetLLVMType(node->GetType()), 2, "");
    phi->addIncoming(GetIRBuilder().getInt1(1), lhs_end_bb);
    phi->addIncoming(eb_rhs.GetResultValue(), rhs_end_bb);
    m_result_value = phi;
  }

  return IsValid();
}

bool ExpressionBuilder::Visit(AST::PeekExpression* node)
{
  ExpressionBuilder index_eb(m_func_builder);
  if (!node->GetIndexExpression()->Accept(&index_eb) || !index_eb.IsValid())
    return false;

  // TODO: Implicit conversions
  assert(node->GetIndexExpression()->GetType()->IsInt());
  m_result_value = m_func_builder->GetTargetFragmentBuilder()->BuildPeek(GetIRBuilder(), index_eb.GetResultValue());
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::PopExpression* node)
{
  m_result_value = m_func_builder->GetTargetFragmentBuilder()->BuildPop(GetIRBuilder());
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::CallExpression* node)
{
  const AST::FunctionDeclaration* fref = node->GetFunctionReference();
  assert(fref != nullptr);

  // Map types
  llvm::Type* return_type = GetContext()->GetLLVMType(fref->GetReturnType());
  std::vector<llvm::Type*> param_types;
  for (const AST::TypeSpecifier* param_ty : fref->GetParameterTypes())
    param_types.push_back(GetContext()->GetLLVMType(param_ty));

  // Create prototype if it doesn't already exist. This may be external.
  std::string func_name = fref->GetExecutableSymbolName();
  llvm::FunctionType* func_type = llvm::FunctionType::get(return_type, param_types, false);
  llvm::Constant* func = m_func_builder->GetModule()->getOrInsertFunction(func_name, func_type);
  if (!func)
  {
    GetContext()->LogError("Unable to get function '%s'", func_name.c_str());
    return false;
  }

  // Get values for each of the parameters
  std::vector<llvm::Value*> func_params;
  if (node->HasArgs())
  {
    for (size_t i = 0; i < node->GetArgList()->GetNodeList().size(); i++)
    {
      // TODO: Implicit conversions here
      ExpressionBuilder param_eb(m_func_builder);
      if (!node->GetArgList()->GetNodeList().at(i)->Accept(&param_eb) || !param_eb.IsValid())
        return false;

      func_params.push_back(param_eb.GetResultValue());
    }
  }

  m_result_value = GetIRBuilder().CreateCall(func, func_params);
  return IsValid();
}

bool ExpressionBuilder::Visit(AST::CastExpression* node)
{
  // Evaluate the expression first.
  ExpressionBuilder expr_builder(m_func_builder);
  if (!node->GetExpression()->Accept(&expr_builder) || !expr_builder.IsValid())
    return false;

  // Work out types.
  llvm::Type* to_type = GetContext()->GetLLVMType(node->GetToType());
  llvm::Type* expr_type = expr_builder.GetResultValue()->getType();
  llvm::Value* expr_value = expr_builder.GetResultValue();

  // Same type/redundant cast?
  if (expr_type == to_type)
  {
    m_result_value = expr_value;
    return IsValid();
  }

  // Integer types?
  if (expr_type->isIntegerTy() && to_type->isIntegerTy())
  {
    // Smaller bit width -> larger bit width?
    // Sign extend. Except for bit/apint1, zero-extend.
    // Otherwise, truncate.
    bool is_bit_type = (static_cast<llvm::IntegerType*>(expr_type)->getBitWidth() == 1);
    m_result_value = GetIRBuilder().CreateIntCast(expr_value, to_type, !is_bit_type);
    return IsValid();
  }

  // Int<->float
  if ((expr_type->isIntegerTy() || expr_type->isFloatTy()) && (to_type->isIntegerTy() || to_type->isFloatTy()))
  {
    // Our integer types are always signed.
    bool src_signed = expr_type->isIntegerTy();
    bool dst_signed = to_type->isIntegerTy();

    // Work out the cast opcode.
    llvm::Instruction::CastOps opcode = llvm::CastInst::getCastOpcode(expr_value, src_signed, to_type, dst_signed);

    // Create cast instruction.
    m_result_value = GetIRBuilder().CreateCast(opcode, expr_value, to_type);
    return IsValid();
  }

  assert(0 && "Unhandled cast");
  return false;
}
}
