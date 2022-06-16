//#=##=##=#==#=#==#===#+==#+==========+==+=+=+=+=+=++=+++=+++++=-++++=-+++++++++++
//
// Part of the LLFS Project, under Apache License v2.0.
// See https://www.apache.org/licenses/LICENSE-2.0 for license information.
// SPDX short identifier: Apache-2.0
//
//+++++++++++-+-+--+----- --- -- -  -  -   -

#include <batteries/suppress.hpp>

BATT_SUPPRESS("-Wmaybe-uninitialized")

#include <llfs/page_recycler.hpp>
//

#include <llfs/metrics.hpp>
#include <llfs/page_cache_job.hpp>
#include <llfs/page_recycler_recovery_visitor.hpp>

#include <batteries/async/backoff.hpp>
#include <batteries/finally.hpp>
#include <batteries/hint.hpp>

#include <unordered_map>

namespace llfs {

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
namespace {

StatusOr<SlotRange> refresh_recycler_info_slot(TypedSlotWriter<PageRecycleEvent>& slot_writer,
                                               const boost::uuids::uuid& recycler_uuid,
                                               const PageRecyclerOptions& options,
                                               batt::Grant* slot_grant)
{
  auto packed_info = PackedPageRecyclerInfo::from(recycler_uuid, options);

  auto local_slot_grant = [&]() -> batt::StatusOr<batt::Grant> {
    if (slot_grant != nullptr) {
      return {batt::StatusCode::kUnknown};
    }
    return slot_writer.reserve(packed_sizeof_slot(packed_info), batt::WaitForResource::kFalse);
  }();

  if (slot_grant == nullptr) {
    BATT_REQUIRE_OK(local_slot_grant);
    slot_grant = &*local_slot_grant;
  }

  BATT_ASSIGN_OK_RESULT(SlotRange slot_range, slot_writer.append(*slot_grant, packed_info));

  Status sync_status =
      slot_writer.sync(LogReadMode::kDurable, SlotUpperBoundAt{.offset = slot_range.upper_bound});
  BATT_REQUIRE_OK(sync_status);

  return slot_range;
}

}  // namespace

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
/*static*/ u64 PageRecycler::calculate_log_size(MaxRefsPerPage max_refs_per_page,
                                                Optional<PageCount> max_buffered_page_count)
{
  static const PackedPageRecyclerInfo info = {};

  PageRecyclerOptions options;

  options.max_refs_per_page = max_refs_per_page;

  return options.total_page_grant_size() *
             (1 + max_buffered_page_count.value_or(PageCount{max_refs_per_page.value()})) +
         options.recycle_task_target() +
         packed_sizeof_slot(info) * (options.info_refresh_rate + 1) + 1 * kKiB;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
/*static*/ StatusOr<std::unique_ptr<PageRecycler>> PageRecycler::recover(
    batt::TaskScheduler& scheduler, std::string_view name, MaxRefsPerPage max_refs_per_page,
    PageDeleter& page_deleter, LogDeviceFactory& log_device_factory)
{
  initialize_status_codes();

  PageRecyclerOptions default_options;
  default_options.max_refs_per_page = max_refs_per_page;

  PageRecyclerRecoveryVisitor visitor{default_options};

  // Read the log, scanning its contents.
  //
  StatusOr<std::unique_ptr<LogDevice>> recovered_log = log_device_factory.open_log_device(
      [&](LogDevice::Reader& log_reader) -> StatusOr<slot_offset_type> {
        TypedSlotReader<PageRecycleEvent> slot_reader{log_reader};

        StatusOr<usize> slots_recovered = slot_reader.run(batt::WaitForResource::kFalse, visitor);
        BATT_REQUIRE_OK(slots_recovered);

        VLOG(1) << "PageRecycler recovered log: " << BATT_INSPECT(*slots_recovered);

        return log_reader.slot_offset();
      });

  BATT_REQUIRE_OK(recovered_log) << batt::LogLevel::kWarning << "log recovery failed!";

  Optional<SlotRange> latest_info_refresh_slot = visitor.latest_info_refresh_slot();

  // Write the first info block if necessary.
  {
    LogDevice& log = **recovered_log;
    if (!latest_info_refresh_slot ||
        visitor.options().info_needs_refresh(latest_info_refresh_slot->lower_bound, log)) {
      TypedSlotWriter<PageRecycleEvent> slot_writer{log};

      BATT_ASSIGN_OK_RESULT(latest_info_refresh_slot,
                            refresh_recycler_info_slot(slot_writer, visitor.recycler_uuid(),
                                                       visitor.options(), /*grant=*/nullptr));
    }
  }
  BATT_CHECK(latest_info_refresh_slot);

  // Construct the recovered state machine.
  //
  BATT_ASSIGN_OK_RESULT(Optional<PageRecycler::Batch> latest_batch, visitor.consume_latest_batch());

  auto state = std::make_unique<State>(
      visitor.recycler_uuid(), latest_info_refresh_slot->lower_bound, visitor.options(),
      recovered_log->get()->capacity(), recovered_log->get()->slot_range(LogReadMode::kDurable));

  state->bulk_load(as_slice(visitor.recovered_pages()));

  return std::unique_ptr<PageRecycler>{new PageRecycler(scheduler, std::string{name}, page_deleter,
                                                        std::move(*recovered_log),
                                                        std::move(latest_batch), std::move(state))};
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
PageRecycler::PageRecycler(batt::TaskScheduler& scheduler, const std::string& name,
                           PageDeleter& page_deleter, std::unique_ptr<LogDevice>&& wal_device,
                           Optional<Batch>&& recovered_batch,
                           std::unique_ptr<PageRecycler::State>&& state) noexcept
    : scheduler_{scheduler}
    , name_{name}
    , page_deleter_{page_deleter}
    , wal_device_{std::move(wal_device)}
    , slot_writer_{*this->wal_device_}
    , recycle_task_grant_{ok_result_or_panic(
          this->slot_writer_.reserve(0, batt::WaitForResource::kFalse))}
    , insert_grant_pool_{ok_result_or_panic(
          this->slot_writer_.reserve(0, batt::WaitForResource::kFalse))}
    , state_{std::move(state)}
    , recycle_task_{}
    , metrics_{}
    , prepared_batch_{std::move(recovered_batch)}
{
  const PageRecyclerOptions& options = this->state_.no_lock().options;

  BATT_CHECK_LE(PageRecycler::calculate_log_size(MaxRefsPerPage{options.max_refs_per_page}),
                this->slot_writer_.log_capacity())
      << "The recycler WAL is too small for the given configuration (the recycler will never "
         "make "
         "progress...)"
      << BATT_INSPECT(options.total_page_grant_size())
      << BATT_INSPECT(options.recycle_task_target());

  BATT_CHECK_GE(PageRecycler::calculate_log_size(MaxRefsPerPage{options.max_refs_per_page}),
                options.recycle_task_target() + options.insert_grant_size())
      << BATT_INSPECT(options.recycle_task_target()) << BATT_INSPECT(options.insert_grant_size());

  const auto metric_name = [this](std::string_view property) {
    return batt::to_string("PageRecycler_", this->name_, "_", property);
  };

#define ADD_METRIC_(n) global_metric_registry().add(metric_name(#n), this->metrics_.n)

  ADD_METRIC_(insert_count);
  ADD_METRIC_(remove_count);

#undef ADD_METRIC_
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
PageRecycler::~PageRecycler() noexcept
{
  VLOG(1) << "PageRecycler::~PageRecycler() ENTERED";

  this->halt();
  this->join();

  global_metric_registry()  //
      .remove(this->metrics_.insert_count)
      .remove(this->metrics_.remove_count);

  VLOG(1) << "PageRecycler::~PageRecycler() RETURNING";
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void PageRecycler::start()
{
  if (!this->recycle_task_) {
    this->refresh_grants();
    this->start_recycle_task();
  }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void PageRecycler::halt()
{
  if (!this->stop_requested_.exchange(true)) {
    this->state_.no_lock().pending_count.close();
    this->recycle_task_grant_.revoke();
    this->insert_grant_pool_.revoke();
    this->slot_writer_.halt();
    this->wal_device_->close().IgnoreError();
  }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void PageRecycler::join()
{
  if (this->recycle_task_) {
    this->recycle_task_->join();
  }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
StatusOr<slot_offset_type> PageRecycler::recycle_pages(const Slice<const PageId>& page_ids,
                                                       batt::Grant* grant, u32 depth)
{
  VLOG(1) << "PageRecycler::recycle_pages(page_ids=" << batt::dump_range(page_ids) << ") "
          << this->name_;

  if (page_ids.empty()) {
    return this->wal_device_->slot_range(LogReadMode::kDurable).upper_bound;
  }

  Optional<slot_offset_type> sync_point = None;

  if (grant == nullptr) {
    BATT_CHECK_EQ(depth, 0u);

    const PageRecyclerOptions& options = this->state_.no_lock().options;

    for (PageId page_id : page_ids) {
      StatusOr<batt::Grant> local_grant = [&] {
        const usize needed_size = options.insert_grant_size();

        BATT_DEBUG_INFO("[PageRecycler::recycle_page] waiting for log space; "
                        << BATT_INSPECT(needed_size)
                        << BATT_INSPECT(this->insert_grant_pool_.size()) << BATT_INSPECT(depth)
                        << BATT_INSPECT(options.total_grant_size_for_depth(depth))
                        << BATT_INSPECT(this->slot_writer_.in_use_size())
                        << BATT_INSPECT(this->slot_writer_.log_capacity())
                        << BATT_INSPECT(options.recycle_task_target()));

        return this->insert_grant_pool_.spend(needed_size, batt::WaitForResource::kTrue);
      }();
      if (!local_grant.ok()) {
        if (!suppress_log_output_for_test() && !this->stop_requested_.load()) {
          LOG(WARNING) << "PageRecycler::recycle_pages failed; not enough log buffer space";
        }
      }
      BATT_REQUIRE_OK(local_grant);
      {
        auto locked_state = this->state_.lock();
        StatusOr<slot_offset_type> append_slot =
            this->insert_to_log(*local_grant, page_id, depth, locked_state);
        BATT_REQUIRE_OK(append_slot);

        clamp_min_slot(&sync_point, *append_slot);
      }
    }
  } else {
    BATT_CHECK_LT(depth, kMaxPageRefDepth);

    auto locked_state = this->state_.lock();
    for (PageId page_id : page_ids) {
      StatusOr<slot_offset_type> append_slot =
          this->insert_to_log(*grant, page_id, depth, locked_state);
      BATT_REQUIRE_OK(append_slot);

      clamp_min_slot(&sync_point, *append_slot);
    }
  }

  BATT_CHECK(sync_point);
  return *sync_point;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
StatusOr<slot_offset_type> PageRecycler::insert_to_log(
    batt::Grant& grant, PageId page_id, u32 depth,
    batt::Mutex<std::unique_ptr<State>>::Lock& locked_state)
{
  BATT_CHECK(locked_state.is_held());

  // Update the state machine.
  //
  const slot_offset_type current_slot = this->slot_writer_.slot_offset();
  batt::SmallVec<PageToRecycle, 2> to_append = (*locked_state)
                                                   ->insert(PageToRecycle{
                                                       .page_id = page_id,
                                                       .slot_offset = current_slot,
                                                       .depth = depth,
                                                   });

  // The update was not accepted (already pending); return success anyhow (repeat inserts are
  // idempotent).
  //
  if (to_append.empty()) {
    return current_slot;
  }

  // Write the slots.
  //
  slot_offset_type last_slot = current_slot;
  for (const PageToRecycle& item : to_append) {
    StatusOr<SlotRange> append_slot = this->slot_writer_.append(grant, item);
    BATT_REQUIRE_OK(append_slot);
    last_slot = slot_max(last_slot, append_slot->upper_bound);
    VLOG(1) << "Write " << item << " to the log; last_slot=" << last_slot;
  }
  BATT_CHECK_NE(this->slot_writer_.slot_offset(), current_slot);

  this->metrics_.insert_count.fetch_add(1);

  return last_slot;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
Status PageRecycler::await_flush(Optional<slot_offset_type> min_upper_bound)
{
  if (!min_upper_bound) {
    return OkStatus();
  }
  return this->wal_device_->sync(LogReadMode::kDurable,
                                 SlotUpperBoundAt{.offset = *min_upper_bound});
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void PageRecycler::start_recycle_task()
{
  this->recycle_task_.emplace(/*executor=*/this->scheduler_.schedule_task(),
                              [this] {
                                this->recycle_task_main();
                              },
                              this->name_ + ".recycle_task");
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void PageRecycler::refresh_grants()
{
  VLOG(1) << "PageRecycler::refresh_grants() ENTERED";

  u64 available = this->slot_writer_.pool_size();
  VLOG(1) << " -- " << BATT_INSPECT(available);

  const PageRecyclerOptions& options = this->state_.no_lock().options;

  VLOG(1) << " -- " << BATT_INSPECT(this->recycle_task_grant_.size())
          << BATT_INSPECT(options.recycle_task_target());

  if (this->recycle_task_grant_.size() < options.recycle_task_target()) {
    const auto target_delta =
        std::min(available, options.recycle_task_target() - this->recycle_task_grant_.size());

    available -= target_delta;

    StatusOr<batt::Grant> tmp =
        this->slot_writer_.reserve(target_delta, batt::WaitForResource::kFalse);

    if (this->stop_requested_) {
      return;
    }

    BATT_CHECK(tmp.ok()) << "this->recycle_task_grant_.size()= " << this->recycle_task_grant_.size()
                         << " target_delta= " << target_delta;

    this->recycle_task_grant_.subsume(std::move(*tmp));
  }

  VLOG(1) << " -- " << BATT_INSPECT(available) << " (after reserving for recycle_task)";

  if (available > 0) {
    if (this->stop_requested_) {
      return;
    }

    BATT_CHECK_EQ(this->recycle_task_grant_.size(), options.recycle_task_target())
        << "The recycle task grant must take priority over the insert grant pool!"
        << BATT_INSPECT(this->stop_requested_);

    batt::Grant grant =
        ok_result_or_panic(this->slot_writer_.reserve(available, batt::WaitForResource::kFalse));
    BATT_CHECK_EQ(this->insert_grant_pool_.get_issuer(), grant.get_issuer());
    this->insert_grant_pool_.subsume(std::move(grant));
  }

  VLOG(1) << " -- " << BATT_INSPECT(this->insert_grant_pool_);
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void PageRecycler::recycle_task_main()
{
  const auto on_return = batt::finally([this] {
    // Stop the slot writer to unblock any threads that might be waiting on WAL space to write a
    // slot.  Since this is the task that drains the WAL, freeing up more space, these requests
    // will never unblock once we return.
    //
    this->slot_writer_.halt();
    this->recycle_task_grant_.revoke();
    this->insert_grant_pool_.revoke();
  });

  this->recycle_task_status_ = [this]() -> Status {
    const PageRecyclerOptions& options = this->state_.no_lock().options;

    for (;;) {
      if (BATT_HINT_TRUE(this->prepared_batch_)) {
        Batch batch_to_commit = std::move(*this->prepared_batch_);
        this->prepared_batch_ = None;

        Status commit_status = commit_batch(batch_to_commit);
        BATT_REQUIRE_OK(commit_status);

        Status trim_status = this->trim_log();
        BATT_REQUIRE_OK(trim_status);
      }

      // Wait for work.
      //
      {
        BATT_DEBUG_INFO("waiting for pending PageToRecycle events;"
                        << " latest_info_refresh_slot="
                        << this->state_.no_lock().latest_info_refresh_slot.get_value()
                        << " lru_slot=" << this->state_.lock()->get()->get_lru_slot()
                        << BATT_INSPECT(this->latest_batch_upper_bound_)
                        << BATT_INSPECT(this->wal_device_->slot_range(LogReadMode::kSpeculative))
                        << BATT_INSPECT(this->wal_device_->slot_range(LogReadMode::kDurable)));

        const usize observed_pending_count = this->state_.no_lock().pending_count.get_value();
        if (observed_pending_count == 0) {
          this->page_deleter_.notify_caught_up(*this,
                                               this->slot_upper_bound(LogReadMode::kSpeculative));

          StatusOr<usize> pending_count = this->state_.no_lock().pending_count.await_not_equal(0);
          BATT_REQUIRE_OK(pending_count);
        }
      }

      // De-queue the next page.  Block for the first page, then pull as many as we can after that
      // from the same depth.
      //
      std::vector<PageToRecycle> to_recycle =
          this->state_.lock()->get()->collect_batch(options.batch_size, this->metrics_);

      // We must write a PackedRecyclePagePrepare event to the WAL in case we need to recover from
      // a crash.
      //
      BATT_ASSIGN_OK_RESULT(this->prepared_batch_, this->prepare_batch(std::move(to_recycle)));
    }
  }();

  if (this->stop_requested_.load()) {
    VLOG(1) << "[PageRecycler::recycle_task] exited with status code= "
            << this->recycle_task_status_;
  } else {
    if (!suppress_log_output_for_test()) {
      LOG(WARNING) << "[PageRecycler::recycle_task] exited, no stop requested; code= "
                   << this->recycle_task_status_;
    }
    this->page_deleter_.notify_failure(*this, this->recycle_task_status_);
  }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
StatusOr<PageRecycler::Batch> PageRecycler::prepare_batch(std::vector<PageToRecycle>&& to_recycle)
{
  VLOG(1) << "Preparing Batch: " << batt::dump_range(to_recycle);

  const PageRecyclerOptions& options = this->state_.no_lock().options;

  Batch batch{
      .to_recycle = std::move(to_recycle),

      // This will be the slot used to guarantee that ref count updates are applied exactly once
      // for this page.
      //
      .slot_offset = this->slot_writer_.slot_offset(),
  };

  Optional<slot_offset_type> sync_upper_bound;

  for (const PageToRecycle& next_page : batch.to_recycle) {
    if (this->stop_requested_) {
      return Status{batt::StatusCode::kCancelled};
    }

    VLOG(1) << "[PageRecycler::recycle_task] writing remove slot: " << next_page
            << BATT_INSPECT(batch.slot_offset) << BATT_INSPECT(this->recycle_task_grant_.size())
            << " " << this->name_;

    StatusOr<SlotRange> append_slot = this->slot_writer_.append(
        this->recycle_task_grant_, PackedRecyclePagePrepare{
                                       .page_id = next_page.page_id.int_value(),
                                       .batch_slot = batch.slot_offset,
                                   });

    if (!append_slot.ok() && this->stop_requested_ && this->recycle_task_grant_.size() == 0) {
      return append_slot.status();
    }

    BATT_REQUIRE_OK(append_slot) << (suppress_log_output_for_test() ? batt::LogLevel::kVerbose
                                                                    : batt::LogLevel::kWarning)
                                 << BATT_INSPECT(this->recycle_task_grant_.size())
                                 << BATT_INSPECT(this->slot_writer_.pool_size())
                                 << BATT_INSPECT(options.recycle_task_target())
                                 << BATT_INSPECT(this->stop_requested_);

    clamp_min_slot(&sync_upper_bound, append_slot->upper_bound);
  }

  Status flush_status = this->await_flush(*sync_upper_bound);
  BATT_REQUIRE_OK(flush_status);

  VLOG(1) << "Batch Prepared; slot=" << batch.slot_offset;

  return batch;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
Status PageRecycler::commit_batch(const Batch& batch)
{
  VLOG(1) << "Committing Batch: " << batch;

  // Recycle the pages inside a job.
  //
  Status commit_status = with_retry_policy(
      batt::ExponentialBackoff::with_default_params(), "recycler_commit_job", [&]() -> Status {
        if (this->stop_requested_) {
          return Status{StatusCode::kRecyclerStopped};
        }
        const usize page_count = batch.to_recycle.size();
        Status delete_result = this->page_deleter_.delete_pages(
            as_slice(batch.to_recycle), *this, batch.slot_offset, this->recycle_task_grant_);
        if (delete_result.ok()) {
          this->metrics_.page_drop_ok_count.fetch_add(page_count);
        } else {
          this->metrics_.page_drop_error_count.fetch_add(page_count);
        }
        return delete_result;
      });

  BATT_REQUIRE_OK(commit_status);

  if (this->stop_requested_) {
    return Status{StatusCode::kRecyclerStopped};
  }

  VLOG(1) << "[PageRecycler::commit_batch] delete_pages OK; "
          << BATT_INSPECT(this->recycle_task_grant_.size()) << BATT_INSPECT(this->stop_requested_);

  // Write the Committed slot.
  //
  StatusOr<SlotRange> append_slot =
      this->slot_writer_.append(this->recycle_task_grant_, PackedRecycleBatchCommit{
                                                               .batch_slot = batch.slot_offset,
                                                           });
  BATT_REQUIRE_OK(append_slot);

  VLOG(1) << "[PageRecycler::commit_batch] append Commit slot OK";

  Status flush_status = this->await_flush(append_slot->upper_bound);
  BATT_REQUIRE_OK(flush_status);

  this->latest_batch_upper_bound_ = append_slot->upper_bound;

  VLOG(1) << "[PageRecycler::commit_batch] flush OK - done";

  return OkStatus();
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
Status PageRecycler::trim_log()
{
  const PageRecyclerOptions& options = this->state_.no_lock().options;
  const boost::uuids::uuid& recycler_uuid = this->state_.no_lock().uuid;

  // Calculate the highest safe trim offset.  We can't go above "lru_slot" (if there is one) or
  // "latest_batch_upper_bound" (if there is one).
  //
  slot_offset_type latest_info_slot = this->state_.no_lock().latest_info_refresh_slot.get_value();

  const Optional<slot_offset_type> lru_slot = this->state_.lock()->get()->get_lru_slot();

  const slot_offset_type trim_point = [&] {
    if (lru_slot && this->latest_batch_upper_bound_) {
      return slot_min(*lru_slot, *this->latest_batch_upper_bound_);
    }
    if (lru_slot) {
      BATT_CHECK(!this->latest_batch_upper_bound_);
      return lru_slot.value_or(latest_info_slot);
    } else {
      BATT_CHECK(this->latest_batch_upper_bound_);
      return this->latest_batch_upper_bound_.value_or(latest_info_slot);
    }
  }();

  // Check to see if we need to refresh the info slot.
  //
  if (options.info_needs_refresh(latest_info_slot, *this->wal_device_) ||
      trim_point > latest_info_slot) {
    VLOG(1) << "[PageRecycler::recycle_task] refreshing info slot; "
            << BATT_INSPECT(latest_info_slot) << BATT_INSPECT(lru_slot)
            << BATT_INSPECT(this->latest_batch_upper_bound_) << BATT_INSPECT(trim_point)
            << BATT_INSPECT(this->wal_device_->slot_range(LogReadMode::kSpeculative))
            << BATT_INSPECT(this->wal_device_->slot_range(LogReadMode::kDurable));

    StatusOr<batt::Grant> info_slot_grant =
        this->recycle_task_grant_.spend(options.info_slot_size());

    if (!info_slot_grant.ok()) {
      BATT_CHECK(this->stop_requested_);
      return Status{StatusCode::kRecyclerStopped};
    }

    if (this->stop_requested_) {
      return Status{StatusCode::kRecyclerStopped};
    }

    BATT_ASSIGN_OK_RESULT(
        const SlotRange new_info_slot,
        refresh_recycler_info_slot(this->slot_writer_, recycler_uuid, options, &*info_slot_grant));

    latest_info_slot = new_info_slot.lower_bound;
    this->state_.no_lock().latest_info_refresh_slot.set_value(latest_info_slot);

    VLOG(1) << "[PageRecycler::recycle_task]  --> " << BATT_INSPECT(new_info_slot)
            << BATT_INSPECT(this->wal_device_->slot_range(LogReadMode::kSpeculative))
            << BATT_INSPECT(this->wal_device_->slot_range(LogReadMode::kDurable));
  }

  // IMPORTANT - never trim off the latest info slot!
  //
  BATT_CHECK_LE(trim_point, latest_info_slot);

  VLOG(1) << "trim_log: " << trim_point;

  // Trim the log; MUST happen after the await_flush.
  //
  Status trim_status = this->slot_writer_.trim(trim_point).status();
  BATT_REQUIRE_OK(trim_status);

  // Top off WAL grants; first the recycle task grant, then the insert grant pool.
  //
  this->refresh_grants();

  return OkStatus();
}

}  // namespace llfs

BATT_UNSUPPRESS()