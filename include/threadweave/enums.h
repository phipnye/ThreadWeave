#ifndef TW_ENUSM_H
#define TW_ENUSM_H

#include <threadweave/utils.h>

#include <cstdint>

namespace ThreadWeave::Internal {

// Enum indicating the status a future
enum class FutureStatus : std::int8_t { pending, ready, waiting };

// Enum tracking the index of a hazard slot
enum class HazardSlot : Index {
  Stack0 = 0,  // Stack only requires one hazard
  Queue0 = 0,  // Queue requires two hazards
  Queue1 = 1,
  Alloc2 = 2,  // Future requires one isolated hazard
  COUNT = 3    // Number of hazard slots per thread
};

}  // namespace ThreadWeave::Internal

#endif
