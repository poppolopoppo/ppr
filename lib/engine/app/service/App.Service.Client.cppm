module;
#include "pP/Macros.h"
export module engine.app:service.client;

import engine.core;
import std;

export namespace pP {
    class Application;
    class Camera;
    class ICameraController;
    class InputContext;
    class Player;
    class WindowViewport;

    // ------------------------------------------------------------------
    // encapsulate window events and camera handling for different scenarios
    // ------------------------------------------------------------------

    class IClientService : public IService {
    public:
        [[nodiscard]] virtual safe_ptr<const Application> getApplication() const noexcept = 0;


        [[nodiscard]] virtual safe_ptr<const Camera> getMainCamera() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<const InputContext> getMainInputContext() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<const Player> getMainPlayer() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<const WindowViewport> getMainViewport() const noexcept = 0;


        [[nodiscard]] virtual safe_ptr<Camera> getMainCamera() noexcept = 0;

        [[nodiscard]] virtual safe_ptr<InputContext> getMainInputContext() noexcept = 0;

        [[nodiscard]] virtual safe_ptr<Player> getMainPlayer() noexcept = 0;

        [[nodiscard]] virtual safe_ptr<WindowViewport> getMainViewport() noexcept = 0;
    };
}
