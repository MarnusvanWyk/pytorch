#include <torch/csrc/jit/operator.h>
#include <torch/csrc/jit/script/compiler.h>
#include <torch/csrc/jit/passes/decompose_ops.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/utils/subgraph_utils.h>

namespace torch {
namespace jit {

Value* decomposeOp(
    Node* op,
    const char* source,
    const std::string& method_name,
    const at::ArrayRef<Value*> inputs) {
  std::shared_ptr<Graph> d_graph;
  std::once_flag flag;
  std::call_once(
      flag,
      [](std::shared_ptr<Graph>* graph_ptr,
         const char* source,
         const std::string& method_name) {
        script::CompilationUnit cu;
        cu.define(source, script::nativeResolver(), nullptr);
        *graph_ptr = cu.get_function(method_name).graph();
      },
      &d_graph,
      source,
      method_name);

  Value* new_output =
      SubgraphUtils::inlineGraph(d_graph, inputs, op).at(0);
  return new_output;
}

static void DecomposeOps(Block* block) {
  static const char* addmm_source = R"SCRIPT(
      def addmm(self: Tensor, mat1: Tensor, mat2: Tensor, beta: number = 1.0, alpha: number = 1.0):
          return self + mat1.mm(mat2)
    )SCRIPT";

  for (auto it = block->nodes().begin(), end = block->nodes().end(); it != end;
       ++it) {
    for (auto sub : it->blocks()) {
      DecomposeOps(sub);
    }

    if (it->matches(
            "aten::addmm(Tensor self, Tensor mat1, Tensor mat2, *, Scalar beta, Scalar alpha) -> Tensor",
            /*const_inputs=*/{attr::beta, attr::alpha})) {
      // For the case where we have an addmm where alpha and beta are Attributes
      // and both of those scalars are equal to 1.0, decompose this into an mm
      // followed by an add so that it can go through the existing optimization (batchmm)
      if (it->get<at::Scalar>(attr::alpha)->toDouble() != 1.0 ||
          it->get<at::Scalar>(attr::beta)->toDouble() != 1.0) {
        continue;
      }

      WithInsertPoint guard(*it);

      Value* new_output = decomposeOp(*it, addmm_source, "addmm", it->inputs());
      // Set the output of the decomposed graph to have the same output type as the
      // original op otherwise the canonicalized graph will have
      // TensorType as the output of this node which is incorrect
      new_output->setType(it->output()->type());
      it->output()->replaceAllUsesWith(new_output);
      it.destroyCurrent();
    }
  }

}

void DecomposeOps(const std::shared_ptr<Graph>& graph) {
  DecomposeOps(graph->block());
  EliminateDeadCode(graph);
}

} // namespace jit
} // namespace torch
