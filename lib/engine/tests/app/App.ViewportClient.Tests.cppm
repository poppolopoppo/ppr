module;

#include "pP/Macros.h"
#include "pP/UnitTest.h"

export module engine.tests.app:viewport_client;

import engine.app;
import engine.core;
import engine.math;
import std;

export namespace pP::tests {
    PPR_UNIT_TEST(viewport_client_construction) {
        Camera cam;
        ViewportClient client{cam, int2{800, 600}};
        PPR_TEST_ASSERT(&client.camera() == &cam);
        PPR_TEST_ASSERT(client.clientRect().x == 800);
        PPR_TEST_ASSERT(client.clientRect().y == 600);
        PPR_TEST_ASSERT(client.viewport().clientRect().x == 800);
        PPR_TEST_ASSERT(client.viewport().clientRect().y == 600);
    };

    PPR_UNIT_TEST(viewport_client_client_rect) {
        Camera cam;
        ViewportClient client{cam, int2{800, 600}};
        client.setClientRect(int2{1024, 768});
        PPR_TEST_ASSERT(client.clientRect().x == 1024);
        PPR_TEST_ASSERT(client.clientRect().y == 768);
        PPR_TEST_ASSERT(client.viewport().clientRect().x == 1024);
        PPR_TEST_ASSERT(client.viewport().clientRect().y == 768);
    };

    PPR_UNIT_TEST(viewport_client_update_pipeline) {
        Camera cam;
        ViewportClient client{cam, int2{800, 600}};
        DummyCameraController ctrl;
        client.update(std::chrono::milliseconds{16}, ctrl);
        // updateModel applied the (unchanged) model with the client rect as viewport size.
        PPR_TEST_ASSERT(cam.viewportSize().x == 800.0f);
        PPR_TEST_ASSERT(cam.viewportSize().y == 600.0f);

        // Optional clientRect overrides the viewport rect for this update.
        client.update(std::chrono::milliseconds{16}, ctrl, int2{1920, 1080});
        PPR_TEST_ASSERT(client.clientRect().x == 1920);
        PPR_TEST_ASSERT(client.clientRect().y == 1080);
        PPR_TEST_ASSERT(cam.viewportSize().x == 1920.0f);
        PPR_TEST_ASSERT(cam.viewportSize().y == 1080.0f);
    };

    PPR_UNIT_TEST(viewport_client_update_keeps_model) {
        Camera cam;
        ViewportClient client{cam, int2{800, 600}};
        DummyCameraController ctrl;
        const CameraModel model{
            .position = float3{1.0f, 2.0f, 3.0f},
            .right = float3{1.0f, 0.0f, 0.0f},
            .up = float3{0.0f, 1.0f, 0.0f},
            .forward = float3{0.0f, 0.0f, 1.0f},
            .cameraCut = false,
        };
        cam.updateModel(model, int2{640, 480});
        client.update(std::chrono::milliseconds{16}, ctrl);
        // The controller is a no-op, so the model carries through to the camera.
        PPR_TEST_ASSERT(distance(cam.position(), float3{1.0f, 2.0f, 3.0f}) < 1e-4f);
        PPR_TEST_ASSERT(cam.viewportSize().x == 800.0f);
    };

    PPR_UNIT_TEST(app_viewport_client) {
        _.recurse({
            viewport_client_construction,
            viewport_client_client_rect,
            viewport_client_update_pipeline,
            viewport_client_update_keeps_model,
        });
    };
}
