import std;
import engine.tests;
import engine.tests.sim;

int main(const int argc, char *argv[]) {
    namespace tests = pP::tests;

    return tests::runTests(argc, argv, tests::sim, "sim-tests");
}
