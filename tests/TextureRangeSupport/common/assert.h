#ifndef KYTY_TEXTURE_RANGE_STANDALONE_ASSERT_SHIM_H_
#define KYTY_TEXTURE_RANGE_STANDALONE_ASSERT_SHIM_H_
// Used ONLY when compiling the exact production MultiLevelPageTable header in
// the isolated no-dependency Windows CPU experiment. The production assert
// helper otherwise requires the complete Kyty common/fmt linkage. Preserve
// fatal behavior for invalid page accesses; do NOT disable range checks.
#include <cstdlib>
#define EXIT(...) do { std::abort(); } while (false)
// Preserve the production fatal validation path (including SlotVector invariants).
#define EXIT_IF(condition) do { if (condition) std::abort(); } while (false)
#endif
