#ifndef ONEFLOW_CORE_KERNEL_RING_REDUCE_SCATTER_KERNEL_H_
#define ONEFLOW_CORE_KERNEL_RING_REDUCE_SCATTER_KERNEL_H_

#include "oneflow/core/kernel/kernel.h"
#include "oneflow/core/register/tensor_slice_copier.h"
#include "oneflow/core/device/memory_copier.h"

namespace oneflow {

template<DeviceType device_type, typename T>
class RingReduceScatterKernel final : public KernelIf<device_type> {
 public:
  OF_DISALLOW_COPY_AND_MOVE(RingReduceScatterKernel);
  RingReduceScatterKernel() = default;
  ~RingReduceScatterKernel() override = default;

 private:
  void VirtualKernelInit(const ParallelContext*) override;
  void ForwardDim0ValidNum(const KernelCtx& ctx,
                           std::function<Blob*(const std::string&)> BnInOp2Blob) const override;
  void ForwardDataContent(const KernelCtx&,
                          std::function<Blob*(const std::string&)>) const override;

  TensorSliceView in_slice_;
  std::vector<TensorSliceView> chunk_slices_;
  int64_t num_steps_;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_KERNEL_RING_REDUCE_SCATTER_KERNEL_H_
