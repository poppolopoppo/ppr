// Test-only corrupted-table probe of the production guard and witness sink.
#include "solver.slang"

// ------------------------------------------------------------------
// device property admission
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(1, 1, 1)]
void inspectPropertyAdmission() {
    particlePropertiesUsable(0u);
}
