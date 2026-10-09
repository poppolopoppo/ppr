import std;
import engine.tests;
import engine.tests.app;

int main(const int argc, char *argv[]) {
    namespace tests = pP::tests;

    return tests::runTests(argc, argv, tests::app, "app-tests");
}
