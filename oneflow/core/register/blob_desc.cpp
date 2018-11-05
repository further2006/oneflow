#include "oneflow/core/register/blob_desc.h"
#include "oneflow/core/register/runtime_blob_desc.h"
#include "oneflow/core/job/job_desc.h"

namespace oneflow {

BlobDesc::BlobDesc()
    : BlobDesc(Shape(), Global<JobDesc>::Get()->DefaultDataType(), false, false, 1) {}

BlobDesc::BlobDesc(const Shape& shape, DataType data_type, bool has_data_id, bool has_col_num,
                   int32_t max_col_num)
    : header_is_opaque_(false),
      has_data_id_(has_data_id),
      has_col_num_(has_col_num),
      has_dim0_valid_num_(false),
      has_dim1_valid_num_(false),
      has_dim2_valid_num_(false),
      has_record_id_in_device_piece_(false),
      max_col_num_(max_col_num),
      blob_mem_id_(-1),
      body_field_(shape, data_type) {}

BlobDesc::BlobDesc(const BlobDesc& BlobDesc) { *this = BlobDesc; }

void BlobDesc::InitFromProto(const BlobDescProto& proto) {
  body_field_.InitFromProto(proto.body());
  max_col_num_ = proto.header().max_col_num();
  blob_mem_id_ = proto.header().blob_mem_id();
  header_pod_desc_.InitFromProto(proto.header().header_pod_desc());
  if (proto.header().has_opaque_header()) {
    header_is_opaque_ = true;
    has_data_id_ = false;
    has_col_num_ = false;
    has_dim0_valid_num_ = false;
    has_dim1_valid_num_ = false;
    has_dim2_valid_num_ = false;
    has_record_id_in_device_piece_ = false;
    opaque_header_ = FieldDesc(proto.header().opaque_header());
  } else {
    CHECK(proto.header().has_field_header());
    header_is_opaque_ = false;
    has_data_id_ = header_pod_desc_.HasField(FieldKey::kDataId);
    has_col_num_ = header_pod_desc_.HasField(FieldKey::kColNum);
    has_dim0_valid_num_ = header_pod_desc_.HasField(FieldKey::kDim0ValidNum);
    has_dim1_valid_num_ = header_pod_desc_.HasField(FieldKey::kDim1ValidNum);
    has_dim2_valid_num_ = header_pod_desc_.HasField(FieldKey::kDim2ValidNum);
    has_record_id_in_device_piece_ = header_pod_desc_.HasField(FieldKey::kRecordIdInDevicePiece);
  }
  if (proto.has_dim0_inner_shape()) {
    dim0_inner_shape_.reset(new Shape(proto.dim0_inner_shape()));
  }
}

BlobDesc::BlobDesc(const StructPodDesc& header_pod_desc, int64_t header_byte_size,
                   const Shape& shape, DataType data_type, int32_t max_col_num)
    : has_data_id_(false),
      has_col_num_(false),
      has_dim0_valid_num_(false),
      has_dim1_valid_num_(false),
      has_dim2_valid_num_(false),
      has_record_id_in_device_piece_(false),
      max_col_num_(max_col_num),
      blob_mem_id_(-1),
      body_field_(shape, data_type) {
  CHECK_EQ(header_pod_desc.ByteSize(), header_byte_size);
  if (header_byte_size > 0) {
    header_is_opaque_ = true;
    opaque_header_ = FieldDesc(Shape({header_byte_size}), DataType::kChar);
    header_pod_desc_ = header_pod_desc;
  } else {
    header_is_opaque_ = false;
  }
}

template<FieldKey field_key>
bool BlobDesc::HasField() const {
  switch (field_key) {
    case FieldKey::kDataId: return has_data_id_;
    case FieldKey::kColNum: return has_col_num_;
    case FieldKey::kDim0ValidNum: return has_dim0_valid_num_;
    case FieldKey::kDim1ValidNum: return has_dim1_valid_num_;
    case FieldKey::kDim2ValidNum: return has_dim2_valid_num_;
    case FieldKey::kRecordIdInDevicePiece: return has_record_id_in_device_piece_;
    default: UNIMPLEMENTED();
  }
  return false;
}

template<FieldKey field_key>
void BlobDesc::SetHasField(bool val) {
  CHECK(!header_is_opaque_);
  switch (field_key) {
    case FieldKey::kDataId: has_data_id_ = val; return;
    case FieldKey::kColNum: has_col_num_ = val; return;
    case FieldKey::kDim0ValidNum: has_dim0_valid_num_ = val; return;
    case FieldKey::kDim1ValidNum: has_dim1_valid_num_ = val; return;
    case FieldKey::kDim2ValidNum: has_dim2_valid_num_ = val; return;
    case FieldKey::kRecordIdInDevicePiece: has_record_id_in_device_piece_ = val; return;
    default: UNIMPLEMENTED();
  }
}

Shape& BlobDesc::mut_dim0_inner_shape() {
  CHECK(!header_is_opaque_);
  if (!dim0_inner_shape_) { dim0_inner_shape_.reset(new Shape()); }
  return *dim0_inner_shape_;
}

void BlobDesc::DataIdFieldToProto(FieldHeaderDesc* proto, StructPodDesc* header_pod_desc) const {
  Shape shape(
      {body_field_.shape().At(0), static_cast<int64_t>(Global<JobDesc>::Get()->SizeOfOneDataId())});
  FieldDesc data_id_field(shape, DataType::kChar);
  data_id_field.ToProto(proto->mutable_data_id());
  header_pod_desc->AddField(FieldKey::kDataId, TensorPodDesc(shape, DataType::kChar));
}

void BlobDesc::ColNumFieldToProto(FieldHeaderDesc* proto, StructPodDesc* header_pod_desc) const {
  Shape shape({body_field_.shape().At(0)});
  FieldDesc col_num_field(shape, DataType::kInt32);
  col_num_field.ToProto(proto->mutable_col_num());
  header_pod_desc->AddField(FieldKey::kColNum, TensorPodDesc(shape, DataType::kInt32));
}
void BlobDesc::Dim0ValidNumToProto(StructPodDesc* header_pod_desc) const {
  CHECK(dim0_inner_shape_);
  CHECK_EQ(dim0_inner_shape_->elem_cnt(), body_field_.shape().At(0));
  Shape shape({dim0_inner_shape_->At(0)});
  header_pod_desc->AddField(FieldKey::kDim0ValidNum, TensorPodDesc(shape, DataType::kInt64));
}

void BlobDesc::Dim1ValidNumToProto(StructPodDesc* header_pod_desc) const {
  Shape shape({body_field_.shape().At(0)});
  header_pod_desc->AddField(FieldKey::kDim1ValidNum, TensorPodDesc(shape, DataType::kInt64));
}

void BlobDesc::Dim2ValidNumToProto(StructPodDesc* header_pod_desc) const {
  Shape shape({body_field_.shape().At(0), body_field_.shape().At(1)});
  header_pod_desc->AddField(FieldKey::kDim2ValidNum, TensorPodDesc(shape, DataType::kInt64));
}

void BlobDesc::RecordIdInDevicePieceToProto(StructPodDesc* header_pod_desc) const {
  Shape shape({body_field_.shape().At(0)});
  header_pod_desc->AddField(FieldKey::kRecordIdInDevicePiece,
                            TensorPodDesc(shape, DataType::kInt64));
}

void BlobDesc::HeaderToProto(BlobDescProto* proto) const {
  proto->mutable_header()->set_max_col_num(max_col_num_);
  proto->mutable_header()->set_blob_mem_id(blob_mem_id_);
  if (!header_is_opaque_) {
    FieldHeaderDesc* field_header = proto->mutable_header()->mutable_field_header();
    StructPodDesc header_pod_desc;
    if (HasField<FieldKey::kDataId>()) { DataIdFieldToProto(field_header, &header_pod_desc); }
    if (HasField<FieldKey::kColNum>()) { ColNumFieldToProto(field_header, &header_pod_desc); }
    if (HasField<FieldKey::kDim0ValidNum>()) { Dim0ValidNumToProto(&header_pod_desc); }
    if (HasField<FieldKey::kDim1ValidNum>()) { Dim1ValidNumToProto(&header_pod_desc); }
    if (HasField<FieldKey::kDim2ValidNum>()) { Dim2ValidNumToProto(&header_pod_desc); }
    if (HasField<FieldKey::kRecordIdInDevicePiece>()) {
      RecordIdInDevicePieceToProto(&header_pod_desc);
    }
    header_pod_desc.ToProto(proto->mutable_header()->mutable_header_pod_desc());
  } else {
    opaque_header_.ToProto(proto->mutable_header()->mutable_opaque_header());
    header_pod_desc_.ToProto(proto->mutable_header()->mutable_header_pod_desc());
  }
}

void BlobDesc::ToProto(BlobDescProto* proto) const {
  HeaderToProto(proto);
  body_field_.ToProto(proto->mutable_body());
  if (dim0_inner_shape_) { dim0_inner_shape_->ToProto(proto->mutable_dim0_inner_shape()); }
}

bool BlobDesc::operator==(const BlobDesc& rhs) const {
  return header_is_opaque_ == rhs.header_is_opaque_ && opaque_header_ == rhs.opaque_header_
         && header_pod_desc_ == rhs.header_pod_desc_ && has_data_id_ == rhs.has_data_id_
         && has_col_num_ == rhs.has_col_num_ && has_dim0_valid_num_ == rhs.has_dim0_valid_num_
         && has_dim1_valid_num_ == rhs.has_dim1_valid_num_
         && has_dim2_valid_num_ == rhs.has_dim2_valid_num_
         && has_record_id_in_device_piece_ == rhs.has_record_id_in_device_piece_
         && max_col_num_ == rhs.max_col_num_ && blob_mem_id_ == rhs.blob_mem_id_
         && body_field_ == rhs.body_field_;
}

BlobDesc& BlobDesc::operator=(const BlobDesc& blob_desc) {
  BlobDescProto proto;
  blob_desc.ToProto(&proto);
  InitFromProto(proto);
  return *this;
}

std::unique_ptr<BlobDesc> ComputePackedBlobDesc(
    const HashMap<LogicalBlobId, std::unique_ptr<BlobDesc>>& lbi2blob_desc) {
  int64_t header_byte_size = 0;
  int64_t body_byte_size = 0;
  HashSet<int> data_type_set;
  int32_t max_col_num = -1;
  int32_t blob_desc_cnt = 0;
  std::unique_ptr<BlobDesc> ret(new BlobDesc());
  const BlobDesc* last_blob_desc = nullptr;
  HashMap<int32_t, size_t> blob_mem_id2size;
  StructPodDesc opaque_header_pod_desc;
  for (auto& pair : lbi2blob_desc) {
    BlobDesc* blob_desc = pair.second.get();
    RtBlobDesc rt_blob_desc(*blob_desc);
    header_byte_size += rt_blob_desc.ByteSizeOfBlobHeader();
    *opaque_header_pod_desc.MutStructField(NewFieldId(pair.first)) = rt_blob_desc.header_pod_desc();
    int64_t cur_body_byte_size = rt_blob_desc.ByteSizeOfBlobBody();
    int32_t blob_mem_id = blob_desc->blob_mem_id();
    if (blob_mem_id == -1) {
      body_byte_size += cur_body_byte_size;
    } else {
      auto size_it = blob_mem_id2size.find(blob_mem_id);
      if (size_it == blob_mem_id2size.end()) {
        CHECK(blob_mem_id2size.emplace(blob_mem_id, cur_body_byte_size).second);
      } else {
        CHECK_EQ(size_it->second, cur_body_byte_size);
      }
    }
    data_type_set.insert(static_cast<int>(blob_desc->data_type()));
    if (max_col_num == -1) {
      max_col_num = blob_desc->max_col_num();
    } else {
      CHECK_EQ(max_col_num, blob_desc->max_col_num());
    }
    blob_desc_cnt += 1;
    last_blob_desc = blob_desc;
  }
  for (auto& pair : blob_mem_id2size) { body_byte_size += pair.second; }
  if (blob_desc_cnt == 0) {
    // do nothing
  } else if (blob_desc_cnt == 1) {
    ret.reset(new BlobDesc(*last_blob_desc));
  } else if (data_type_set.size() == 1) {
    DataType sole_data_type = static_cast<DataType>(*(data_type_set.begin()));
    int64_t size_of_one_elem = GetSizeOfDataType(sole_data_type);
    CHECK_EQ(body_byte_size % size_of_one_elem, 0);
    int64_t total_elem_cnt = body_byte_size / size_of_one_elem;
    if (header_byte_size == 0) {
      ret.reset(new BlobDesc(Shape({total_elem_cnt}), sole_data_type, false, false, max_col_num));
    } else {
      ret.reset(new BlobDesc(opaque_header_pod_desc, header_byte_size, Shape({total_elem_cnt}),
                             sole_data_type, max_col_num));
    }
  } else {
    ret.reset(new BlobDesc(opaque_header_pod_desc, header_byte_size, Shape({body_byte_size}),
                           DataType::kChar, max_col_num));
  }
  return ret;
}

#define MAKE_HAS_FIELD_ENTRY(field_key)                \
  template bool BlobDesc::HasField<field_key>() const; \
  template void BlobDesc::SetHasField<field_key>(bool);
OF_PP_FOR_EACH_TUPLE(MAKE_HAS_FIELD_ENTRY, FIELD_KEY_SEQ);

}  // namespace oneflow
