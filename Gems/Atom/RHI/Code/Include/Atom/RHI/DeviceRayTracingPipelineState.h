/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#pragma once

#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>
#include <Atom/RHI/DevicePipelineState.h>
#include <Atom/RHI.Reflect/Base.h>
#include <Atom/RHI/DeviceObject.h>

namespace AZ::RHI
{
    //! Contains ray tracing shaders used by the pipeline
    struct RayTracingShaderLibrary
    {
        RHI::PipelineStateDescriptorForRayTracing m_descriptor;

        AZ::Name m_rayGenerationShaderName;
        AZ::Name m_missShaderName;
        AZ::Name m_callableShaderName;
        AZ::Name m_closestHitShaderName;
        AZ::Name m_anyHitShaderName;
        AZ::Name m_intersectionShaderName;
    };
    using RayTracingShaderLibraryVector = AZStd::vector<RayTracingShaderLibrary>;

    //! Defines a hit group which consists of a ClosestHit and/or an AnyHit shader
    struct RayTracingHitGroup
    {
        AZ::Name m_hitGroupName;
        AZ::Name m_closestHitShaderName;
        AZ::Name m_anyHitShaderName;
        AZ::Name m_intersectionShaderName;
    };
    using RayTracingHitGroupVector = AZStd::vector<RayTracingHitGroup>;

    //! Defines ray tracing pipeline settings
    struct RayTracingConfiguration
    {
        static const uint32_t MaxPayloadSizeDefault = 16;
        uint32_t m_maxPayloadSize = MaxPayloadSizeDefault;

        static const uint32_t MaxAttributeSizeDefault = 8;
        uint32_t m_maxAttributeSize = MaxAttributeSizeDefault;

        static const uint32_t MaxRecursionDepthDefault = 1;
        uint32_t m_maxRecursionDepth = MaxRecursionDepthDefault;
    };

    //! DeviceRayTracingPipelineStateDescriptor
    //!
    //! Describes a single-device ray tracing pipeline state.
    struct DeviceRayTracingPipelineStateDescriptor
    {
        RayTracingConfiguration m_configuration;
        RayTracingShaderLibraryVector m_shaderLibraries;
        RayTracingHitGroupVector m_hitGroups;
        const RHI::DevicePipelineState* m_pipelineState = nullptr;
    };

    //! Defines the shaders, hit groups, and other parameters required for ray tracing operations.
    class DeviceRayTracingPipelineState
        : public DeviceObject
    {
    public:
        DeviceRayTracingPipelineState() = default;
        virtual ~DeviceRayTracingPipelineState() = default;

        static RHI::Ptr<RHI::DeviceRayTracingPipelineState> CreateRHIRayTracingPipelineState();

        const DeviceRayTracingPipelineStateDescriptor& GetDescriptor() const { return m_descriptor; }
        ResultCode Init(Device& device, const DeviceRayTracingPipelineStateDescriptor* descriptor);

    private:

        // explicit shutdown is not allowed for this type
        void Shutdown() override final;

        //////////////////////////////////////////////////////////////////////////
        // Platform API
        virtual RHI::ResultCode InitInternal(RHI::Device& deviceBase, const RHI::DeviceRayTracingPipelineStateDescriptor* descriptor) = 0;
        virtual void ShutdownInternal() = 0;
        //////////////////////////////////////////////////////////////////////////

        DeviceRayTracingPipelineStateDescriptor m_descriptor;
    };
}
