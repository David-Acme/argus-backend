#pragma once

#include <shared/contracts/tool-contracts.hxx>
#include <vector>

// Tool metadata shared by the in-process MemoryService and the remote wire
// adapter (Ruling BY): one definition, so the wire descriptors cannot drift
// from the legacy tool loop. Handlers are attached by the caller.
std::vector<tools::ToolDescriptor> memoryToolDescriptors();
