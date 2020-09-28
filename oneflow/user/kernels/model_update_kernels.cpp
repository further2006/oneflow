/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/core/framework/framework.h"
#include "oneflow/user/kernels/model_update_kernel_util.h"
#include "oneflow/core/kernel/indexed_slices_reduce_sum_kernel_util.h"
#include "oneflow/core/common/balanced_splitter.h"

namespace oneflow {

namespace {

template<DeviceType device_type, typename T, typename K>
class TmpBufferManager final {
 public:
  OF_DISALLOW_COPY_AND_MOVE(TmpBufferManager);
  TmpBufferManager(void* ptr, const int64_t num_indices, const int64_t num_values) : ptr_(ptr) {
    const size_t unique_diff_indices_bytes = GetCudaAlignedSize(num_indices * sizeof(K));
    const size_t unique_diff_values_bytes = GetCudaAlignedSize(num_values * sizeof(T));
    const size_t num_unique_diff_indices_bytes = GetCudaAlignedSize(1 * sizeof(int32_t));
    CHECK_EQ(num_values % num_indices, 0);
    IndexedSlicesReduceSumKernelUtil<device_type, K, T, int64_t>::GetReduceSumWorkspaceSizeInBytes(
        nullptr, num_indices, num_values / num_indices, &unique_workspace_bytes_);
    unique_diff_indices_offset_ = 0;
    unique_diff_values_offset_ = unique_diff_indices_offset_ + unique_diff_indices_bytes;
    num_unique_diff_indices_offset_ = unique_diff_values_offset_ + unique_diff_values_bytes;
    unique_workspace_offset_ = num_unique_diff_indices_offset_ + num_unique_diff_indices_bytes;
    CHECK_GE(unique_workspace_bytes_, 0);
    total_buffer_size_ = unique_diff_indices_bytes + unique_diff_values_bytes
                         + num_unique_diff_indices_bytes
                         + static_cast<size_t>(unique_workspace_bytes_);
  }
  ~TmpBufferManager() = default;

  int64_t UniqueWorkspaceBytes() const { return unique_workspace_bytes_; }
  size_t GetTotalBufferSize() const { return total_buffer_size_; }
  K* UniqueDiffIndicesPtr() const {
    CHECK(ptr_ != nullptr);
    return reinterpret_cast<K*>(reinterpret_cast<char*>(ptr_) + unique_diff_indices_offset_);
  }
  T* UniqueDiffValuesPtr() const {
    CHECK(ptr_ != nullptr);
    return reinterpret_cast<T*>(reinterpret_cast<char*>(ptr_) + unique_diff_values_offset_);
  }
  int32_t* NumUniqueDiffIndicesPtr() const {
    CHECK(ptr_ != nullptr);
    return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(ptr_)
                                      + num_unique_diff_indices_offset_);
  }
  char* UniqueWorkspacePtr() const {
    CHECK(ptr_ != nullptr);
    return reinterpret_cast<char*>(ptr_) + unique_workspace_offset_;
  }

 private:
  size_t unique_diff_indices_offset_;
  size_t unique_diff_values_offset_;
  size_t num_unique_diff_indices_offset_;
  size_t unique_workspace_offset_;

  int64_t unique_workspace_bytes_;
  size_t total_buffer_size_;
  void* ptr_;
};

class IndexedSlicesUpdateOpKernelState final : public user_op::OpKernelState {
 public:
  IndexedSlicesUpdateOpKernelState(int64_t lower, int64_t upper) : lower_(lower), upper_(upper) {}
  ~IndexedSlicesUpdateOpKernelState() override = default;

  int64_t lower() const { return lower_; }
  int64_t upper() const { return upper_; }

 private:
  const int64_t lower_;
  const int64_t upper_;
};

std::shared_ptr<user_op::OpKernelState> CreateIndexedSlicesUpdateOpKernelState(
    user_op::KernelInitContext* ctx) {
  const SbpParallel& model_sbp = ctx->SbpParallel4ArgNameAndIndex("model", 0);
  const user_op::TensorDesc* model_logical_desc =
      ctx->LogicalTensorDesc4ArgNameAndIndex("model", 0);
  const int64_t num_model_instances = model_logical_desc->shape().At(0);
  if (model_sbp.has_split_parallel() && model_sbp.split_parallel().axis() == 0
      && ctx->parallel_ctx().parallel_num() > 1) {
    CHECK(ctx->SbpParallel4ArgNameAndIndex("model_diff_indices", 0).has_broadcast_parallel());
    CHECK(ctx->SbpParallel4ArgNameAndIndex("model_diff_values", 0).has_broadcast_parallel());
    BalancedSplitter bs(num_model_instances, ctx->parallel_ctx().parallel_num());
    return std::make_shared<IndexedSlicesUpdateOpKernelState>(
        bs.At(ctx->parallel_ctx().parallel_id()).begin(),
        bs.At(ctx->parallel_ctx().parallel_id()).end());
  } else {
    return std::make_shared<IndexedSlicesUpdateOpKernelState>(0, num_model_instances);
  }
}

template<DeviceType device_type, typename T, typename G>
class SGDUpdateKernel final : public user_op::OpKernel {
 public:
  SGDUpdateKernel() = default;
  ~SGDUpdateKernel() override = default;

 private:
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* learning_rate = ctx->Tensor4ArgNameAndIndex("learning_rate", 0);
    const user_op::Tensor* model_diff = ctx->Tensor4ArgNameAndIndex("model_diff", 0);
    user_op::Tensor* model = ctx->Tensor4ArgNameAndIndex("model", 0);
    const auto scale = ctx->Attr<float>("scale");
    const auto l1 = ctx->Attr<float>("l1");
    const auto l2 = ctx->Attr<float>("l2");
    const auto weight_decay = ctx->Attr<float>("weight_decay");
    SGDUpdateKernelUtil<device_type, T, G>::Update(
        ctx->device_ctx(), model->shape().elem_cnt(), scale, l1, l2, weight_decay,
        learning_rate->dptr<float>(), model_diff->dptr<G>(), model->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

#define REGISTER_SGD_UPDATE_KERNEL(device, dtype, gtype)                                 \
  REGISTER_USER_KERNEL("sgd_update")                                                     \
      .SetCreateFn<SGDUpdateKernel<device, dtype, gtype>>()                              \
      .SetIsMatchedHob((user_op::HobDeviceTag() == device)                               \
                       & (user_op::HobDataType("model", 0) == GetDataType<dtype>::value) \
                       & (user_op::HobDataType("model_diff", 0) == GetDataType<gtype>::value));

REGISTER_SGD_UPDATE_KERNEL(DeviceType::kCPU, float, float);
REGISTER_SGD_UPDATE_KERNEL(DeviceType::kCPU, double, double);
#ifdef WITH_CUDA
REGISTER_SGD_UPDATE_KERNEL(DeviceType::kGPU, float, float16);
REGISTER_SGD_UPDATE_KERNEL(DeviceType::kGPU, float, float);
REGISTER_SGD_UPDATE_KERNEL(DeviceType::kGPU, double, double);
#endif  // WITH_CUDA

template<DeviceType device_type, typename T, typename K>
class IndexedSlicesSGDUpdateKernel final : public user_op::OpKernel {
 public:
  IndexedSlicesSGDUpdateKernel() = default;
  ~IndexedSlicesSGDUpdateKernel() override = default;

  std::shared_ptr<user_op::OpKernelState> CreateOpKernelState(
      user_op::KernelInitContext* ctx) const override {
    return CreateIndexedSlicesUpdateOpKernelState(ctx);
  }

 private:
  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState* state) const override {
    const user_op::Tensor* learning_rate = ctx->Tensor4ArgNameAndIndex("learning_rate", 0);
    const user_op::Tensor* model_diff_indices =
        ctx->Tensor4ArgNameAndIndex("model_diff_indices", 0);
    const user_op::Tensor* model_diff_values = ctx->Tensor4ArgNameAndIndex("model_diff_values", 0);
    user_op::Tensor* model = ctx->Tensor4ArgNameAndIndex("model", 0);
    auto* kernel_state = dynamic_cast<IndexedSlicesUpdateOpKernelState*>(state);
    CHECK_NOTNULL(kernel_state);
    CHECK_EQ(model->shape().At(0), kernel_state->upper() - kernel_state->lower());
    IndexedSlicesSGDUpdateKernelUtil<device_type, T, K>::Update(
        ctx->device_ctx(), model_diff_indices->shape().elem_cnt(), model->shape().At(0),
        model->shape().Count(1), kernel_state->lower(), learning_rate->dptr<float>(),
        model_diff_indices->dptr<K>(), model_diff_values->dptr<T>(), model->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

#define REGISTER_INDEXED_SLICES_SGD_UPDATE_KERNEL(device_type_v, data_type_pair,                 \
                                                  indices_type_pair)                             \
  REGISTER_USER_KERNEL("indexed_slices_sgd_update")                                              \
      .SetCreateFn<IndexedSlicesSGDUpdateKernel<device_type_v, OF_PP_PAIR_FIRST(data_type_pair), \
                                                OF_PP_PAIR_FIRST(indices_type_pair)>>()          \
      .SetIsMatchedHob(                                                                          \
          (user_op::HobDeviceTag() == ToString(device_type_v))                                   \
          & (user_op::HobDataType("model", 0) == OF_PP_PAIR_SECOND(data_type_pair))              \
          & (user_op::HobDataType("model_diff_values", 0) == OF_PP_PAIR_SECOND(data_type_pair))  \
          & (user_op::HobDataType("model_diff_indices", 0)                                       \
             == OF_PP_PAIR_SECOND(indices_type_pair)));

OF_PP_SEQ_PRODUCT_FOR_EACH_TUPLE(REGISTER_INDEXED_SLICES_SGD_UPDATE_KERNEL, DEVICE_TYPE_SEQ,
                                 FLOATING_DATA_TYPE_SEQ, INDEX_DATA_TYPE_SEQ)

template<DeviceType device_type, typename T, typename K>
user_op::InferTmpSizeFn GenInferTmpSizeFn() {
  return [](user_op::InferContext* ctx) {
    const user_op::TensorDesc* indices = ctx->TensorDesc4ArgNameAndIndex("model_diff_indices", 0);
    const user_op::TensorDesc* values = ctx->TensorDesc4ArgNameAndIndex("model_diff_values", 0);
    const int64_t num_indices = indices->shape().elem_cnt();
    const int64_t num_values = values->shape().elem_cnt();
    TmpBufferManager<device_type, T, K> buffer_manager(nullptr, num_indices, num_values);
    return buffer_manager.GetTotalBufferSize();
  };
}

template<DeviceType device_type, typename T, typename G>
class MomentumUpdateKernel final : public user_op::OpKernel {
 public:
  MomentumUpdateKernel() = default;
  ~MomentumUpdateKernel() override = default;

 private:
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* learning_rate = ctx->Tensor4ArgNameAndIndex("learning_rate", 0);
    const user_op::Tensor* model_diff = ctx->Tensor4ArgNameAndIndex("model_diff", 0);
    user_op::Tensor* model = ctx->Tensor4ArgNameAndIndex("model", 0);
    user_op::Tensor* momentum = ctx->Tensor4ArgNameAndIndex("momentum", 0);
    const auto scale = ctx->Attr<float>("scale");
    const auto l1 = ctx->Attr<float>("l1");
    const auto l2 = ctx->Attr<float>("l2");
    const auto beta = ctx->Attr<float>("beta");
    const auto weight_decay = ctx->Attr<float>("weight_decay");
    MomentumUpdateKernelUtil<device_type, T, G>::Update(
        ctx->device_ctx(), model->shape().elem_cnt(), scale, l1, l2, beta, weight_decay,
        learning_rate->dptr<float>(), model_diff->dptr<G>(), model->mut_dptr<T>(),
        momentum->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

#define REGISTER_MOMENTUM_UPDATE_KERNEL(device, dtype, gtype)                            \
  REGISTER_USER_KERNEL("momentum_update")                                                \
      .SetCreateFn<MomentumUpdateKernel<device, dtype, gtype>>()                         \
      .SetIsMatchedHob((user_op::HobDeviceTag() == device)                               \
                       & (user_op::HobDataType("model", 0) == GetDataType<dtype>::value) \
                       & (user_op::HobDataType("model_diff", 0) == GetDataType<gtype>::value));

REGISTER_MOMENTUM_UPDATE_KERNEL(DeviceType::kCPU, float, float);
REGISTER_MOMENTUM_UPDATE_KERNEL(DeviceType::kCPU, double, double);
#ifdef WITH_CUDA
REGISTER_MOMENTUM_UPDATE_KERNEL(DeviceType::kGPU, float, float16);
REGISTER_MOMENTUM_UPDATE_KERNEL(DeviceType::kGPU, float, float);
REGISTER_MOMENTUM_UPDATE_KERNEL(DeviceType::kGPU, double, double);
#endif  // WITH_CUDA

template<DeviceType device_type, typename T, typename K>
class IndexedSlicesMomentumUpdateKernel final : public user_op::OpKernel {
 public:
  IndexedSlicesMomentumUpdateKernel() = default;
  ~IndexedSlicesMomentumUpdateKernel() override = default;

  std::shared_ptr<user_op::OpKernelState> CreateOpKernelState(
      user_op::KernelInitContext* ctx) const override {
    return CreateIndexedSlicesUpdateOpKernelState(ctx);
  }

 private:
  using ReduceSumUtilT = IndexedSlicesReduceSumKernelUtil<device_type, K, T, int32_t>;
  using MdUpdateUtilT = IndexedSlicesMomentumMdUpdateKernelUtil<device_type, T, K, int32_t>;
  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState* state) const override {
    const user_op::Tensor* learning_rate = ctx->Tensor4ArgNameAndIndex("learning_rate", 0);
    const user_op::Tensor* model_diff_indices =
        ctx->Tensor4ArgNameAndIndex("model_diff_indices", 0);
    const user_op::Tensor* model_diff_values = ctx->Tensor4ArgNameAndIndex("model_diff_values", 0);
    user_op::Tensor* model = ctx->Tensor4ArgNameAndIndex("model", 0);
    user_op::Tensor* momentum = ctx->Tensor4ArgNameAndIndex("momentum", 0);
    const auto beta = ctx->Attr<float>("beta");
    const int64_t num_indices = model_diff_indices->shape().elem_cnt();
    const int64_t num_values = model_diff_values->shape().elem_cnt();
    CHECK_EQ(num_values % num_indices, 0);
    const int64_t feature_size = num_values / num_indices;
    CHECK_EQ(feature_size, model_diff_values->shape().Count(model_diff_indices->shape().NumAxes()));
    auto* kernel_state = dynamic_cast<IndexedSlicesUpdateOpKernelState*>(state);
    CHECK_NOTNULL(kernel_state);
    CHECK_EQ(model->shape().At(0), kernel_state->upper() - kernel_state->lower());
    user_op::Tensor* tmp_buffer = ctx->Tensor4ArgNameAndIndex("tmp_buffer", 0);
    TmpBufferManager<device_type, T, K> buffer_manager(tmp_buffer->mut_dptr(), num_indices,
                                                       num_values);
    CHECK_EQ(tmp_buffer->shape().elem_cnt(), buffer_manager.GetTotalBufferSize());
    ReduceSumUtilT::ReduceSum(
        ctx->device_ctx(), num_indices, feature_size, model_diff_indices->dptr<K>(),
        model_diff_values->dptr<T>(), buffer_manager.NumUniqueDiffIndicesPtr(),
        buffer_manager.UniqueDiffIndicesPtr(), buffer_manager.UniqueDiffValuesPtr(),
        buffer_manager.UniqueWorkspacePtr(), buffer_manager.UniqueWorkspaceBytes());
    MdUpdateUtilT::Update(ctx->device_ctx(), beta, num_indices, feature_size, kernel_state->lower(),
                          kernel_state->upper(), buffer_manager.NumUniqueDiffIndicesPtr(),
                          learning_rate->dptr<float>(), buffer_manager.UniqueDiffIndicesPtr(),
                          buffer_manager.UniqueDiffValuesPtr(), model->mut_dptr<T>(),
                          momentum->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

#define REGISTER_INDEXED_SLICES_MOMENTUM_UPDATE_KERNEL(device_type_v, data_type_pair,              \
                                                       indices_type_pair)                          \
  REGISTER_USER_KERNEL("indexed_slices_momentum_update")                                           \
      .SetCreateFn<IndexedSlicesMomentumUpdateKernel<                                              \
          device_type_v, OF_PP_PAIR_FIRST(data_type_pair), OF_PP_PAIR_FIRST(indices_type_pair)>>() \
      .SetIsMatchedHob(                                                                            \
          (user_op::HobDeviceTag() == ToString(device_type_v))                                     \
          & (user_op::HobDataType("model", 0) == OF_PP_PAIR_SECOND(data_type_pair))                \
          & (user_op::HobDataType("model_diff_values", 0) == OF_PP_PAIR_SECOND(data_type_pair))    \
          & (user_op::HobDataType("model_diff_indices", 0)                                         \
             == OF_PP_PAIR_SECOND(indices_type_pair)))                                             \
      .SetInferTmpSizeFn(GenInferTmpSizeFn<device_type_v, OF_PP_PAIR_FIRST(data_type_pair),        \
                                           OF_PP_PAIR_FIRST(indices_type_pair)>());

OF_PP_SEQ_PRODUCT_FOR_EACH_TUPLE(REGISTER_INDEXED_SLICES_MOMENTUM_UPDATE_KERNEL, DEVICE_TYPE_SEQ,
                                 FLOATING_DATA_TYPE_SEQ, INDEX_DATA_TYPE_SEQ)

template<DeviceType device_type, typename T, typename G>
class AdamUpdateKernel final : public user_op::OpKernel {
 public:
  AdamUpdateKernel() = default;
  ~AdamUpdateKernel() override = default;

 private:
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* learning_rate = ctx->Tensor4ArgNameAndIndex("learning_rate", 0);
    const user_op::Tensor* model_diff = ctx->Tensor4ArgNameAndIndex("model_diff", 0);
    user_op::Tensor* model = ctx->Tensor4ArgNameAndIndex("model", 0);
    user_op::Tensor* m = ctx->Tensor4ArgNameAndIndex("m", 0);
    user_op::Tensor* v = ctx->Tensor4ArgNameAndIndex("v", 0);
    const auto scale = ctx->Attr<float>("scale");
    const auto l1 = ctx->Attr<float>("l1");
    const auto l2 = ctx->Attr<float>("l2");
    const auto beta1 = ctx->Attr<float>("beta1");
    const auto beta2 = ctx->Attr<float>("beta2");
    const auto epsilon = ctx->Attr<float>("epsilon");
    const auto do_bias_correction = ctx->Attr<bool>("do_bias_correction");
    const auto weight_decay = ctx->Attr<float>("weight_decay");
    T* beta1_t_ptr = nullptr;
    T* beta2_t_ptr = nullptr;
    if (do_bias_correction) {
      user_op::Tensor* beta1_t = ctx->Tensor4ArgNameAndIndex("beta1_t", 0);
      beta1_t_ptr = beta1_t->mut_dptr<T>();
      user_op::Tensor* beta2_t = ctx->Tensor4ArgNameAndIndex("beta2_t", 0);
      beta2_t_ptr = beta2_t->mut_dptr<T>();
    }
    AdamUpdateKernelUtil<device_type, T, G>::Update(
        ctx->device_ctx(), model->shape().elem_cnt(), scale, l1, l2, beta1, beta2, epsilon,
        do_bias_correction, weight_decay, learning_rate->dptr<float>(), model_diff->dptr<G>(),
        model->mut_dptr<T>(), m->mut_dptr<T>(), v->mut_dptr<T>(), beta1_t_ptr, beta2_t_ptr);
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

#define REGISTER_ADAM_UPDATE_KERNEL(device, dtype, gtype)                                \
  REGISTER_USER_KERNEL("adam_update")                                                    \
      .SetCreateFn<AdamUpdateKernel<device, dtype, gtype>>()                             \
      .SetIsMatchedHob((user_op::HobDeviceTag() == device)                               \
                       & (user_op::HobDataType("model", 0) == GetDataType<dtype>::value) \
                       & (user_op::HobDataType("model_diff", 0) == GetDataType<gtype>::value));

REGISTER_ADAM_UPDATE_KERNEL(DeviceType::kCPU, float, float);
REGISTER_ADAM_UPDATE_KERNEL(DeviceType::kCPU, double, double);
#ifdef WITH_CUDA
REGISTER_ADAM_UPDATE_KERNEL(DeviceType::kGPU, float, float16);
REGISTER_ADAM_UPDATE_KERNEL(DeviceType::kGPU, float, float);
REGISTER_ADAM_UPDATE_KERNEL(DeviceType::kGPU, double, double);
#endif  // WITH_CUDA

template<DeviceType device_type, typename T, typename K>
class IndexedSlicesAdamUpdateKernel final : public user_op::OpKernel {
 public:
  IndexedSlicesAdamUpdateKernel() = default;
  ~IndexedSlicesAdamUpdateKernel() override = default;
  std::shared_ptr<user_op::OpKernelState> CreateOpKernelState(
      user_op::KernelInitContext* ctx) const override {
    return CreateIndexedSlicesUpdateOpKernelState(ctx);
  }

 private:
  using ReduceSumUtilT = IndexedSlicesReduceSumKernelUtil<device_type, K, T, int32_t>;
  using MdUpdateUtilT = IndexedSlicesAdamMdUpdateKernelUtil<device_type, T, K, int32_t>;
  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState* state) const override {
    const user_op::Tensor* learning_rate = ctx->Tensor4ArgNameAndIndex("learning_rate", 0);
    const user_op::Tensor* model_diff_indices =
        ctx->Tensor4ArgNameAndIndex("model_diff_indices", 0);
    const user_op::Tensor* model_diff_values = ctx->Tensor4ArgNameAndIndex("model_diff_values", 0);
    user_op::Tensor* model = ctx->Tensor4ArgNameAndIndex("model", 0);
    user_op::Tensor* m = ctx->Tensor4ArgNameAndIndex("m", 0);
    user_op::Tensor* v = ctx->Tensor4ArgNameAndIndex("v", 0);
    const auto beta1 = ctx->Attr<float>("beta1");
    const auto beta2 = ctx->Attr<float>("beta2");
    const auto epsilon = ctx->Attr<float>("epsilon");
    const auto do_bias_correction = ctx->Attr<bool>("do_bias_correction");
    T* beta1_t_ptr = nullptr;
    T* beta2_t_ptr = nullptr;
    if (do_bias_correction) {
      user_op::Tensor* beta1_t = ctx->Tensor4ArgNameAndIndex("beta1_t", 0);
      beta1_t_ptr = beta1_t->mut_dptr<T>();
      user_op::Tensor* beta2_t = ctx->Tensor4ArgNameAndIndex("beta2_t", 0);
      beta2_t_ptr = beta2_t->mut_dptr<T>();
    }
    auto* kernel_state = dynamic_cast<IndexedSlicesUpdateOpKernelState*>(state);
    CHECK_NOTNULL(kernel_state);
    CHECK_EQ(model->shape().At(0), kernel_state->upper() - kernel_state->lower());
    const int64_t num_indices = model_diff_indices->shape().elem_cnt();
    const int64_t num_values = model_diff_values->shape().elem_cnt();
    CHECK_EQ(num_values % num_indices, 0);
    const int64_t feature_size = num_values / num_indices;
    CHECK_EQ(feature_size, model_diff_values->shape().Count(model_diff_indices->shape().NumAxes()));
    user_op::Tensor* tmp_buffer = ctx->Tensor4ArgNameAndIndex("tmp_buffer", 0);
    TmpBufferManager<device_type, T, K> buffer_manager(tmp_buffer->mut_dptr(), num_indices,
                                                       num_values);
    CHECK_EQ(tmp_buffer->shape().elem_cnt(), buffer_manager.GetTotalBufferSize());

    ReduceSumUtilT::ReduceSum(
        ctx->device_ctx(), num_indices, feature_size, model_diff_indices->dptr<K>(),
        model_diff_values->dptr<T>(), buffer_manager.NumUniqueDiffIndicesPtr(),
        buffer_manager.UniqueDiffIndicesPtr(), buffer_manager.UniqueDiffValuesPtr(),
        buffer_manager.UniqueWorkspacePtr(), buffer_manager.UniqueWorkspaceBytes());

    MdUpdateUtilT::Update(ctx->device_ctx(), beta1, beta2, epsilon, do_bias_correction, num_indices,
                          feature_size, kernel_state->lower(), kernel_state->upper(),
                          buffer_manager.NumUniqueDiffIndicesPtr(), learning_rate->dptr<float>(),
                          buffer_manager.UniqueDiffIndicesPtr(),
                          buffer_manager.UniqueDiffValuesPtr(), model->mut_dptr<T>(),
                          m->mut_dptr<T>(), v->mut_dptr<T>(), beta1_t_ptr, beta2_t_ptr);
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

#define REGISTER_INDEXED_SLICES_ADAM_UPDATE_KERNEL(device_type_v, data_type_pair,                 \
                                                   indices_type_pair)                             \
  REGISTER_USER_KERNEL("indexed_slices_adam_update")                                              \
      .SetCreateFn<IndexedSlicesAdamUpdateKernel<device_type_v, OF_PP_PAIR_FIRST(data_type_pair), \
                                                 OF_PP_PAIR_FIRST(indices_type_pair)>>()          \
      .SetIsMatchedHob(                                                                           \
          (user_op::HobDeviceTag() == ToString(device_type_v))                                    \
          & (user_op::HobDataType("model", 0) == OF_PP_PAIR_SECOND(data_type_pair))               \
          & (user_op::HobDataType("model_diff_values", 0) == OF_PP_PAIR_SECOND(data_type_pair))   \
          & (user_op::HobDataType("model_diff_indices", 0)                                        \
             == OF_PP_PAIR_SECOND(indices_type_pair)))                                            \
      .SetInferTmpSizeFn(GenInferTmpSizeFn<device_type_v, OF_PP_PAIR_FIRST(data_type_pair),       \
                                           OF_PP_PAIR_FIRST(indices_type_pair)>());

OF_PP_SEQ_PRODUCT_FOR_EACH_TUPLE(REGISTER_INDEXED_SLICES_ADAM_UPDATE_KERNEL, DEVICE_TYPE_SEQ,
                                 FLOATING_DATA_TYPE_SEQ, INDEX_DATA_TYPE_SEQ)

template<DeviceType device_type, typename T, typename G>
class LambUpdateKernel final : public user_op::OpKernel {
 public:
  LambUpdateKernel() = default;
  ~LambUpdateKernel() = default;

 private:
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* learning_rate = ctx->Tensor4ArgNameAndIndex("learning_rate", 0);
    const user_op::Tensor* model_diff = ctx->Tensor4ArgNameAndIndex("model_diff", 0);
    user_op::Tensor* model = ctx->Tensor4ArgNameAndIndex("model", 0);
    user_op::Tensor* m = ctx->Tensor4ArgNameAndIndex("m", 0);
    user_op::Tensor* v = ctx->Tensor4ArgNameAndIndex("v", 0);
    user_op::Tensor* beta1_t = ctx->Tensor4ArgNameAndIndex("beta1_t", 0);
    user_op::Tensor* beta2_t = ctx->Tensor4ArgNameAndIndex("beta2_t", 0);
    user_op::Tensor* tmp_buffer = ctx->Tensor4ArgNameAndIndex("tmp_buffer", 0);
    T* norm_buffer_ptr = tmp_buffer->mut_dptr<T>();
    T* adam_diff_ptr = reinterpret_cast<T*>(reinterpret_cast<char*>(tmp_buffer->mut_dptr()))
                       + GetCudaAlignedSize(2 * sizeof(T));
    const auto scale = ctx->Attr<float>("scale");
    const auto l1 = ctx->Attr<float>("l1");
    const auto l2 = ctx->Attr<float>("l2");
    const auto beta1 = ctx->Attr<float>("beta1");
    const auto beta2 = ctx->Attr<float>("beta2");
    const auto epsilon = ctx->Attr<float>("epsilon");
    const auto weight_decay = ctx->Attr<float>("weight_decay");
    LambUpdateKernelUtil<device_type, T, G>::Update(
        ctx->device_ctx(), m->shape().elem_cnt(), scale, l1, l2, beta1, beta2, epsilon,
        weight_decay, learning_rate->dptr<float>(), model_diff->dptr<G>(), adam_diff_ptr,
        model->mut_dptr<T>(), m->mut_dptr<T>(), v->mut_dptr<T>(), norm_buffer_ptr,
        beta1_t->mut_dptr<T>(), beta2_t->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

template<DeviceType device_type, typename T>
user_op::InferTmpSizeFn LambGenInferTmpSizeFn() {
  return [](user_op::InferContext* ctx) {
    const user_op::TensorDesc* model = ctx->TensorDesc4ArgNameAndIndex("model", 0);
    return GetCudaAlignedSize((model->shape().elem_cnt() + 2) * sizeof(T));
  };
}

#define REGISTER_LAMB_UPDATE_KERNEL(device, dtype, gtype)                                      \
  REGISTER_USER_KERNEL("lamb_update")                                                          \
      .SetCreateFn<LambUpdateKernel<device, dtype, gtype>>()                                   \
      .SetIsMatchedHob((user_op::HobDeviceTag() == device)                                     \
                       & (user_op::HobDataType("model", 0) == GetDataType<dtype>::value)       \
                       & (user_op::HobDataType("model_diff", 0) == GetDataType<gtype>::value)) \
      .SetInferTmpSizeFn(LambGenInferTmpSizeFn<device, dtype>());

REGISTER_LAMB_UPDATE_KERNEL(DeviceType::kCPU, float, float);
REGISTER_LAMB_UPDATE_KERNEL(DeviceType::kCPU, double, double);
#ifdef WITH_CUDA
REGISTER_LAMB_UPDATE_KERNEL(DeviceType::kGPU, float, float);
REGISTER_LAMB_UPDATE_KERNEL(DeviceType::kGPU, double, double);
#endif  // WITH_CUDA

}  // namespace

}  // namespace oneflow
