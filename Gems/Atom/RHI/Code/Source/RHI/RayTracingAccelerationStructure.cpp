/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Atom/RHI/Factory.h>
#include <Atom/RHI/Buffer.h>
#include <Atom/RHI/RayTracingAccelerationStructure.h>
#include <Atom/RHI/RayTracingBufferPools.h>
#include <Atom/RHI/RHISystemInterface.h>

namespace AZ::RHI
{
    DeviceRayTracingBlasDescriptor RayTracingBlasDescriptor::GetDeviceRayTracingBlasDescriptor(int deviceIndex) const
    {
        AZ_Assert(
            m_geometries.empty() || !m_aabb.has_value(), "Triangle geometry and procedural geometry must not be used in the same BLAS");

        DeviceRayTracingBlasDescriptor descriptor;

        for (const auto& geometry : m_geometries)
        {
            DeviceRayTracingGeometry& deviceGeometry = descriptor.m_geometries.emplace_back();
            deviceGeometry.m_vertexFormat = geometry.m_vertexFormat;
            deviceGeometry.m_vertexBuffer = geometry.m_vertexBuffer.GetDeviceStreamBufferView(deviceIndex);
            deviceGeometry.m_indexBuffer = geometry.m_indexBuffer.GetDeviceIndexBufferView(deviceIndex);
        }

        if (m_aabb.has_value())
        {
            descriptor.m_aabb = m_aabb;
        }

        descriptor.m_buildFlags = m_buildFlags;

        return descriptor;
    }

    DeviceRayTracingTlasDescriptor RayTracingTlasDescriptor::GetDeviceRayTracingTlasDescriptor(int deviceIndex) const
    {
        AZ_Assert(m_instances.empty() || !m_instancesBuffer, "Instance list and instance buffer must not be used in the same TLAS");

        DeviceRayTracingTlasDescriptor descriptor;

        for (const auto& instance : m_instances)
        {
            auto& tlasInstance = descriptor.m_instances.emplace_back();

            tlasInstance.m_instanceID = instance.m_instanceID;
            tlasInstance.m_hitGroupIndex = instance.m_hitGroupIndex;
            tlasInstance.m_instanceMask = instance.m_instanceMask;
            tlasInstance.m_transform = instance.m_transform;
            tlasInstance.m_nonUniformScale = instance.m_nonUniformScale;
            tlasInstance.m_transparent = instance.m_transparent;
            tlasInstance.m_blas = instance.m_blas->GetDeviceRayTracingBlas(deviceIndex);
        }

        if (m_instancesBuffer)
        {
            descriptor.m_instancesBuffer = m_instancesBuffer->GetDeviceBuffer(deviceIndex);
            descriptor.m_numInstancesInBuffer = m_numInstancesInBuffer;
        }

        return descriptor;
    }

    ResultCode RayTracingBlas::CreateBuffers(
        MultiDevice::DeviceMask deviceMask,
        const RayTracingBlasDescriptor* descriptor,
        const RayTracingBufferPools& rayTracingBufferPools)
    {
        m_descriptor = *descriptor;
        ResultCode resultCode{ ResultCode::Success };

        MultiDeviceObject::Init(deviceMask);

        IterateDevices(
            [this, &resultCode, &descriptor, &rayTracingBufferPools](auto deviceIndex)
            {
                auto device = RHISystemInterface::Get()->GetDevice(deviceIndex);
                this->m_deviceObjects[deviceIndex] = Factory::Get().CreateRayTracingBlas();

                auto deviceDescriptor{ descriptor->GetDeviceRayTracingBlasDescriptor(deviceIndex) };

                resultCode = GetDeviceRayTracingBlas(deviceIndex)
                                 ->CreateBuffers(
                                     *device, &deviceDescriptor, *rayTracingBufferPools.GetDeviceRayTracingBufferPools(deviceIndex).get());

                return resultCode == ResultCode::Success;
            });

        if (resultCode != ResultCode::Success)
        {
            // Reset already initialized device-specific DeviceRayTracingBlas and set deviceMask to 0
            m_deviceObjects.clear();
            MultiDeviceObject::Init(static_cast<MultiDevice::DeviceMask>(0u));
        }

        if (const auto& name = GetName(); !name.IsEmpty())
        {
            SetName(name);
        }

        return resultCode;
    }

    ResultCode RayTracingBlas::CreateCompactedBuffers(
        const RayTracingBlas& sourceBlas,
        const AZStd::unordered_map<int, uint64_t>& compactedSizes,
        const RayTracingBufferPools& rayTracingBufferPools)
    {
        m_descriptor = sourceBlas.m_descriptor;

        MultiDeviceObject::Init(sourceBlas.GetDeviceMask());
        ResultCode resultCode{ ResultCode::Success };

        IterateDevices(
            [&](auto deviceIndex)
            {
                auto device = RHISystemInterface::Get()->GetDevice(deviceIndex);
                this->m_deviceObjects[deviceIndex] = Factory::Get().CreateRayTracingBlas();

                resultCode = GetDeviceRayTracingBlas(deviceIndex)
                                 ->CreateCompactedBuffers(
                                     *device,
                                     sourceBlas.GetDeviceRayTracingBlas(deviceIndex),
                                     compactedSizes.at(deviceIndex),
                                     *rayTracingBufferPools.GetDeviceRayTracingBufferPools(deviceIndex).get());

                return resultCode == ResultCode::Success;
            });

        if (resultCode != ResultCode::Success)
        {
            // Reset already initialized device-specific DeviceRayTracingBlas and set deviceMask to 0
            m_deviceObjects.clear();
            MultiDeviceObject::Init(static_cast<MultiDevice::DeviceMask>(0u));
        }

        if (const auto& name = GetName(); !name.IsEmpty())
        {
            SetName(name);
        }

        return resultCode;
    }

    bool RayTracingBlas::IsValid() const
    {
        if (m_deviceObjects.empty())
        {
            return false;
        }

        IterateObjects<DeviceRayTracingBlas>(
            [](auto /*deviceIndex*/, auto deviceRayTracingBlas)
            {
                if (!deviceRayTracingBlas->IsValid())
                {
                    return false;
                }
                return true;
            });
        return true;
    }

    ResultCode RayTracingTlas::CreateBuffers(
        MultiDevice::DeviceMask deviceMask,
        const RayTracingTlasDescriptor* descriptor,
        const RayTracingBufferPools& rayTracingBufferPools)
    {
        m_descriptor = *descriptor;

        MultiDeviceObject::Init(deviceMask);

        ResultCode resultCode{ResultCode::Success};
        IterateDevices(
            [this, &descriptor, &rayTracingBufferPools, &resultCode](int deviceIndex)
            {
                auto device = RHISystemInterface::Get()->GetDevice(deviceIndex);
                if (!m_deviceObjects.contains(deviceIndex))
                {
                    this->m_deviceObjects[deviceIndex] = Factory::Get().CreateRayTracingTlas().get();
                }

                auto deviceDescriptor{ descriptor->GetDeviceRayTracingTlasDescriptor(deviceIndex) };

                resultCode = GetDeviceRayTracingTlas(deviceIndex)
                                 ->CreateBuffers(
                                     *device, &deviceDescriptor, *rayTracingBufferPools.GetDeviceRayTracingBufferPools(deviceIndex).get());
                return resultCode == ResultCode::Success;
            });

        if (resultCode != ResultCode::Success)
        {
            // Reset already initialized device-specific DeviceRayTracingTlas and set deviceMask to 0
            m_deviceObjects.clear();
            MultiDeviceObject::Init(static_cast<MultiDevice::DeviceMask>(0u));
        }

        if (const auto& name = GetName(); !name.IsEmpty())
        {
            SetName(name);
        }

        // Each call to CreateBuffers advances m_currentBufferIndex internally, reset buffers to always receive currently active
        m_tlasBuffer.reset();
        m_tlasInstancesBuffer.reset();

        return resultCode;
    }

    const RHI::Ptr<RHI::Buffer> RayTracingTlas::GetTlasBuffer() const
    {
        AZStd::lock_guard lock(m_tlasBufferMutex);
        if (m_deviceObjects.empty())
        {
            return nullptr;
        }

        if (m_tlasBuffer)
        {
            return m_tlasBuffer;
        }

        m_tlasBuffer = aznew RHI::Buffer;
        m_tlasBuffer->Init(GetDeviceMask());

        IterateObjects<DeviceRayTracingTlas>(
            [this](int deviceIndex, auto deviceRayTracingTlas)
            {
                m_tlasBuffer->m_deviceObjects[deviceIndex] = deviceRayTracingTlas->GetTlasBuffer();

                if (!m_tlasBuffer->m_deviceObjects[deviceIndex])
                {
                    m_tlasBuffer->m_deviceObjects.clear();
                    m_tlasBuffer = nullptr;
                    return ResultCode::Fail;
                }

                m_tlasBuffer->SetDescriptor(m_tlasBuffer->GetDeviceBuffer(deviceIndex)->GetDescriptor());
                return ResultCode::Success;
            });

        if (m_tlasBuffer)
        {
            if (const auto& name = m_tlasBuffer->GetName(); !name.IsEmpty())
            {
                m_tlasBuffer->SetName(name);
            }
        }

        return m_tlasBuffer;
    }

    const RHI::Ptr<RHI::Buffer> RayTracingTlas::GetTlasInstancesBuffer() const
    {
        AZStd::lock_guard lock(m_tlasInstancesBufferMutex);

        if (m_deviceObjects.empty())
        {
            return nullptr;
        }

        if (m_tlasInstancesBuffer)
        {
            return m_tlasInstancesBuffer;
        }

        m_tlasInstancesBuffer = aznew RHI::Buffer;
        m_tlasInstancesBuffer->Init(GetDeviceMask());

        IterateObjects<DeviceRayTracingTlas>(
            [this](int deviceIndex, auto deviceRayTracingTlas)
            {
                m_tlasInstancesBuffer->m_deviceObjects[deviceIndex] = deviceRayTracingTlas->GetTlasBuffer();

                if (!m_tlasInstancesBuffer->m_deviceObjects[deviceIndex])
                {
                    m_tlasInstancesBuffer->m_deviceObjects.clear();
                    m_tlasInstancesBuffer = nullptr;
                    return ResultCode::Fail;
                }

                m_tlasInstancesBuffer->SetDescriptor(m_tlasInstancesBuffer->GetDeviceBuffer(deviceIndex)->GetDescriptor());
                return ResultCode::Success;
            });

        if (m_tlasInstancesBuffer)
        {
            if (const auto& name = m_tlasInstancesBuffer->GetName(); !name.IsEmpty())
            {
                m_tlasInstancesBuffer->SetName(name);
            }
        }

        return m_tlasInstancesBuffer;
    }
} // namespace AZ::RHI
