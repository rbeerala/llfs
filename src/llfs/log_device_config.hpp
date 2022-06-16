//#=##=##=#==#=#==#===#+==#+==========+==+=+=+=+=+=++=+++=+++++=-++++=-+++++++++++
//
// Part of the LLFS Project, under Apache License v2.0.
// See https://www.apache.org/licenses/LICENSE-2.0 for license information.
// SPDX short identifier: Apache-2.0
//
//+++++++++++-+-+--+----- --- -- -  -  -   -

#pragma once
#ifndef LLFS_PACKED_LOG_DEVICE_CONFIG_HPP
#define LLFS_PACKED_LOG_DEVICE_CONFIG_HPP

#include <llfs/constants.hpp>
#include <llfs/int_types.hpp>
#include <llfs/optional.hpp>
#include <llfs/packed_config.hpp>
#include <llfs/storage_file_builder.hpp>

#include <batteries/static_assert.hpp>

#include <boost/uuid/uuid.hpp>

namespace llfs {

struct PackedLogDeviceConfig;

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
//
struct LogDeviceConfigOptions {
  using PackedConfigType = PackedLogDeviceConfig;

  // The capacity in bytes of the log.
  //
  usize log_size;

  // The unique identifier for the log; if None, a random UUID will be generated.
  //
  Optional<boost::uuids::uuid> uuid;

  // log2 of the number of 4kib (memory) pages per flush block.
  // Higher values == higher (better) throughput, higher (worse) latency.
  //
  Optional<u16> pages_per_block_log2;
};

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
//
struct PackedLogDeviceConfig {
  static constexpr usize kSize = PackedConfigSlot::kSize;

  // Must be PackedConfigSlot::Tag::kLogDevice.
  //
  little_u32 tag;

  // Reserved for future use (set to 0 for now).
  //
  u8 pad0_[2];

  // The log2 of the number of 4096-byte pages per flush block.
  //
  little_u16 pages_per_block_log2;

  // The offset of flush block 0 relative to this structure.
  //
  little_i64 block_0_offset;

  // The total size of the log in bytes.
  //
  little_u64 physical_size;

  // The logical size of the log; this excludes all block headers.
  //
  little_u64 logical_size;

  // The uuid for this log.
  //
  boost::uuids::uuid uuid;

  // Reserved for future use (set to 0 for now).
  //
  u8 pad1_[16];

  //+++++++++++-+-+--+----- --- -- -  -  -   -

  usize pages_per_block() const
  {
    return usize{1} << pages_per_block_log2;
  }

  usize block_size() const
  {
    return 4 * kKiB * this->pages_per_block();
  }
};

BATT_STATIC_ASSERT_EQ(sizeof(PackedLogDeviceConfig), PackedLogDeviceConfig::kSize);

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

template <>
struct PackedConfigTagFor<PackedLogDeviceConfig> {
  static constexpr u32 value = PackedConfigSlot::Tag::kLogDevice;
};

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

std::ostream& operator<<(std::ostream& out, const PackedLogDeviceConfig& t);

Status configure_storage_object(StorageFileBuilder::Transaction&,
                                FileOffsetPtr<PackedLogDeviceConfig&> p_config,
                                const LogDeviceConfigOptions& options);

}  // namespace llfs

#endif  // LLFS_PACKED_LOG_DEVICE_CONFIG_HPP