# Opt-in spatial prerequisites at the reviewed Slang-RHI pin, enabled after the
# independent surface patch and before public include paths are marked SYSTEM.
include_guard(GLOBAL)

set(_msaa_original_header "${slang-rhi_SOURCE_DIR}/include/slang-rhi.h")
set(_msaa_original_device "${slang-rhi_SOURCE_DIR}/src/vulkan/vk-device.cpp")
set(_msaa_original_builder "${slang-rhi_SOURCE_DIR}/src/vulkan/vk-texture.cpp")
file(READ "${_msaa_original_header}" _msaa_header)
file(READ "${_msaa_original_device}" _msaa_device)
file(READ "${_msaa_original_builder}" _msaa_builder)
string(REPLACE "\r\n" "\n" _msaa_header "${_msaa_header}")
string(REPLACE "\r\n" "\n" _msaa_device "${_msaa_device}")
string(REPLACE "\r\n" "\n" _msaa_builder "${_msaa_builder}")
string(SHA256 _msaa_header_hash "${_msaa_header}")
string(SHA256 _msaa_device_hash "${_msaa_device}")
string(SHA256 _msaa_builder_hash "${_msaa_builder}")
if(NOT _msaa_header_hash STREQUAL "31bd6ad07b48543a1026b2abab5ba582781d691cfdf9394b3a6a2013e47a69eb")
    message(FATAL_ERROR "Slang-RHI MSAA public header differs from the reviewed pin")
endif()
if(NOT _msaa_device_hash STREQUAL "274541a3e60b84913c24358401a9a09d718946b382ad98c1e47f8a34093a457d")
    message(FATAL_ERROR "Slang-RHI MSAA Vulkan device differs from the reviewed pin")
endif()
if(NOT _msaa_builder_hash STREQUAL "10797bb78d5444f804982e5e7c9f0d92befc28190c19dc6bfa1dc826b3cb6a9d")
    message(FATAL_ERROR "Slang-RHI MSAA native image builder differs from the reviewed pin")
endif()

function(_ppr_msaa_replace_once variable before after)
    set(source "${${variable}}")
    string(LENGTH "${source}" original_length)
    string(LENGTH "${before}" anchor_length)
    string(REPLACE "${before}" "" without_anchor "${source}")
    string(LENGTH "${without_anchor}" remaining_length)
    math(EXPR removed_length "${original_length} - ${remaining_length}")
    if(NOT removed_length EQUAL anchor_length)
        message(FATAL_ERROR "Slang-RHI MSAA replacement anchor must occur exactly once")
    endif()

    string(REPLACE "${before}" "${after}" source "${source}")
    set(${variable} "${source}" PARENT_SCOPE)
endfunction()

# Append at the end: existing Feature values retain their numeric identities.
# All backends see the declaration; only Vulkan advertises this feature.
set(_msaa_feature_old [=[    x(AtomicBfloat16,                           "atomic-bfloat16"                               )
// clang-format on]=])
set(_msaa_feature_new [=[    x(AtomicBfloat16,                           "atomic-bfloat16"                               ) \
    /* PPR: Vulkan standard indexed sample locations, without programmable overrides. */ \
    x(StandardSampleLocations4,                "standard-sample-locations-4"                   ) \
    x(SampleRateShading,                       "sample-rate-shading"                            ) \
    x(IndependentBlendEnabled,                 "independent-blend-enabled"                      )
// clang-format on]=])
_ppr_msaa_replace_once(_msaa_header "${_msaa_feature_old}" "${_msaa_feature_new}")

set(_msaa_capability_old [=[    addFeature(Feature::Surface);
    addFeature(Feature::ParameterBlock);]=])
set(_msaa_capability_new [=[    addFeature(Feature::Surface);

    if (basicProps.limits.standardSampleLocations)
    {
        // PPR: the core Vulkan indexed table applies; no sample-location override is encoded.
        addFeature(Feature::StandardSampleLocations4);
    }

    if (!desc.existingDeviceHandles.handles[2] && m_api.m_deviceFeatures.sampleRateShading)
    {
        // PPR: the supported base features were enabled by this device's creation path.
        addFeature(Feature::SampleRateShading);
    }

    if (!desc.existingDeviceHandles.handles[2] && m_api.m_deviceFeatures.independentBlend)
    {
        // PPR: this exact base-feature structure is passed to vkCreateDevice.
        // Imported devices have no enabled-feature proof and never advertise it.
        addFeature(Feature::IndependentBlendEnabled);
    }

    addFeature(Feature::ParameterBlock);]=])
_ppr_msaa_replace_once(_msaa_device "${_msaa_capability_old}" "${_msaa_capability_new}")

# Stay within committed descriptors used by the spatial lane: device-local,
# single-layer 2D/2DMS RGBA16F sampled colour attachments, D32 sampled depth
# attachments, and RGBA16F single-sample sampled storage output. Copy flags are
# included exactly when requested. Other descriptors retain the upstream path.
set(_msaa_allocation_old [=[Result DeviceImpl::getTextureAllocationInfo(const TextureDesc& desc_, Size* outSize, Size* outAlignment)
{
    TextureDesc desc = fixupTextureDesc(desc_);
]=])
set(_msaa_allocation_new [=[Result DeviceImpl::getTextureAllocationInfo(const TextureDesc& desc_, Size* outSize, Size* outAlignment)
{
    const uint32_t pprUsage = uint32_t(desc_.usage);
    const uint32_t pprCopy = uint32_t(TextureUsage::CopySource) | uint32_t(TextureUsage::CopyDestination);
    const uint32_t pprColor = uint32_t(TextureUsage::ShaderResource) | uint32_t(TextureUsage::RenderTarget);
    const uint32_t pprDepth = uint32_t(TextureUsage::ShaderResource) | uint32_t(TextureUsage::DepthStencil);
    const uint32_t pprStorage = uint32_t(TextureUsage::ShaderResource) | uint32_t(TextureUsage::UnorderedAccess);
    const bool pprColorAttachment = desc_.format == Format::RGBA16Float and (pprUsage & ~pprCopy) == pprColor;
    const bool pprDepthAttachment = desc_.format == Format::D32Float and (pprUsage & ~pprCopy) == pprDepth;
    const bool pprStorageOutput = desc_.format == Format::RGBA16Float and (pprUsage & ~pprCopy) == pprStorage;
    const bool pprScoped = desc_.next == nullptr and desc_.heap == nullptr and desc_.heapOffset == 0 and
        desc_.memoryType == MemoryType::DeviceLocal and
        desc_.sampleQuality == 0 and (desc_.type == TextureType::Texture2D or desc_.type == TextureType::Texture2DMS) and
        (pprColorAttachment or pprDepthAttachment or pprStorageOutput);
    if (pprScoped)
    {
        // Validate before normalization and before any Vulkan query. In particular
        // sample-count flags are single-bit enum values, not arbitrary integers.
        const uint32_t samples = desc_.sampleCount;
        if (outSize == nullptr or outAlignment == nullptr or desc_.size.width == 0 or desc_.size.height == 0 or
            desc_.size.depth != 1 or desc_.arrayLength != 1 or desc_.mipCount == 0 or samples == 0 or
            samples > 64 or (samples & (samples - 1)) != 0 or
            (desc_.type == TextureType::Texture2D and samples != 1))
        {
            return SLANG_E_INVALID_ARG;
        }

        // Multisampled storage requires a separately enabled optional feature.
        // This bounded contract admits only the single-sample storage output.
        if (pprStorageOutput and samples != 1)
        {
            return SLANG_E_NOT_AVAILABLE;
        }
    }

    TextureDesc desc = fixupTextureDesc(desc_);
    if (pprScoped and (desc.mipCount == 0 or desc.mipCount > calcMipCount(desc) or
        (desc.type == TextureType::Texture2DMS and desc.mipCount != 1)))
    {
        return SLANG_E_INVALID_ARG;
    }
]=])
_ppr_msaa_replace_once(_msaa_device "${_msaa_allocation_old}" "${_msaa_allocation_new}")

set(_msaa_create_old [=[    VkImage image;
    SLANG_VK_RETURN_ON_FAIL_REPORT(m_api.vkCreateImage(m_device, &imageInfo, nullptr, &image), this);]=])
set(_msaa_create_new [=[    if (pprScoped)
    {
        // Query the actual pin's shared getVkImageCreateInfo(desc, false) result.
        // Committed creation without initial data uses the same builder inputs;
        // placed heaps and their implicit transfer-destination usage are unscoped.
        VkPhysicalDeviceImageFormatInfo2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        query.format = imageInfo.format;
        query.type = imageInfo.imageType;
        query.tiling = imageInfo.tiling;
        query.usage = imageInfo.usage;
        query.flags = imageInfo.flags;
        VkImageFormatProperties2 support = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        const VkResult queryResult = m_api.vkGetPhysicalDeviceImageFormatProperties2(
            m_api.m_physicalDevice, &query, &support
        );
        if (queryResult == VK_ERROR_FORMAT_NOT_SUPPORTED)
        {
            return SLANG_E_NOT_AVAILABLE;
        }

        SLANG_VK_RETURN_ON_FAIL_REPORT(queryResult, this);

        const VkImageFormatProperties& limits = support.imageFormatProperties;
        if ((limits.sampleCounts & imageInfo.samples) == 0 or imageInfo.extent.width > limits.maxExtent.width or
            imageInfo.extent.height > limits.maxExtent.height or imageInfo.extent.depth > limits.maxExtent.depth or
            imageInfo.mipLevels > limits.maxMipLevels or imageInfo.arrayLayers > limits.maxArrayLayers)
        {
            return SLANG_E_NOT_AVAILABLE;
        }

        if ((pprColorAttachment or pprDepthAttachment) and
            (imageInfo.extent.width > m_info.limits.maxFramebufferDimensions[0] or
             imageInfo.extent.height > m_info.limits.maxFramebufferDimensions[1] or
             imageInfo.arrayLayers > m_info.limits.maxFramebufferDimensions[2]))
        {
            return SLANG_E_NOT_AVAILABLE;
        }
    }

    VkImage image;
    SLANG_VK_RETURN_ON_FAIL_REPORT(m_api.vkCreateImage(m_device, &imageInfo, nullptr, &image), this);]=])
_ppr_msaa_replace_once(_msaa_device "${_msaa_create_old}" "${_msaa_create_new}")

set(_msaa_directory "${CMAKE_CURRENT_BINARY_DIR}/ppr-slang-rhi-msaa")
set(_msaa_include "${_msaa_directory}/include")
file(MAKE_DIRECTORY "${_msaa_include}")

# A complete public-header overlay preserves quoted sibling includes. Remove the
# original public include root from both build and consumer search paths, rather
# than relying on precedence between one patched header and original siblings.
file(GLOB_RECURSE _msaa_public_headers CONFIGURE_DEPENDS RELATIVE "${slang-rhi_SOURCE_DIR}/include"
    "${slang-rhi_SOURCE_DIR}/include/*")
foreach(_msaa_public_header IN LISTS _msaa_public_headers)
    if(NOT _msaa_public_header STREQUAL "slang-rhi.h")
        configure_file("${slang-rhi_SOURCE_DIR}/include/${_msaa_public_header}"
            "${_msaa_include}/${_msaa_public_header}" COPYONLY)
    endif()
endforeach()

file(WRITE "${_msaa_directory}/slang-rhi.h.in" "${_msaa_header}")
configure_file("${_msaa_directory}/slang-rhi.h.in" "${_msaa_include}/slang-rhi.h" COPYONLY)
foreach(_msaa_property INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    get_target_property(_msaa_includes slang-rhi "${_msaa_property}")
    if(_msaa_includes)
        set(_msaa_kept_includes)
        foreach(_msaa_entry IN LISTS _msaa_includes)
            if(NOT _msaa_entry STREQUAL "${slang-rhi_SOURCE_DIR}/include")
                string(FIND "${_msaa_entry}" "${slang-rhi_SOURCE_DIR}/include" _msaa_original_route)
                if(NOT _msaa_original_route EQUAL -1)
                    message(FATAL_ERROR "Unexpected Slang-RHI public include route defeats the coherent overlay")
                endif()
                list(APPEND _msaa_kept_includes "${_msaa_entry}")
            endif()
        endforeach()
        set_property(TARGET slang-rhi PROPERTY "${_msaa_property}" "${_msaa_kept_includes}")
    endif()
endforeach()
target_include_directories(slang-rhi SYSTEM BEFORE PUBLIC "${_msaa_include}")

if(SLANG_RHI_ENABLE_VULKAN)
    # Dynamic rendering resolves into a color attachment, not an explicit
    # transfer resolve destination. Match the existing COLOR_ATTACHMENT_OPTIMAL
    # resolveImageLayout and RenderTarget stage/access; leave transfer APIs alone.
    set(_msaa_original_command "${slang-rhi_SOURCE_DIR}/src/vulkan/vk-command.cpp")
    file(READ "${_msaa_original_command}" _msaa_command)
    string(REPLACE "\r\n" "\n" _msaa_command "${_msaa_command}")
    string(SHA256 _msaa_command_hash "${_msaa_command}")
    if(NOT _msaa_command_hash STREQUAL "bcc7fbd26ff81dda826cff4c0c675effd1ec10ab571ec0d87e5cd78860740a24")
        message(FATAL_ERROR "Slang-RHI MSAA Vulkan command source differs from the reviewed pin")
    endif()

    set(_msaa_resolve_old [=[        if (resolveView)
            requireTextureState(
                resolveView->m_texture,
                resolveView->m_desc.subresourceRange,
                ResourceState::ResolveDestination
            );]=])
    set(_msaa_resolve_new [=[        if (resolveView)
            requireTextureState(
                resolveView->m_texture,
                resolveView->m_desc.subresourceRange,
                ResourceState::RenderTarget
            );]=])
    _ppr_msaa_replace_once(_msaa_command "${_msaa_resolve_old}" "${_msaa_resolve_new}")

    file(WRITE "${_msaa_directory}/vk-command.cpp.in" "${_msaa_command}")
    configure_file("${_msaa_directory}/vk-command.cpp.in" "${_msaa_directory}/vk-command.cpp" COPYONLY)
    file(WRITE "${_msaa_directory}/vk-device.cpp.in" "${_msaa_device}")
    configure_file("${_msaa_directory}/vk-device.cpp.in" "${_msaa_directory}/vk-device.cpp" COPYONLY)

    get_target_property(_msaa_sources slang-rhi SOURCES)
    set(_msaa_replaced FALSE)
    set(_msaa_command_replaced 0)
    foreach(_msaa_source IN LISTS _msaa_sources)
        if(_msaa_source STREQUAL "src/vulkan/vk-device.cpp" OR _msaa_source STREQUAL "${_msaa_original_device}")
            list(APPEND _msaa_patched_sources "${_msaa_directory}/vk-device.cpp")
            set(_msaa_replaced TRUE)
        elseif(_msaa_source STREQUAL "src/vulkan/vk-command.cpp" OR _msaa_source STREQUAL "${_msaa_original_command}")
            list(APPEND _msaa_patched_sources "${_msaa_directory}/vk-command.cpp")
            math(EXPR _msaa_command_replaced "${_msaa_command_replaced} + 1")
        else()
            list(APPEND _msaa_patched_sources "${_msaa_source}")
        endif()
    endforeach()
    if(NOT _msaa_replaced)
        message(FATAL_ERROR "Slang-RHI MSAA Vulkan device source registration was not found")
    endif()
    if(NOT _msaa_command_replaced EQUAL 1)
        message(FATAL_ERROR "Slang-RHI MSAA Vulkan command source registration must occur exactly once")
    endif()

    set_property(TARGET slang-rhi PROPERTY SOURCES "${_msaa_patched_sources}")
    target_include_directories(slang-rhi PRIVATE "${slang-rhi_SOURCE_DIR}/src/vulkan")
endif()
