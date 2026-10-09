import std;
import engine.tests;
import engine.tests.core;

int main(int argc, char *argv[]) {
    namespace tests = pP::tests;

    return tests::runTests(argc, argv, tests::core, "core-tests");
}
