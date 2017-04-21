#include "operator/clone_op.h"

namespace oneflow {

void CloneOp::Init(const OperatorConf& op_conf) {
  mut_op_name() = op_conf.name();

  CHECK(op_conf.has_clone_conf());
  auto cnf = new CloneOpConf(op_conf.clone_conf());
  mut_pb_op_conf().reset(cnf);

  EnrollInputBn("in");
  for (int64_t i = 0; i < cnf->out_num(); ++i) {
    EnrollOutputBn("out_" + std::to_string(i));
  }
}

} // namespace oneflow
