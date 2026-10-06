#include "component-wire.hxx"

namespace component_wire
{
namespace wire = argus::settings::v1;

wire::ComponentSource sourceOf(ComponentSource source)
{
  return source == ComponentSource::Download ? wire::COMPONENT_SOURCE_DOWNLOAD : wire::COMPONENT_SOURCE_PROVISIONED;
}

ComponentSource sourceFrom(wire::ComponentSource source)
{
  return source == wire::COMPONENT_SOURCE_DOWNLOAD ? ComponentSource::Download : ComponentSource::Provisioned;
}

wire::ComponentState stateOf(ComponentState state)
{
  switch (state) {
  case ComponentState::Installed: return wire::COMPONENT_STATE_INSTALLED;
  case ComponentState::Missing: return wire::COMPONENT_STATE_MISSING;
  case ComponentState::Installing: return wire::COMPONENT_STATE_INSTALLING;
  case ComponentState::Failed: return wire::COMPONENT_STATE_FAILED;
  case ComponentState::HostOnly: return wire::COMPONENT_STATE_HOST_ONLY;
  }
  return wire::COMPONENT_STATE_UNSPECIFIED;
}

ComponentState stateFrom(wire::ComponentState state)
{
  switch (state) {
  case wire::COMPONENT_STATE_INSTALLED: return ComponentState::Installed;
  case wire::COMPONENT_STATE_INSTALLING: return ComponentState::Installing;
  case wire::COMPONENT_STATE_FAILED: return ComponentState::Failed;
  case wire::COMPONENT_STATE_HOST_ONLY: return ComponentState::HostOnly;
  default: return ComponentState::Missing;
  }
}

void fill(wire::ComponentSpec& wire, const ComponentSpec& spec)
{
  wire.set_id(spec.id);
  wire.set_source(sourceOf(spec.source));
  wire.set_host_command(spec.hostCommand);
  for (const auto& file : spec.files) {
    auto* entry = wire.add_files();
    entry->set_path(file.path);
    entry->set_url(file.url);
    entry->set_size_bytes(file.sizeBytes);
    entry->set_sha256(file.sha256);
  }
}

ComponentSpec specFrom(const wire::ComponentSpec& wire)
{
  ComponentSpec spec{
      .id = wire.id(), .source = sourceFrom(wire.source()), .files = {}, .hostCommand = wire.host_command()};
  spec.files.reserve(static_cast<std::size_t>(wire.files_size()));
  for (const auto& file : wire.files())
    spec.files.push_back(
        {.path = file.path(), .url = file.url(), .sizeBytes = file.size_bytes(), .sha256 = file.sha256()});
  return spec;
}

wire::PinVerdict verdictOf(PinVerdict verdict)
{
  switch (verdict) {
  case PinVerdict::NoPin: return wire::PIN_VERDICT_NO_PIN;
  case PinVerdict::Accepted: return wire::PIN_VERDICT_ACCEPTED;
  case PinVerdict::Required: return wire::PIN_VERDICT_REQUIRED;
  case PinVerdict::Invalid: return wire::PIN_VERDICT_INVALID;
  case PinVerdict::Locked: return wire::PIN_VERDICT_LOCKED;
  }
  return wire::PIN_VERDICT_UNSPECIFIED;
}

PinVerdict verdictFrom(wire::PinVerdict verdict)
{
  switch (verdict) {
  case wire::PIN_VERDICT_NO_PIN: return PinVerdict::NoPin;
  case wire::PIN_VERDICT_ACCEPTED: return PinVerdict::Accepted;
  case wire::PIN_VERDICT_INVALID: return PinVerdict::Invalid;
  case wire::PIN_VERDICT_LOCKED: return PinVerdict::Locked;
  default: return PinVerdict::Required;
  }
}

void fill(wire::ComponentStatus& wire, const ComponentStatus& status)
{
  wire.set_id(status.id);
  wire.set_state(stateOf(status.state));
  wire.set_bytes_present(status.bytesPresent);
  wire.set_bytes_total(status.bytesTotal);
  wire.set_ready(status.ready);
  wire.set_host_command(status.hostCommand);
  wire.set_reason(status.reason);
}

ComponentStatus statusFrom(const wire::ComponentStatus& wire)
{
  return {.id = wire.id(),
          .state = stateFrom(wire.state()),
          .bytesPresent = wire.bytes_present(),
          .bytesTotal = wire.bytes_total(),
          .ready = wire.ready(),
          .hostCommand = wire.host_command(),
          .reason = wire.reason()};
}
}
