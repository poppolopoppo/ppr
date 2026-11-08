import std;
import engine.core;
import engine.rhi;
import engine.tests;
import engine.tests.asset;

int main(const int argc, char *argv[]) {
    namespace tests = pP::tests;

    // GPU validation is opt-in: the `*.gpu_validation` CTest entries export
    // PPR_TEST_GPU_VALIDATION=1. Explicitly pin both RHI validation modes before
    // fixture initialization so the policy does not depend on build defaults.
    const auto gpu_validation = pP::config::getenvLookup("PPR_TEST_GPU_VALIDATION");
    const bool validation_enabled = gpu_validation.has_value() and *gpu_validation == "1";

    const auto rhi = pP::IRhiService::get();
    rhi->setCoreValidation(validation_enabled);
    rhi->setGpuAssistedValidation(validation_enabled);

    return tests::runTests(argc, argv, tests::asset, "asset-tests");
}
