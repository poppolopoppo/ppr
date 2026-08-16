module;
#include "pP/UnitTest.h"
#include <slang.h>
#include <slang-com-ptr.h>

export module engine.tests.app:shader;

import engine.core;
import engine.shader;
import engine.rhi;
import std;

export namespace pP::tests {
    // ------------------------------------------------------------------
    // A. Shader Error Codes
    // ------------------------------------------------------------------
    namespace ShaderErrC {
        static_assert(static_cast<int>(shader::errc::ok) == SLANG_OK);
        static_assert(static_cast<int>(shader::errc::not_found) == SLANG_E_NOT_FOUND);
        static_assert(static_cast<int>(shader::errc::invalid_arg) == SLANG_E_INVALID_ARG);
        static_assert(static_cast<int>(shader::errc::not_implemented) == SLANG_E_NOT_IMPLEMENTED);
        static_assert(static_cast<int>(shader::errc::out_of_memory) == SLANG_E_OUT_OF_MEMORY);
        static_assert(static_cast<int>(shader::errc::cannot_open) == SLANG_E_CANNOT_OPEN);

        PPR_UNIT_TEST(errc_enum_values) {
            PPR_TEST_ASSERT(static_cast<int>(shader::errc::ok) == SLANG_OK);
            PPR_TEST_ASSERT(static_cast<int>(shader::errc::not_found) == SLANG_E_NOT_FOUND);
            PPR_TEST_ASSERT(static_cast<int>(shader::errc::invalid_arg) == SLANG_E_INVALID_ARG);
            PPR_TEST_ASSERT(static_cast<int>(shader::errc::not_implemented) == SLANG_E_NOT_IMPLEMENTED);
            PPR_TEST_ASSERT(static_cast<int>(shader::errc::out_of_memory) == SLANG_E_OUT_OF_MEMORY);
            PPR_TEST_ASSERT(static_cast<int>(shader::errc::cannot_open) == SLANG_E_CANNOT_OPEN);
        };

        PPR_UNIT_TEST(errc_category_name) {
            const auto &cat = shader::error_category();
            PPR_TEST_ASSERT(std::string_view(cat.name()) == "slang");
        };

        PPR_UNIT_TEST(errc_ok_not_error) {
            auto ec = shader::make_error_code(shader::errc::ok);
            PPR_TEST_ASSERT(!ec);

            ec = shader::make_error_code(shader::errc::not_found);
            PPR_TEST_ASSERT(!!ec);
            PPR_TEST_ASSERT(ec.category().name() == std::string_view("slang"));
        };

        PPR_UNIT_TEST(errc_make_error_code_from_result) {
            auto ec = shader::make_error_code(SLANG_OK);
            PPR_TEST_ASSERT(!ec);

            ec = shader::make_error_code(SLANG_FAIL);
            PPR_TEST_ASSERT(!!ec);
        };

        PPR_UNIT_TEST(errc_result_convenience) {
            auto ec = shader::result(SLANG_OK);
            PPR_TEST_ASSERT(!ec);

            ec = shader::result(SLANG_FAIL);
            PPR_TEST_ASSERT(!!ec);
        };

        PPR_UNIT_TEST(errc_error_condition_mapping) {
            auto ec = shader::make_error_code(shader::errc::invalid_arg);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::invalid_argument);

            ec = shader::make_error_code(shader::errc::not_found);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::no_such_file_or_directory);

            ec = shader::make_error_code(shader::errc::time_out);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::timed_out);

            ec = shader::make_error_code(shader::errc::buffer_too_small);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::result_out_of_range);

            ec = shader::make_error_code(shader::errc::not_implemented);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::function_not_supported);

            ec = shader::make_error_code(shader::errc::out_of_memory);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::not_enough_memory);
        };
    }

    // ------------------------------------------------------------------
    // B. RHI Error Codes
    // ------------------------------------------------------------------
    namespace RhiErrC {
        PPR_UNIT_TEST(rhi_error_category_name) {
            const auto &cat = rhi::error_category();
            PPR_TEST_ASSERT(std::string_view(cat.name()) == "slang-rhi");
        };

        PPR_UNIT_TEST(rhi_make_error_code) {
            auto ec = rhi::make_error_code(rhi::errc::ok);
            PPR_TEST_ASSERT(!ec);

            ec = rhi::make_error_code(rhi::errc::not_found);
            PPR_TEST_ASSERT(!!ec);
            PPR_TEST_ASSERT(ec.category().name() == std::string_view("slang-rhi"));

            ec = rhi::make_error_code(rhi::errc::unknown_error);
            PPR_TEST_ASSERT(!!ec);

            ec = rhi::result(SLANG_OK);
            PPR_TEST_ASSERT(!ec);

            ec = rhi::result(SLANG_FAIL);
            PPR_TEST_ASSERT(!!ec);
        };

        PPR_UNIT_TEST(rhi_error_condition_mapping) {
            auto ec = rhi::make_error_code(rhi::errc::invalid_arg);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::invalid_argument);

            ec = rhi::make_error_code(rhi::errc::not_found);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::no_such_file_or_directory);

            ec = rhi::make_error_code(rhi::errc::time_out);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::timed_out);

            ec = rhi::make_error_code(rhi::errc::buffer_too_small);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::result_out_of_range);

            ec = rhi::make_error_code(rhi::errc::out_of_memory);
            PPR_TEST_ASSERT(ec.default_error_condition() == std::errc::not_enough_memory);
        };
    }

    // ------------------------------------------------------------------
    // C. Diagnose & ModuleHandle
    // ------------------------------------------------------------------
    namespace ModHandle {
        PPR_UNIT_TEST(diagnose_default_construct) {
            shader::Diagnose diag;
            slang::IBlob **ref = diag.writeRef();
            PPR_TEST_ASSERT(ref != nullptr);
            PPR_TEST_ASSERT(*ref == nullptr);
        };

        PPR_UNIT_TEST(module_handle_default) {
            shader::SharedModule handle;
            PPR_TEST_ASSERT(handle.get() == nullptr);
        };
    }

    // ------------------------------------------------------------------
    // D. Shader Compilation & Reflection
    // ------------------------------------------------------------------
    namespace ShaderComp {
        constexpr string_literal kTriangleShader = R"(
struct VSOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};
[shader("vertex")]
VSOutput vertexMain(float3 position : POSITION, float3 color : COLOR) {
    VSOutput output;
    output.position = float4(position, 1.0);
    output.color = color;
    return output;
}
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
)";

        PPR_UNIT_TEST(compile_simple_triangle_shader) {
            const auto svc = IShaderService::get();
            PPR_TEST_ASSERT(!svc->initialize());

            shader::SharedModule handle;
            const auto ec = svc->loadModuleFromSource("triangle", "triangle.slang", kTriangleShader, handle.writeRef());
            PPR_TEST_ASSERT(!ec);

            auto *module = handle.get();
            PPR_TEST_ASSERT(module != nullptr);

            shader::ComPtr<slang::IEntryPoint> vertex_ep;
            PPR_TEST_ASSERT(SLANG_SUCCEEDED(module->findEntryPointByName("vertexMain", vertex_ep.writeRef())));
            PPR_TEST_ASSERT(vertex_ep != nullptr);

            shader::ComPtr<slang::IEntryPoint> fragment_ep;
            PPR_TEST_ASSERT(SLANG_SUCCEEDED(module->findEntryPointByName("fragmentMain", fragment_ep.writeRef())));
            PPR_TEST_ASSERT(fragment_ep != nullptr);

            std::ignore = svc->shutdown();
        };

        constexpr string_literal kParamShader = R"(
struct Constants {
    float4x4 g_proj;
    float4 g_color;
    float g_intensity;
};
ConstantBuffer<Constants> g_constants : register(b0);

struct VSOutput {
    float4 position : SV_Position;
    float4 color : COLOR;
};
[shader("vertex")]
VSOutput vertexMain(float3 position : POSITION) {
    VSOutput output;
    output.position = mul(g_constants.g_proj, float4(position, 1.0));
    output.color = g_constants.g_color;
    return output;
}
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    return input.color * g_constants.g_intensity;
}
)";

        PPR_UNIT_TEST(compile_invalid_shader_recovery) {
            const auto svc = IShaderService::get();
            PPR_TEST_ASSERT(!svc->initialize());

            shader::SharedModule handle;
            const auto bad_ec = svc->loadModuleFromSource("bad", "bad.slang", "this is not valid shader code @@@", handle.writeRef());
            PPR_TEST_ASSERT(!!bad_ec);

            const auto recovery_ec = svc->loadModuleFromSource("recovery", "recovery.slang", kTriangleShader, handle.writeRef());
            PPR_TEST_ASSERT(!recovery_ec);
            PPR_TEST_ASSERT(handle.get() != nullptr);

            std::ignore = svc->shutdown();
        };

        PPR_UNIT_TEST(shader_parameter_reflection) {
            const auto svc = IShaderService::get();
            PPR_TEST_ASSERT(!svc->initialize());

            shader::SharedModule handle;
            const auto ec = svc->loadModuleFromSource("test_params", "test_params.slang", kParamShader, handle.writeRef());
            PPR_TEST_ASSERT(!ec);

            auto *module = handle.get();
            PPR_TEST_ASSERT(module != nullptr);

            shader::ComPtr<slang::IEntryPoint> vertex_ep;
            PPR_TEST_ASSERT(SLANG_SUCCEEDED(module->findEntryPointByName("vertexMain", vertex_ep.writeRef())));

            shader::ComPtr<slang::IEntryPoint> fragment_ep;
            PPR_TEST_ASSERT(SLANG_SUCCEEDED(module->findEntryPointByName("fragmentMain", fragment_ep.writeRef())));

            slang::IComponentType *components[] = {module, vertex_ep.get(), fragment_ep.get()};
            shader::ComPtr<slang::IComponentType> composite;
            PPR_TEST_ASSERT(SLANG_SUCCEEDED(
                module->getSession()->createCompositeComponentType(components, 3, composite.writeRef())));

            shader::ComPtr<slang::IComponentType> linked_program;
            PPR_TEST_ASSERT(SLANG_SUCCEEDED(composite->link(linked_program.writeRef())));

            auto *layout = linked_program->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);

            // Find g_constants in global parameters
            bool found = false;
            const auto count = layout->getParameterCount();
            PPR_TEST_ASSERT(count > 0);
            for (unsigned i = 0; i < count; ++i) {
                auto *param = layout->getParameterByIndex(i);
                PPR_TEST_ASSERT(param != nullptr);
                if (std::string_view(param->getName()) == "g_constants") {
                    found = true;
                    auto *type_layout = param->getTypeLayout();
                    PPR_TEST_ASSERT(type_layout != nullptr);
                    auto *type = param->getType();
                    PPR_TEST_ASSERT(type != nullptr);
                    PPR_TEST_ASSERT(type->getKind() == slang::TypeReflection::Kind::ConstantBuffer);

                    auto *element_layout = type_layout->getElementTypeLayout();
                    PPR_TEST_ASSERT(element_layout != nullptr);
                    PPR_TEST_ASSERT(element_layout->getSize() > 0);
                    break;
                }
            }
            PPR_TEST_ASSERT(found);
            std::ignore = svc->shutdown();
        };

        PPR_UNIT_TEST(set_target_format_lifecycle) {
            const auto svc = IShaderService::get();
            std::ignore = svc->shutdown();

            // Not initialized yet — target format must fail with uninitialized
            PPR_TEST_ASSERT(svc->setTargetFormat(SLANG_DXBC) == make_error_code(shader::errc::uninitialized));

            PPR_TEST_ASSERT(!svc->initialize());

            // Setting the same format is a no-op
            PPR_TEST_ASSERT(!svc->setTargetFormat(SLANG_DXBC));

            // Changing the format before any module load recreates the session
            PPR_TEST_ASSERT(!svc->setTargetFormat(SLANG_SPIRV));

            {
                shader::SharedModule handle;
                PPR_TEST_ASSERT(!svc->loadModuleFromSource("target_probe", "target_probe.slang", kTriangleShader, handle.writeRef()));
                PPR_TEST_ASSERT(handle.get() != nullptr);

                // Changing the format after modules are loaded must fail
                PPR_TEST_ASSERT(svc->setTargetFormat(SLANG_DXBC) == make_error_code(shader::errc::invalid_arg));
            }

            // Restore a clean, default-target state for the remaining tests
            PPR_TEST_ASSERT(!svc->shutdown());
        };
    }

    // ------------------------------------------------------------------
    // Parent aggregator
    // ------------------------------------------------------------------
    PPR_UNIT_TEST(app_shader) {
        _.recurse({
            ShaderErrC::errc_enum_values,
            ShaderErrC::errc_category_name,
            ShaderErrC::errc_ok_not_error,
            ShaderErrC::errc_make_error_code_from_result,
            ShaderErrC::errc_result_convenience,
            ShaderErrC::errc_error_condition_mapping,
            RhiErrC::rhi_error_category_name,
            RhiErrC::rhi_make_error_code,
            RhiErrC::rhi_error_condition_mapping,
            ModHandle::diagnose_default_construct,
            ModHandle::module_handle_default,
            ShaderComp::compile_simple_triangle_shader,
            ShaderComp::compile_invalid_shader_recovery,
            ShaderComp::shader_parameter_reflection,
            ShaderComp::set_target_format_lifecycle,
        });
    };
}
