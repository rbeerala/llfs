//#=##=##=#==#=#==#===#+==#+==========+==+=+=+=+=+=++=+++=+++++=-++++=-+++++++++++
//
// Part of the LLFS Project, under Apache License v2.0.
// See https://www.apache.org/licenses/LICENSE-2.0 for license information.
// SPDX short identifier: Apache-2.0
//
//+++++++++++-+-+--+----- --- -- -  -  -   -

#include <llfs/volume.hpp>
//

#include <llfs/volume_reader.hpp>
#include <llfs/volume_recovery_visitor.hpp>

#include <boost/uuid/random_generator.hpp>

namespace llfs {

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
/*static*/ auto Volume::recover(VolumeRecoverParams&& params,
                                const VolumeReader::SlotVisitorFn& slot_visitor_fn)
    -> StatusOr<std::unique_ptr<Volume>>
{
  batt::TaskScheduler& scheduler = *params.scheduler;
  const VolumeOptions& options = params.options;
  batt::SharedPtr<PageCache> cache = params.cache;
  LogDeviceFactory& root_log_factory = *params.root_log_factory;
  LogDeviceFactory& recycler_log_factory = *params.recycler_log_factory;

  if (!params.trim_control) {
    params.trim_control = std::make_unique<SlotLockManager>();
  }

  auto page_deleter = std::make_unique<PageCache::PageDeleterImpl>(*cache);

  BATT_ASSIGN_OK_RESULT(
      std::unique_ptr<PageRecycler> recycler,
      PageRecycler::recover(scheduler, options.name + "_PageRecycler", options.max_refs_per_page,
                            *page_deleter, recycler_log_factory));

  VolumePendingJobsMap pending_jobs;
  VolumeRecoveryVisitor visitor{batt::make_copy(slot_visitor_fn), pending_jobs};

  // Open the log device and scan all slots.
  //
  BATT_ASSIGN_OK_RESULT(std::unique_ptr<LogDevice> root_log,
                        root_log_factory.open_log_device(
                            [&](LogDevice::Reader& log_reader) -> StatusOr<slot_offset_type> {
                              TypedSlotReader<VolumeEventVariant> slot_reader{log_reader};

                              StatusOr<usize> slots_read =
                                  slot_reader.run(batt::WaitForResource::kFalse,
                                                  [&visitor](auto&&... args) -> Status {
                                                    return visitor(BATT_FORWARD(args)...).status();
                                                  });
                              BATT_UNTESTED_COND(!slots_read.ok());
                              BATT_REQUIRE_OK(slots_read);

                              return log_reader.slot_offset();
                            }));

  // Put the main log in a clean state.  This means all configuration data must be recorded, device
  // attachments created, and pending jobs resolved.
  {
    TypedSlotWriter<VolumeEventVariant> slot_writer{*root_log};
    batt::Grant grant = BATT_OK_RESULT_OR_PANIC(
        slot_writer.reserve(slot_writer.pool_size(), batt::WaitForResource::kFalse));

    // If no uuids were found while opening the log, create them now.
    //
    if (!visitor.ids) {
      VLOG(1) << "Initializing Volume uuids for the first time";

      visitor.ids.emplace(SlotWithPayload<PackedVolumeIds>{
          .slot_range = {0, 1},
          .payload =
              {
                  .main_uuid = options.uuid.value_or(boost::uuids::random_generator{}()),
                  .recycler_uuid = recycler->uuid(),
                  .trimmer_uuid = boost::uuids::random_generator{}(),
              },
      });

      StatusOr<SlotRange> ids_slot = slot_writer.append(grant, visitor.ids->payload);

      BATT_UNTESTED_COND(!ids_slot.ok());
      BATT_REQUIRE_OK(ids_slot);

      Status flush_status =
          slot_writer.sync(LogReadMode::kDurable, SlotUpperBoundAt{ids_slot->upper_bound});

      BATT_UNTESTED_COND(!flush_status.ok());
      BATT_REQUIRE_OK(flush_status);
    }
    VLOG(1) << BATT_INSPECT(visitor.ids->payload);

    // Attach the main uuid, recycler uuid, and trimmer uuid to each device in the cache storage
    // pool.
    //
    {
      // Loop through all combinations of uuid, device_id.
      //
      VLOG(1) << "Recovered attachments: " << batt::dump_range(visitor.device_attachments);
      for (const auto& uuid : {
               visitor.ids->payload.main_uuid,
               visitor.ids->payload.recycler_uuid,
               visitor.ids->payload.trimmer_uuid,
           }) {
        for (const PageArena& arena : cache->all_arenas()) {
          auto attach_event = PackedVolumeAttachEvent{{
              .client_uuid = uuid,
              .device_id = arena.device().get_id(),
          }};

          if (visitor.device_attachments.count(attach_event)) {
            continue;
          }

          VLOG(1) << "[Volume::recover] attaching client " << uuid << " to device "
                  << arena.device().get_id();

          StatusOr<slot_offset_type> sync_slot =
              arena.allocator().attach_user(uuid, /*user_slot=*/0u);

          BATT_UNTESTED_COND(!sync_slot.ok());
          BATT_REQUIRE_OK(sync_slot);

          Status sync_status = arena.allocator().sync(*sync_slot);

          BATT_UNTESTED_COND(!sync_status.ok());
          BATT_REQUIRE_OK(sync_status);

          StatusOr<SlotRange> ids_slot = slot_writer.append(grant, attach_event);

          BATT_UNTESTED_COND(!ids_slot.ok());
          BATT_REQUIRE_OK(ids_slot);

          Status flush_status =
              slot_writer.sync(LogReadMode::kDurable, SlotUpperBoundAt{ids_slot->upper_bound});

          BATT_UNTESTED_COND(!flush_status.ok());
          BATT_REQUIRE_OK(flush_status);
        }
      }
    }

    // Resolve any jobs with a PrepareJob slot but no CommitJob or RollbackJob.
    //
    Status jobs_resolved = visitor.resolve_pending_jobs(
        *cache, *recycler, /*volume_uuid=*/visitor.ids->payload.main_uuid, slot_writer, grant);

    BATT_UNTESTED_COND(!jobs_resolved.ok());
    BATT_REQUIRE_OK(jobs_resolved);
  }

  std::unique_ptr<Volume> volume{
      new Volume{params.options, visitor.ids->payload.main_uuid, std::move(cache),
                 std::move(params.trim_control), std::move(page_deleter), std::move(root_log),
                 std::move(recycler), visitor.ids->payload.trimmer_uuid}};

  volume->start();

  return volume;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
/*explicit*/ Volume::Volume(const VolumeOptions& options, const boost::uuids::uuid& volume_uuid,
                            batt::SharedPtr<PageCache>&& page_cache,
                            std::unique_ptr<SlotLockManager>&& trim_control,
                            std::unique_ptr<PageCache::PageDeleterImpl>&& page_deleter,
                            std::unique_ptr<LogDevice>&& root_log,
                            std::unique_ptr<PageRecycler>&& recycler,
                            const boost::uuids::uuid& trimmer_uuid) noexcept
    : options_{options}
    , volume_uuid_{volume_uuid}
    , cache_{std::move(page_cache)}
    , trim_control_{std::move(trim_control)}
    , page_deleter_{std::move(page_deleter)}
    , root_log_{std::move(root_log)}
    , trim_lock_{BATT_OK_RESULT_OR_PANIC(this->trim_control_->lock_slots(
          this->root_log_->slot_range(LogReadMode::kDurable), "Volume::(ctor)"))}
    , recycler_{std::move(recycler)}
    , slot_writer_{*this->root_log_}
    , trimmer_{this->cache(),
               *this->trim_control_,
               *this->recycler_,
               trimmer_uuid,
               this->root_log_->new_reader(/*slot_lower_bound=*/None, LogReadMode::kDurable),
               this->slot_writer_}
{
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
Status Volume::trim(slot_offset_type slot_lower_bound)
{
  return this->trim_lock_.with_lock([slot_lower_bound, this](SlotReadLock& trim_lock) -> Status {
    SlotRange target_range = trim_lock.slot_range();
    target_range.lower_bound = slot_max(target_range.lower_bound, slot_lower_bound);

    BATT_ASSIGN_OK_RESULT(trim_lock, this->trim_control_->update_lock(
                                         std::move(trim_lock), target_range, "Volume::trim"));

    return OkStatus();
  });
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
StatusOr<SlotRange> Volume::sync(LogReadMode mode, SlotUpperBoundAt event)
{
  Status status = this->root_log_->sync(mode, event);
  BATT_REQUIRE_OK(status);

  return this->root_log_->slot_range(mode);
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
StatusOr<SlotReadLock> Volume::lock_slots(const SlotRangeSpec& slot_range_spec, LogReadMode mode)
{
  const SlotRange slot_range = [&] {
    if (slot_range_spec.lower_bound == None || slot_range_spec.upper_bound == None) {
      const SlotRange default_range = this->root_log_->slot_range(mode);
      return SlotRange{slot_range_spec.lower_bound.value_or(default_range.lower_bound),
                       slot_range_spec.upper_bound.value_or(default_range.upper_bound)};
    } else {
      return SlotRange{*slot_range_spec.lower_bound, *slot_range_spec.upper_bound};
    }
  }();

  return this->trim_control_->lock_slots(slot_range, "Volume::lock_slots");
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
Volume::~Volume() noexcept
{
  this->root_log_->flush().IgnoreError();
  this->halt();
  this->join();
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void Volume::start()
{
  if (this->recycler_) {
    this->recycler_->start();
  }

  if (!this->trimmer_task_) {
    this->trimmer_task_.emplace(/*executor=*/batt::Runtime::instance().schedule_task(),
                                [this] {
                                  Status result = this->trimmer_.run();
                                  VLOG(1) << "Volume::trimmer_task_ exited with status=" << result;
                                },
                                "Volume::trimmer_task_");
  }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void Volume::halt()
{
  this->slot_writer_.halt();
  this->trim_control_->halt();
  this->trimmer_.halt();
  this->root_log_->close().IgnoreError();
  if (this->recycler_) {
    this->recycler_->halt();
  }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void Volume::join()
{
  if (this->trimmer_task_) {
    this->trimmer_task_->join();
    this->trimmer_task_ = None;
  }
  if (this->recycler_) {
    this->recycler_->join();
  }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
StatusOr<batt::Grant> Volume::reserve(u64 size, batt::WaitForResource wait_for_log_space)
{
  return this->slot_writer_.reserve(size, wait_for_log_space);
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//

StatusOr<SlotRange> Volume::append(const PackableRef& payload, batt::Grant& grant)
{
  return this->slot_writer_.append(grant, payload);
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//

StatusOr<SlotRange> Volume::append(AppendableJob&& appendable, batt::Grant& grant,
                                   Optional<SlotSequencer>&& sequencer)
{
  const auto check_sequencer_is_resolved = batt::finally([&sequencer] {
    BATT_CHECK_IMPLIES(bool{sequencer}, sequencer->is_resolved())
        << "If a SlotSequencer is passed, it must be resolved even on failure paths.";
  });

  //+++++++++++-+-+--+----- --- -- -  -  -   -
  // Phase 0: Wait for the previous slot in the sequence to be appended to the log.
  //
  if (sequencer) {
    BATT_DEBUG_INFO("awaiting previous slot in sequence; "
                    << BATT_INSPECT(sequencer->has_prev()) << BATT_INSPECT(sequencer->poll_prev())
                    << BATT_INSPECT(sequencer->get_current()) << sequencer->debug_info());

    StatusOr<SlotRange> prev_slot = sequencer->await_prev();
    if (!prev_slot.ok()) {
      sequencer->set_error(prev_slot.status());
    }
    BATT_REQUIRE_OK(prev_slot);

    // We only need to do a speculative sync here, because flushing later slots in the log implies
    // that all earlier ones are flushed, and we are going to do a durable sync (flush) for our
    // prepare event below.
    //
    BATT_DEBUG_INFO("awaiting flush of previous slot: " << *prev_slot);

    Status sync_prev = this->slot_writer_.sync(LogReadMode::kSpeculative,
                                               SlotUpperBoundAt{prev_slot->upper_bound});
    if (!sync_prev.ok()) {
      sequencer->set_error(sync_prev);
    }
    BATT_REQUIRE_OK(sync_prev);
  }

  //+++++++++++-+-+--+----- --- -- -  -  -   -
  // Phase 1: Write a prepare slot to the write-ahead log and flush it to durable storage.
  //
  BATT_DEBUG_INFO("appending PrepareJob slot to the WAL");

  StatusOr<SlotRange> prepare_slot =
      LLFS_COLLECT_LATENCY(this->metrics_.prepare_slot_append_latency,
                           this->slot_writer_.append(grant, prepare(appendable)));

  if (sequencer) {
    if (!prepare_slot.ok()) {
      BATT_CHECK(sequencer->set_error(prepare_slot.status()))
          << "each slot within a sequence may only be set once!";
    } else {
      BATT_CHECK(sequencer->set_current(*prepare_slot))
          << "each slot within a sequence may only be set once!";
    }
  }
  BATT_REQUIRE_OK(prepare_slot);

  BATT_DEBUG_INFO("flushing PrepareJob slot to storage");

  Status sync_prepare = LLFS_COLLECT_LATENCY(
      this->metrics_.prepare_slot_sync_latency,
      this->slot_writer_.sync(LogReadMode::kDurable, SlotUpperBoundAt{prepare_slot->upper_bound}));

  BATT_UNTESTED_COND(!sync_prepare.ok());
  BATT_REQUIRE_OK(sync_prepare);

  //+++++++++++-+-+--+----- --- -- -  -  -   -
  // Phase 2a: Commit the job; this writes new pages, updates ref counts, and deletes dropped
  // pages.
  //
  BATT_DEBUG_INFO("committing PageCacheJob");

  const JobCommitParams params{
      .caller_uuid = &this->get_volume_uuid(),
      .caller_slot = prepare_slot->lower_bound,
      .recycler = as_ref(*this->recycler_),
      .recycle_grant = nullptr,
      .recycle_depth = 0,
  };

  Status commit_job_result = commit(std::move(appendable.job), params, Caller::Unknown);

  // BATT_UNTESTED_COND(!commit_job_result.ok());
  BATT_REQUIRE_OK(commit_job_result);

  //+++++++++++-+-+--+----- --- -- -  -  -   -
  // Phase 2b: Write the commit slot.
  //
  BATT_DEBUG_INFO("writing commit slot");

  StatusOr<SlotRange> commit_slot =
      this->slot_writer_.append(grant, PackedCommitJob{
                                           .prepare_slot = prepare_slot->lower_bound,
                                       });

  BATT_REQUIRE_OK(commit_slot);

  return SlotRange{
      .lower_bound = prepare_slot->lower_bound,
      .upper_bound = commit_slot->upper_bound,
  };
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
StatusOr<VolumeReader> Volume::reader(const SlotRangeSpec& slot_range, LogReadMode mode)
{
  BATT_CHECK_NOT_NULLPTR(this->root_log_);

  SlotRange base_range = this->root_log_->slot_range(mode);

  // Clamp the effective slot range to the portion covered by the trim lock.
  //
  base_range.lower_bound = this->trim_lock_.with_lock([&](SlotReadLock& trim_lock) {
    if (trim_lock) {
      return slot_max(base_range.lower_bound, trim_lock.slot_range().lower_bound);
    }
    BATT_UNTESTED_LINE();
    return base_range.lower_bound;
  });

  // Acquire a lock on the
  //
  StatusOr<SlotReadLock> read_lock = this->trim_control_->lock_slots(
      SlotRange{
          .lower_bound = slot_range.lower_bound.value_or(base_range.lower_bound),
          .upper_bound = slot_range.upper_bound.value_or(base_range.upper_bound),
      },
      "Volume::read");

  BATT_UNTESTED_COND(!read_lock.ok());
  BATT_REQUIRE_OK(read_lock);

  return VolumeReader{*this, std::move(*read_lock), mode};
}

}  // namespace llfs