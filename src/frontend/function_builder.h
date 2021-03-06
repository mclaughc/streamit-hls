#pragma once
#include <stack>
#include <unordered_map>
#include <vector>
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "parser/ast_visitor.h"

namespace llvm
{
class FunctionType;
}

namespace Frontend
{
class WrappedLLVMContext;

class FunctionBuilder : public AST::Visitor
{
public:
  struct TargetFragmentBuilder
  {
    virtual llvm::Value* BuildPop(llvm::IRBuilder<>& builder) = 0;
    virtual llvm::Value* BuildPeek(llvm::IRBuilder<>& builder, llvm::Value* idx_value) = 0;
    virtual bool BuildPush(llvm::IRBuilder<>& builder, llvm::Value* value) = 0;
  };

  using VariableTable = std::unordered_map<const AST::Declaration*, llvm::Value*>;

  FunctionBuilder(WrappedLLVMContext* ctx, llvm::Module* mod, TargetFragmentBuilder* target_builder,
                  llvm::Function* func);
  ~FunctionBuilder();

  bool Visit(AST::Node* node) override;
  bool Visit(AST::VariableDeclaration* node) override;
  bool Visit(AST::Statement* node) override;

  WrappedLLVMContext* GetContext() const { return m_context; }
  llvm::Module* GetModule() const { return m_module; }
  llvm::Function* GetFunction() const { return m_func; }
  TargetFragmentBuilder* GetTargetFragmentBuilder() const { return m_target_builder; }
  llvm::BasicBlock* GetEntryBasicBlock() const { return m_entry_basic_block; }
  llvm::BasicBlock* GetCurrentBasicBlock() const { return m_current_basic_block; }
  llvm::IRBuilder<>& GetCurrentIRBuilder() { return m_current_ir_builder; }

  void CreateParameterVariables(const std::vector<AST::ParameterDeclaration*>* func_params);
  void AddVariable(const AST::Declaration* var, llvm::Value* val);
  llvm::AllocaInst* CreateVariable(const AST::Declaration* var);
  llvm::Value* GetVariable(const AST::Declaration* var);

  // Returns the old basic block pointer
  llvm::BasicBlock* NewBasicBlock(const std::string& name = {});

  // Switches to the specific basic block
  void SwitchBasicBlock(llvm::BasicBlock* new_bb);

  // break/continue basic block points - this is a stack
  llvm::BasicBlock* GetCurrentBreakBasicBlock() const;
  void PushBreakBasicBlock(llvm::BasicBlock* bb);
  void PopBreakBasicBlock();
  llvm::BasicBlock* GetCurrentContinueBasicBlock() const;
  void PushContinueBasicBlock(llvm::BasicBlock* bb);
  void PopContinueBasicBlock();

  static llvm::FunctionType* GetFunctionType(WrappedLLVMContext* context,
                                             const std::vector<AST::ParameterDeclaration*>* func_params);

protected:
  WrappedLLVMContext* m_context;
  llvm::Module* m_module;
  TargetFragmentBuilder* m_target_builder;
  llvm::Function* m_func;
  llvm::BasicBlock* m_entry_basic_block;
  llvm::BasicBlock* m_current_basic_block;
  llvm::IRBuilder<> m_current_ir_builder;
  VariableTable m_vars;
  std::stack<llvm::BasicBlock*> m_break_basic_block_stack;
  std::stack<llvm::BasicBlock*> m_continue_basic_block_stack;
};
}