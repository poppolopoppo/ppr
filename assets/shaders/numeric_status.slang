// ------------------------------------------------------------------
// bounded numeric status records
// ------------------------------------------------------------------
#ifndef PPR_NUMERIC_STATUS_SLANG
#define PPR_NUMERIC_STATUS_SLANG

// One 32-byte record per numeric-producing dispatch or draw in its in-flight
// attempt's status buffer. Words 0/1 are the atomic minima of the invalid-state
// and range-error index; UINT32_MAX means "no failure". Words 2..7 are reserved
// zero and are never used as racing payload: there is no compound atomic and no
// nondeterministic first-error-thread ordering. The CPU ticket maps each record
// to its fixed pass/level and world/accepted-frame identity.
struct NumericStatus {
    uint invalidIndex;
    uint rangeIndex;
    uint reserved[6];
};

static const uint kNumericStatusWords = 8u;
// Four projection records (clear/keys/rows/cells) followed by the transport
// records (source preparation, terminal level L5, lower cascades L4..L1).
static const uint kNumericStatusRecords = 7u;
static const uint kNoNumericIndex = 0xffffffffu;
// The projection stage reports only invalid arithmetic/state (nonfinite and
// out-of-domain candidates). The source/terminal/cascade stages report both
// categories: invalid arithmetic/state and representational range overflow of
// their bounded radiance records. Both minima are checked independently when a
// stage is inspected.
static const uint kNumericInvalidState = 0u;
static const uint kNumericRangeError = 1u;

// Bound as an 8-word-per-record array so both minima are ordinary 32-bit
// atomics on disjoint words.
RWStructuredBuffer<uint> g_numeric_status;

// Records the first (lowest) failing index for one category. A failing
// invocation writes this and then publishes a finite containment value (or
// rejects its candidate) so no invalid memory access or NaN can propagate; the
// containment value never makes the frame successful.
void numericRecordFailure(uint record, uint category, uint index) {
    InterlockedMin(g_numeric_status[record * kNumericStatusWords + category], index);
}
#endif
