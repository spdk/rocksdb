//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/blob_log_writer.h"

#include <stdint.h>
#include "rocksdb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/file_reader_writer.h"

namespace rocksdb {
namespace blob_log {


Writer::Writer(unique_ptr<WritableFileWriter>&& dest,
               uint64_t log_number, uint64_t bpsync,
               bool use_fs)
    : dest_(std::move(dest)),
      log_number_(log_number),
      block_offset_(0), bytes_per_sync_(bpsync),
      next_sync_offset_(0), use_fsync_(use_fs) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc_[i] = crc32c::Value(&t, 1);
  }
}

Writer::~Writer() {
}


Status Writer::WriteHeader(blob_log::BlobLogHeader& header)
{
  std::string str;
  header.EncodeTo(&str);
  
  Status s = dest_->Append(Slice(str));
  if (s.ok()) {
    block_offset_ += str.size();
    s = dest_->Flush();
  }
  return s;
}

Status Writer::AppendFooter(blob_log::BlobLogFooter& footer)
{
  std::string str;
  footer.EncodeTo(&str);
   
  Status s = dest_->Append(Slice(str));
  if (s.ok())
  {
    block_offset_ += str.size();
    s = dest_->Close();
    dest_.reset();
  }

  return s;
}

Status Writer::AddRecord(const Slice& key, const Slice& val,
  uint64_t& key_offset, uint64_t& blob_offset, uint32_t ttl)
{
  RecordType type = kFullType;
  Status s = EmitPhysicalRecord(type, kTTLType, key, val,
    key_offset, blob_offset, ttl);
  return s;
}

Status Writer::AddRecord(const Slice& key, const Slice& val,
  uint64_t& key_offset, uint64_t& blob_offset) {
  RecordType type = kFullType;
  Status s = EmitPhysicalRecord(type, kRegularType, key, val, key_offset,
    blob_offset, -1, -1);
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, RecordSubType st, 
  const Slice& key, const Slice& val,
  uint64_t& key_offset, uint64_t& blob_offset,
  int32_t ttl, int64_t ts) {

  char buf[kHeaderSize];

  uint32_t offset = 4;
  // Format the header
  EncodeFixed32(buf+offset, key.size());
  offset += 4;
  EncodeFixed64(buf+offset, val.size());
  offset += 8;
  if (ttl != -1) {
    EncodeFixed32(buf+offset, (uint32_t)ttl);
  }
  offset += 4;
  if (ts != -1) {
    EncodeFixed64(buf+offset, (uint64_t)ts);
  }
  offset += 8;

  buf[offset] = static_cast<char>(t);
  offset++;
  buf[offset] = static_cast<char>(st);

  uint32_t crc = 0;
  // Compute the crc of the record type and the payload.
  crc = crc32c::Extend(crc, val.data(), val.size());
  crc = crc32c::Mask(crc);  // Adjust for storage
  EncodeFixed32(buf, crc);

  // Write the header and the payload
  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(key);
    if (s.ok()) {
      s = dest_->Append(val);
      if (s.ok()) {
        s = dest_->Flush();
      }
    }
  }
 
  key_offset = block_offset_ + kHeaderSize;
  blob_offset = key_offset + key.size();
  block_offset_ += kHeaderSize + key.size() + val.size();

  if (block_offset_ > next_sync_offset_) {
    dest_->Sync(true);
    next_sync_offset_ += bytes_per_sync_;
  }

  return s;
}

}  // namespace blob_log
}  // namespace rocksdb
