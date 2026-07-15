import engine.core;
import engine.math;
import engine.rhi;
import engine.app;
import std;

int main(int argc, char *argv[]) {
    using namespace pP;
    Application app("ppr", std::span<const char* const>(&argv[0], argc));
    return app.run();
}
