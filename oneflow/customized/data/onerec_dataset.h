#ifndef ONEFLOW_CUSTOMIZED_DATA_ONEREC_DATASET_H_
#define ONEFLOW_CUSTOMIZED_DATA_ONEREC_DATASET_H_

#include "oneflow/core/common/blocking_counter.h"
#include "oneflow/customized/data/dataset.h"
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/common/str_util.h"
#include "oneflow/core/framework/op_kernel.h"
#include "oneflow/core/persistence/persistent_in_stream.h"
#include "oneflow/core/job/job_set.pb.h"

#define XXH_NAMESPACE LZ4_
#include <xxhash.h>

namespace oneflow {

namespace {

constexpr int64_t kMaxPayloadSize = std::numeric_limits<int32_t>::max();
constexpr int64_t kMagicNumber = 0x24434552454E4F5E;  // '^ONEREC$', little endian
constexpr int32_t kReservedNumber = 0;
constexpr int32_t kPayloadAlignmentSize = 8;
constexpr int32_t kMagicFieldSize = 8;
constexpr int32_t kReservedFieldSize = 4;
constexpr int32_t kPayloadSizeFieldSize = 4;
constexpr int32_t kDigestFieldSize = 8;
constexpr int32_t kHeaderSizeWithoutDigest =
    kMagicFieldSize + kReservedFieldSize + kPayloadSizeFieldSize;
constexpr int32_t kHeaderSize = kHeaderSizeWithoutDigest + kDigestFieldSize;

inline XXH64_hash_t ByteSwap(XXH64_hash_t x) {
  return ((x & 0xff00000000000000ull) >> 56u) | ((x & 0x00ff000000000000ull) >> 40u)
         | ((x & 0x0000ff0000000000ull) >> 24u) | ((x & 0x000000ff00000000ull) >> 8u)
         | ((x & 0x00000000ff000000ull) << 8u) | ((x & 0x0000000000ff0000ull) << 24u)
         | ((x & 0x000000000000ff00ull) << 40u) | ((x & 0x00000000000000ffull) << 56u);
}

struct OneRecFrameHeader {
  int64_t magic;
  int32_t reserved;
  int32_t payload_size;
  XXH64_hash_t digest;
};

union OneRecFrameHeaderView {
  char raw[kHeaderSize];
  OneRecFrameHeader header;
};

union OneRecFrameFooterView {
  char raw[kDigestFieldSize];
  XXH64_hash_t digest;
};

}  // namespace

namespace data {

class OneRecDataset final : public Dataset<TensorBuffer> {
 public:
  using LoadTargetPtr = std::shared_ptr<TensorBuffer>;
  using LoadTargetPtrList = std::vector<LoadTargetPtr>;
  OF_DISALLOW_COPY_AND_MOVE(OneRecDataset);
  OneRecDataset(user_op::KernelInitContext* ctx) {
    current_epoch_ = 0;
    shuffle_after_epoch_ = ctx->Attr<bool>("shuffle_after_epoch");
    data_file_paths_ = ctx->Attr<std::vector<std::string>>("files");
    parallel_id_ = ctx->parallel_ctx().parallel_id();
    parallel_num_ = ctx->parallel_ctx().parallel_num();
    BalancedSplitter bs(data_file_paths_.size(), parallel_num_);
    range_ = bs.At(parallel_id_);
    ResetInstream();
  }

  ~OneRecDataset() = default;

  LoadTargetPtrList Next() override {
    LoadTargetPtrList ret;
    LoadTargetPtr sample_ptr(new TensorBuffer());
    ReadSample(*sample_ptr);
    ret.push_back(std::move(sample_ptr));
    return ret;
  }

 private:
  void ReadSample(TensorBuffer& tensor) {
    static_assert(sizeof(OneRecFrameHeader) == kHeaderSize, "");
    OneRecFrameHeaderView header_view{};
    static_assert(sizeof(header_view.header) == kHeaderSize, "");
    if (in_stream_->ReadFully(header_view.raw, kHeaderSize) != 0) {
      ResetInstream();
      current_epoch_++;
      CHECK_EQ(in_stream_->ReadFully(header_view.raw, kHeaderSize), 0);
    }
    CHECK_EQ(header_view.header.magic, kMagicNumber);
    CHECK_EQ(header_view.header.reserved, kReservedNumber);
    const int32_t payload_size = header_view.header.payload_size;
    CHECK_GE(payload_size, 0);
    CHECK_LE(payload_size, kMaxPayloadSize);
    XXH64_state_t* const state = LZ4_XXH64_createState();
    CHECK_NOTNULL(state);
    XXH64_hash_t const seed = 0;
    CHECK_NE(LZ4_XXH64_reset(state, seed), XXH_ERROR);
    CHECK_NE(XXH64_update(state, header_view.raw, kHeaderSizeWithoutDigest), XXH_ERROR);
    CHECK_EQ(ByteSwap(header_view.header.digest), LZ4_XXH64_digest(state));
    const int32_t padded_size = RoundUp(payload_size, kPayloadAlignmentSize);
    const int32_t body_size = padded_size + kDigestFieldSize;

    char* body = reinterpret_cast<char*>(malloc(body_size));
    // tensor.Resize(Shape({body_size}), DataType::kChar);
    // char* body = tensor.mut_data<char>();

    CHECK_EQ(in_stream_->ReadFully(body, body_size), 0);
    static_assert(sizeof(OneRecFrameFooterView) == kDigestFieldSize, "");
    OneRecFrameFooterView footer_view{};
    std::memcpy(&footer_view, body + padded_size, sizeof(OneRecFrameFooterView));
    CHECK_NE(XXH64_reset(state, seed), XXH_ERROR);
    CHECK_NE(LZ4_XXH64_update(state, body, payload_size), XXH_ERROR);
    CHECK_EQ(ByteSwap(footer_view.digest), LZ4_XXH64_digest(state));
    CHECK_NE(LZ4_XXH64_freeState(state), XXH_ERROR);

    // tensor.Resize(Shape({payload_size}), DataType::kChar);
    tensor.Resize(Shape({payload_size}), DataType::kChar);
    std::memcpy(tensor.mut_data(), body, payload_size);
    LOG(INFO) << "payload_size" << payload_size << "body_size" << body_size;
  }

  void ResetInstream() {
    if (shuffle_after_epoch_) {
      std::mt19937 g(kOneflowDatasetSeed + current_epoch_);
      std::shuffle(data_file_paths_.begin(), data_file_paths_.end(), g);
    }
    std::vector<std::string> file_paths = GetLocalFilePaths();
    in_stream_.reset(new PersistentInStream(DataFS(), file_paths, false, false));
  }

  std::vector<std::string> GetLocalFilePaths() {
    std::vector<std::string> ret;
    for (int i = range_.begin(); i < range_.end(); ++i) { ret.push_back(data_file_paths_.at(i)); }
    return ret;
  }

  int32_t current_epoch_;
  bool shuffle_after_epoch_;

  int32_t parallel_id_;
  int32_t parallel_num_;
  Range range_;
  std::vector<std::string> data_file_paths_;
  std::unique_ptr<PersistentInStream> in_stream_;
};

}  // namespace data
}  // namespace oneflow

#endif  // ONEFLOW_CUSTOMIZED_DATA_ONEREC_DATASET_H_