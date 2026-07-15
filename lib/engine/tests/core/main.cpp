import engine.tests;
import engine.tests.core;

int main(const int argc, char *argv[]) {
    namespace tests = pP::tests;

    tests::TestCli cli = tests::parseCli(argc, argv);
    return tests::runSuite(std::move(cli), tests::core);
}
