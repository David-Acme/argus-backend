#pragma once

#include <shared/contracts/tool-contracts.hxx>
#include <vector>

// Tool metadata shared by the in-process MemoryService and the remote wire adapter.
std::vector<tools::ToolDescriptor> memoryToolDescriptors();
