#include "virtual_display/driver/driver_controller.h"

#include <utility>

namespace virtual_display::driver {
  bool ControllerStatus::ok() const {
    return store_error == StoreError::None &&
           validation_error == ValidationError::None &&
           backend_error == BackendError::None;
  }

  DriverController::DriverController(DisplayStore store, DisplayDriverBackend &backend):
      store_ {std::move(store)},
      backend_ {backend} {
  }

  ControllerCreateResult DriverController::create_temporary_display(
    const CreateTemporaryDisplayRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    auto created = store_.create_temporary_display(request, now);
    if (created.status.error != StoreError::None) {
      return {from_store_result(created.status), {}};
    }

    const auto record = store_.find_temporary_display(request.display_id);
    if (!record) {
      return {
        {StoreError::DisplayNotFound, ValidationError::None, BackendError::None},
        {}
      };
    }

    const auto backend_result = backend_.arrive_temporary_display(descriptor_from_record(*record));
    if (backend_result.error != BackendError::None) {
      LeaseDisplayRequest rollback {};
      rollback.lease_id = request.lease_id;
      rollback.display_id = request.display_id;
      (void) store_.remove_temporary_display(rollback);
      return {
        {StoreError::None, ValidationError::None, backend_result.error},
        {}
      };
    }

    created.result.os_adapter_luid = backend_result.os_adapter_luid;
    created.result.target_id = backend_result.target_id;
    return {{}, created.result};
  }

  ControllerStatus DriverController::remove_temporary_display(const LeaseDisplayRequest &request) {
    if (const auto validation = validate_lease_display_request(request);
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    const auto record = store_.find_temporary_display(request.display_id);
    if (!record || record->lease_id != request.lease_id) {
      return {StoreError::DisplayNotFound, ValidationError::None, BackendError::None};
    }

    if (const auto backend_error = backend_.depart_temporary_display(request.display_id);
        backend_error != BackendError::None) {
      return {StoreError::None, ValidationError::None, backend_error};
    }

    return from_store_result(store_.remove_temporary_display(request));
  }

  ControllerStatus DriverController::feed_lease(
    const LeaseRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    return from_store_result(store_.feed_lease(request, now));
  }

  ControllerStatus DriverController::release_lease(const LeaseRequest &request) {
    if (const auto validation = validate_lease_request(request);
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    const auto displays = store_.temporary_displays_for_lease(request.lease_id);
    if (displays.empty()) {
      return from_store_result(store_.release_lease(request));
    }

    BackendError backend_error = BackendError::None;
    for (const auto &display: displays) {
      if (backend_.depart_temporary_display(display.display_id) == BackendError::None) {
        LeaseDisplayRequest remove {};
        remove.lease_id = display.lease_id;
        remove.display_id = display.display_id;
        (void) store_.remove_temporary_display(remove);
      } else {
        backend_error = BackendError::Failed;
      }
    }

    return {StoreError::None, ValidationError::None, backend_error};
  }

  QueryLeaseResult DriverController::query_lease(
    const std::uint64_t lease_id,
    const std::chrono::steady_clock::time_point now
  ) const {
    return store_.query_lease(lease_id, now);
  }

  ControllerStatus DriverController::set_permanent_display_count(const PermanentDisplayCountRequest &request) {
    if (const auto validation = validate_permanent_display_count(request, store_.max_permanent_displays());
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    if (const auto backend_error = backend_.set_permanent_display_count(request.display_count);
        backend_error != BackendError::None) {
      return {StoreError::None, ValidationError::None, backend_error};
    }

    return from_store_result(store_.set_permanent_display_count(request));
  }

  PermanentDisplayCountResult DriverController::query_permanent_display_count() const {
    return store_.query_permanent_display_count();
  }

  std::uint32_t DriverController::reap_expired(const std::chrono::steady_clock::time_point now) {
    const auto expired = store_.expired_temporary_displays(now);
    std::uint32_t removed = 0;

    for (const auto &display: expired) {
      if (backend_.depart_temporary_display(display.display_id) == BackendError::None) {
        LeaseDisplayRequest remove {};
        remove.lease_id = display.lease_id;
        remove.display_id = display.display_id;
        if (store_.remove_temporary_display(remove).error == StoreError::None) {
          ++removed;
        }
      }
    }

    return removed;
  }

  const DisplayStore &DriverController::store() const {
    return store_;
  }

  ControllerStatus DriverController::from_store_result(const StoreResult &result) {
    return {result.error, result.validation_error, BackendError::None};
  }

  DisplayDescriptor DriverController::descriptor_from_record(const TemporaryDisplayRecord &record) const {
    DisplayDescriptor descriptor {};
    descriptor.lease_id = record.lease_id;
    descriptor.display_id = record.display_id;
    descriptor.container_id = container_guid_from_display_id(record.display_id);
    descriptor.connector_index = record.connector_index;
    descriptor.width = record.width;
    descriptor.height = record.height;
    descriptor.refresh_rate_millihz = record.refresh_rate_millihz;
    descriptor.edid = create_edid(edid_options_for_temporary_display(record));
    return descriptor;
  }

  const char *to_string(const BackendError error) {
    switch (error) {
      case BackendError::None:
        return "none";
      case BackendError::Failed:
        return "failed";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
