import std;
import engine.core;
import engine.rhi;
import engine.tests;
import engine.tests.app;

int main(const int argc, char *argv[]) {
    namespace tests = pP::tests;

    // GPU validation is opt-in: the `*.gpu_validation` CTest entries export
    // PPR_TEST_GPU_VALIDATION=1. Every other invocation explicitly pins both
    // RHI validation modes off so the debug layers never cost ordinary runs.
    const auto gpu_validation = pP::config::getenvLookup("PPR_TEST_GPU_VALIDATION");
    if (not gpu_validation.has_value() or *gpu_validation != "1") {
        const auto rhi = pP::IRhiService::get();
        rhi->setCoreValidation(false);
        rhi->setGpuAssistedValidation(false);
    }

    return tests::runTests(argc, argv, tests::app, "app-tests");
}
