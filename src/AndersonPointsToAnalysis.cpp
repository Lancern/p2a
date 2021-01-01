//
// Created by Sirui Mu on 2020/12/30.
//

#include "llvm-anderson/AndersonPointsToAnalysis.h"

#include <llvm/IR/Instructions.h>

namespace llvm {

namespace anderson {

class AndersonPointsToAnalysis::PointsToSolver {
  // TODO: Implement AndersonPointsToAnalysis::PointsToSolver

public:
  explicit PointsToSolver(const llvm::Module &module) noexcept
    : _valueTree(std::make_unique<ValueTree>(module))
  { }

  ValueTree* GetValueTree() const noexcept {
    return _valueTree.get();
  }

  void Solve() noexcept {
    // TODO: Implement AndersonPointsToAnalysis::PointsToSolver::Solve
  }

private:
  std::unique_ptr<ValueTree> _valueTree;
};

namespace {

using PointsToSolver = AndersonPointsToAnalysis::PointsToSolver;

template <typename Instruction>
struct PointerInstructionHandler { };

template <>
struct PointerInstructionHandler<llvm::ExtractValueInst> {
  static void Handle(PointsToSolver &solver, const llvm::ExtractValueInst &inst) noexcept {
    if (!inst.getType()->isPointerTy()) {
      return;
    }

    auto targetPtrValue = static_cast<const llvm::Value *>(&inst);
    auto targetPtrNode = solver.GetValueTree()->GetNode(targetPtrValue);
    assert(targetPtrNode->isPointer());

    auto sourceValue = inst.getAggregateOperand();
    auto sourcePtrNode = solver.GetValueTree()->GetNode(sourceValue);
    for (auto index : inst.indices()) {
      sourcePtrNode = sourcePtrNode->GetChild(static_cast<size_t>(index));
    }
    assert(sourcePtrNode->isPointer());

    targetPtrNode->pointer()->AddPointsToPointeesOf(sourcePtrNode->pointer());
  }
};

template <>
struct PointerInstructionHandler<llvm::GetElementPtrInst> {
  static void Handle(PointsToSolver &solver, const llvm::GetElementPtrInst &inst) noexcept {
    auto targetPtrValue = static_cast<const llvm::Value *>(&inst);
    auto targetPtrNode = solver.GetValueTree()->GetNode(targetPtrValue);
    assert(targetPtrNode->isPointer());

    // TODO: Implement PointerInstructionHandler<llvm::GetElementPtrInst>::Handle
  }
};

template <>
struct PointerInstructionHandler<llvm::LoadInst> {
  static void Handle(PointsToSolver &solver, const llvm::LoadInst &inst) noexcept {
    if (!inst.getType()->isPointerTy()) {
      return;
    }

    auto resultPtrValue = static_cast<const llvm::Value *>(&inst);
    auto sourcePtrValue = inst.getPointerOperand();
    auto resultPtrNode = solver.GetValueTree()->GetNode(resultPtrValue);
    auto sourcePtrNode = solver.GetValueTree()->GetNode(sourcePtrValue);

    assert(resultPtrNode->isPointer());
    assert(sourcePtrNode->isPointer());

    resultPtrNode->pointer()->AddPointsToPointeesOfPointeesOf(sourcePtrNode->pointer());
  }
};

template <>
struct PointerInstructionHandler<llvm::PHINode> {
  static void Handle(PointsToSolver &solver, const llvm::PHINode &phi) noexcept {
    if (!phi.getType()->isPointerTy()) {
      return;
    }

    auto resultPtrValue = static_cast<const llvm::Value *>(&phi);
    auto resultPtrNode = solver.GetValueTree()->GetNode(resultPtrValue);
    assert(resultPtrNode->isPointer());

    for (const auto &sourcePtrValueUse : phi.incoming_values()) {
      auto sourcePtrValue = sourcePtrValueUse.get();
      auto sourcePtrNode = solver.GetValueTree()->GetNode(sourcePtrValue);
      assert(sourcePtrNode->isPointer());

      resultPtrNode->pointer()->AddPointsToPointeesOf(sourcePtrNode->pointer());
    }
  }
};

template <>
struct PointerInstructionHandler<llvm::SelectInst> {
  static void Handle(PointsToSolver &solver, const llvm::SelectInst &inst) noexcept {
    if (!inst.getType()->isPointerTy()) {
      return;
    }

    auto resultPtrValue = static_cast<const llvm::Value *>(&inst);
    auto resultPtrNode = solver.GetValueTree()->GetNode(resultPtrValue);
    assert(resultPtrNode->isPointer());

    const llvm::Value *sourcePtrValues[2] = {
        inst.getTrueValue(),
        inst.getFalseValue()
    };
    for (auto sourcePtrValue : sourcePtrValues) {
      auto sourcePtrNode = solver.GetValueTree()->GetNode(sourcePtrValue);
      assert(sourcePtrNode->isPointer());

      resultPtrNode->pointer()->AddPointsToPointeesOf(sourcePtrNode->pointer());
    }
  }
};

template <>
struct PointerInstructionHandler<llvm::StoreInst> {
  static void Handle(PointsToSolver &solver, const llvm::StoreInst &inst) noexcept {
    auto targetPtrValue = inst.getPointerOperand();
    if (!targetPtrValue->getType()->getPointerElementType()->isPointerTy()) {
      return;
    }

    auto sourcePtrValue = inst.getValueOperand();

    auto targetPtrNode = solver.GetValueTree()->GetNode(targetPtrValue);
    auto sourcePtrNode = solver.GetValueTree()->GetNode(sourcePtrValue);
    assert(targetPtrNode->isPointer());
    assert(sourcePtrNode->isPointer());

    targetPtrNode->pointer()->AddPointeePointsToPointeesOf(sourcePtrNode->pointer());
  }
};

#define LLVM_POINTER_INST_LIST(H) \
  H(ExtractValueInst)             \
  H(GetElementPtrInst)            \
  H(LoadInst)                     \
  H(PHINode)                      \
  H(SelectInst)                   \
  H(StoreInst)

void UpdateAndersonSolverOnInst(PointsToSolver &solver, const llvm::Instruction &inst) noexcept {
#define INST_DISPATCHER(instType)                                                                 \
  if (llvm::isa<llvm::instType>(inst)) {                                                          \
    PointerInstructionHandler<llvm::instType>::Handle(solver, llvm::cast<llvm::instType>(inst));  \
  }
LLVM_POINTER_INST_LIST(INST_DISPATCHER)
#undef INST_DISPATCHER
}

#undef LLVM_POINTER_INST_LIST

} // namespace <anonymous>

char AndersonPointsToAnalysis::ID = 0;

AndersonPointsToAnalysis::AndersonPointsToAnalysis() noexcept
  : llvm::ModulePass { ID },
    _solver(nullptr)
{ }

AndersonPointsToAnalysis::~AndersonPointsToAnalysis() noexcept = default;

bool AndersonPointsToAnalysis::runOnModule(llvm::Module &module) {
  _solver = std::make_unique<PointsToSolver>(module);

  for (const auto &func : module) {
    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        UpdateAndersonSolverOnInst(*_solver, inst);
      }
    }
  }
  _solver->Solve();

  return false;  // The module is not modified by this pass.
}

ValueTree* AndersonPointsToAnalysis::GetValueTree() noexcept {
  return _solver->GetValueTree();
}

const ValueTree* AndersonPointsToAnalysis::GetValueTree() const noexcept {
  return _solver->GetValueTree();
}

static llvm::RegisterPass<AndersonPointsToAnalysis> RegisterAnderson { // NOLINT(cert-err58-cpp)
  "anderson",
  "Anderson points-to analysis",
  true,
  true
};

} // namespace anderson

} // namespace llvm