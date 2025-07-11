/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "DynamicPrimitiveProcessor.h"
#include "AuxGeomDrawProcessorShared.h"

#include <Atom/RHI/DrawPacketBuilder.h>
#include <Atom/RHI/Factory.h>
#include <Atom/RHI.Reflect/InputStreamLayoutBuilder.h>

#include <Atom/RPI.Reflect/Shader/ShaderAsset.h>
#include <Atom/RPI.Public/DynamicDraw/DynamicDrawInterface.h>
#include <Atom/RPI.Public/RPIUtils.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/Shader/Shader.h>
#include <Atom/RPI.Public/View.h>

#include <AzCore/Debug/Profiler.h>

namespace AZ
{
    namespace Render
    {
        namespace
        {
            static const RHI::PrimitiveTopology PrimitiveTypeToTopology[PrimitiveType_Count] =
            {
                RHI::PrimitiveTopology::PointList,
                RHI::PrimitiveTopology::LineList,
                RHI::PrimitiveTopology::TriangleList,
            };
        }

        bool DynamicPrimitiveProcessor::Initialize(const AZ::RPI::Scene* scene)
        {
            for (int primitiveType = 0; primitiveType < PrimitiveType_Count; ++primitiveType)
            {
                SetupInputStreamLayout(m_inputStreamLayout[primitiveType], PrimitiveTypeToTopology[primitiveType]);
                m_streamBufferViewsValidatedForLayout[primitiveType] = false;
            }

            m_scene = scene;
            InitShader();

            return true;
        }

        void DynamicPrimitiveProcessor::Release()
        {
            m_drawPackets.clear();
            m_processSrgs.clear();
            m_shaderData.m_defaultSRG = nullptr;

            m_shader = nullptr;
            m_scene = nullptr;

            for (RPI::Ptr<RPI::PipelineStateForDraw>* pipelineState : m_createdPipelineStates)
            {
                pipelineState->reset();
            }
            m_createdPipelineStates.clear();
        }

        void DynamicPrimitiveProcessor::PrepareFrame()
        {
            if (m_needUpdatePipelineStates)
            {
                // for created pipeline state, re-set their data from scene
                for (RPI::Ptr<RPI::PipelineStateForDraw>* pipelineState : m_createdPipelineStates)
                {
                    (*pipelineState)->SetOutputFromScene(m_scene);
                    (*pipelineState)->Finalize();
                }
                m_needUpdatePipelineStates = false;
            }
        }

        void DynamicPrimitiveProcessor::FrameEnd()
        {
            m_processSrgs.clear();
            m_drawPackets.clear();
        }

        void DynamicPrimitiveProcessor::ProcessDynamicPrimitives(AuxGeomBufferData* bufferData, const RPI::FeatureProcessor::RenderPacket& fpPacket)
        {
            AZ_PROFILE_SCOPE(AzRender, "DynamicPrimitiveProcessor: ProcessDynamicPrimitives");
            RHI::DrawPacketBuilder drawPacketBuilder{RHI::MultiDevice::AllDevices};

            DynamicPrimitiveData& srcPrimitives = bufferData->m_primitiveData;
            // Update the buffers for the dynamic primitives and generate draw packets for them
            if (srcPrimitives.m_indexBuffer.size() > 0)
            {
                // Update the buffers for all dynamic primitives in this frame's data
                // There is just one index buffer and one vertex buffer for all dynamic primitives
                if (!UpdateIndexBuffer(srcPrimitives.m_indexBuffer)
                    || !UpdateVertexBuffer(srcPrimitives.m_vertexBuffer))
                {
                    // Skip adding render data if failed to update buffers
                    // Note, the error would be already reported inside the Update* functions
                    return;
                }

                // Validate the stream buffer views for all stream layout's if necessary
                for (int primitiveType = 0; primitiveType < PrimitiveType_Count; ++primitiveType)
                {
                    ValidateStreamBufferViews(m_geometryView.GetStreamBufferViews(), m_streamBufferViewsValidatedForLayout, primitiveType);
                }

                // Loop over all the primitives and use one draw call for each AuxGeom API call
                // We have to create separate draw packets for each view that the AuxGeom is in (typically only one)
                AZStd::vector<RPI::ViewPtr> auxGeomViews;
                for (auto& view : fpPacket.m_views)
                {
                    // If this view is ignoring packets with our draw list tag then skip this view
                    if (!view->HasDrawListTag(m_shaderData.m_drawListTag))
                    {
                        continue;
                    }
                    auxGeomViews.emplace_back(view);
                }

                for (auto& primitive : srcPrimitives.m_primitiveBuffer)
                {
                    bool useManualViewProjectionOverride = primitive.m_viewProjOverrideIndex != -1;

                    PipelineStateOptions pipelineStateOptions;
                    pipelineStateOptions.m_perpectiveType = useManualViewProjectionOverride? PerspectiveType_ManualOverride : PerspectiveType_ViewProjection;
                    pipelineStateOptions.m_blendMode = primitive.m_blendMode;
                    pipelineStateOptions.m_primitiveType = primitive.m_primitiveType;
                    pipelineStateOptions.m_depthReadType = primitive.m_depthReadType;
                    pipelineStateOptions.m_depthWriteType = primitive.m_depthWriteType;
                    pipelineStateOptions.m_faceCullMode = primitive.m_faceCullMode;
                    RPI::Ptr<RPI::PipelineStateForDraw> pipelineState = GetPipelineState(pipelineStateOptions);

                    Data::Instance<RPI::ShaderResourceGroup> srg;
                    if (useManualViewProjectionOverride || primitive.m_primitiveType == PrimitiveType_PointList)
                    {
                        srg = RPI::ShaderResourceGroup::Create(m_shader->GetAsset(), m_shader->GetSupervariantIndex(), m_shaderData.m_perDrawSrgLayout->GetName());
                        if (!srg)
                        {
                            AZ_Warning("AuxGeom", false, "Failed to create a shader resource group for an AuxGeom draw, Ignoring the draw");
                            continue; // failed to create an srg for this draw, just drop the draw.
                        }
                        if (useManualViewProjectionOverride)
                        {
                            srg->SetConstant(m_shaderData.m_viewProjectionOverrideIndex, bufferData->m_viewProjOverrides[primitive.m_viewProjOverrideIndex]);
                            m_shaderData.m_viewProjectionOverrideIndex.AssertValid();
                        }
                        if (primitive.m_primitiveType == PrimitiveType_PointList)
                        {
                            srg->SetConstant(m_shaderData.m_pointSizeIndex, aznumeric_cast<float>(primitive.m_width));
                            m_shaderData.m_pointSizeIndex.AssertValid();
                        }
                        pipelineState->UpdateSrgVariantFallback(srg);
                        srg->Compile();
                    }
                    else
                    {
                        srg = m_shaderData.m_defaultSRG;
                    }

                    primitive.m_geometryView = m_geometryView;
                    primitive.m_geometryView.SetDrawArguments(RHI::DrawIndexed(0, primitive.m_indexCount, primitive.m_indexOffset));

                    for (auto& view : auxGeomViews)
                    {
                        RHI::DrawItemSortKey sortKey = primitive.m_blendMode == BlendMode_Off ? 0 : view->GetSortKeyForPosition(primitive.m_center);

                        auto drawPacket = BuildDrawPacketForDynamicPrimitive(
                            primitive.m_geometryView,
                            pipelineState,
                            srg,
                            drawPacketBuilder,
                            sortKey);

                        if (drawPacket)
                        {
                            m_drawPackets.emplace_back(drawPacket);
                            m_processSrgs.push_back(srg);
                            view->AddDrawPacket(drawPacket.get());
                        }
                    }
                }
            }
        }

        bool DynamicPrimitiveProcessor::UpdateIndexBuffer(const IndexBuffer& source)
        {
            const size_t sourceByteSize = source.size() * sizeof(AuxGeomIndex);
            
            RHI::Ptr<RPI::DynamicBuffer> dynamicBuffer = RPI::DynamicDrawInterface::Get()->GetDynamicBuffer(static_cast<uint32_t>(sourceByteSize), RHI::Alignment::InputAssembly);
            if (!dynamicBuffer)
            {
                AZ_WarningOnce("AuxGeom", false, "Failed to allocate dynamic buffer of size %d.", sourceByteSize);
                return false;
            }
            dynamicBuffer->Write(source.data(), static_cast<uint32_t>(sourceByteSize));
            m_geometryView.SetIndexBufferView(dynamicBuffer->GetIndexBufferView(RHI::IndexFormat::Uint32));
            return true;
        }

        bool DynamicPrimitiveProcessor::UpdateVertexBuffer(const VertexBuffer& source)
        {
            const size_t sourceByteSize = source.size() * sizeof(AuxGeomDynamicVertex);

            RHI::Ptr<RPI::DynamicBuffer> dynamicBuffer = RPI::DynamicDrawInterface::Get()->GetDynamicBuffer(static_cast<uint32_t>(sourceByteSize), RHI::Alignment::InputAssembly);
            if (!dynamicBuffer)
            {
                AZ_WarningOnce("AuxGeom", false, "Failed to allocate dynamic buffer of size %d.", sourceByteSize);
                return false;
            }
            dynamicBuffer->Write(source.data(), static_cast<uint32_t>(sourceByteSize));
            if (m_geometryView.GetStreamBufferViews().empty())
            {
                m_geometryView.AddStreamBufferView(dynamicBuffer->GetStreamBufferView(sizeof(AuxGeomDynamicVertex)));
            }
            else
            {
                m_geometryView.SetStreamBufferView(0, dynamicBuffer->GetStreamBufferView(sizeof(AuxGeomDynamicVertex)));
            }
            return true;
        }

        void DynamicPrimitiveProcessor::ValidateStreamBufferViews(AZStd::span<const RHI::StreamBufferView> streamBufferViews, bool* isValidated, int primitiveType)
        {
            if (!isValidated[primitiveType])
            {
                if (!RHI::ValidateStreamBufferViews(m_inputStreamLayout[primitiveType], streamBufferViews))
                {
                    AZ_Error("DynamicPrimitiveProcessor", false, "Failed to validate the stream buffer views");
                    return;
                }
                else
                {
                    isValidated[primitiveType] = true;
                }
            }
        }

        void DynamicPrimitiveProcessor::SetupInputStreamLayout(RHI::InputStreamLayout& inputStreamLayout, RHI::PrimitiveTopology topology)
        {
            RHI::InputStreamLayoutBuilder layoutBuilder;
            layoutBuilder.AddBuffer()
                ->Channel("POSITION", RHI::Format::R32G32B32_FLOAT)
                ->Channel("COLOR", RHI::Format::R8G8B8A8_UNORM);
            layoutBuilder.SetTopology(topology);
            inputStreamLayout = layoutBuilder.End();
        }

        RPI::Ptr<RPI::PipelineStateForDraw>& DynamicPrimitiveProcessor::GetPipelineState(const PipelineStateOptions& pipelineStateOptions)
        {
            // The declaration: m_pipelineStates[PerspectiveType_Count][BlendMode_Count][PrimitiveType_Count][DepthRead_Count][DepthWrite_Count][FaceCull_Count];
            return m_pipelineStates[pipelineStateOptions.m_perpectiveType][pipelineStateOptions.m_blendMode][pipelineStateOptions.m_primitiveType]
                [pipelineStateOptions.m_depthReadType][pipelineStateOptions.m_depthWriteType][pipelineStateOptions.m_faceCullMode];
        }

        void DynamicPrimitiveProcessor::SetUpdatePipelineStates()
        {
            m_needUpdatePipelineStates = true;
        }

        void DynamicPrimitiveProcessor::InitPipelineState(const PipelineStateOptions& pipelineStateOptions)
        {
            // Use the the pipeline state for PipelineStateOptions with default values and input perspective type as base pipeline state. Create one if it was empty.
            PipelineStateOptions defaultOptions;
            defaultOptions.m_perpectiveType = pipelineStateOptions.m_perpectiveType;
            RPI::Ptr<RPI::PipelineStateForDraw>& basePipelineState = GetPipelineState(defaultOptions);
            if (basePipelineState.get() == nullptr)
            {
                basePipelineState = aznew RPI::PipelineStateForDraw;
                Name perspectiveTypeViewProjection = Name("ViewProjectionMode::ViewProjection");
                Name perspectiveTypeManualOverride = Name("ViewProjectionMode::ManualOverride");
                Name optionViewProjectionModeName = Name("o_viewProjMode");

                RPI::ShaderOptionList shaderOptionAndValues;
                shaderOptionAndValues.push_back(RPI::ShaderOption(optionViewProjectionModeName,
                    (pipelineStateOptions.m_perpectiveType == AuxGeomShapePerpectiveType::PerspectiveType_ViewProjection)?perspectiveTypeViewProjection: perspectiveTypeManualOverride));
                basePipelineState->Init(m_shader, &shaderOptionAndValues);

                m_createdPipelineStates.push_back(&basePipelineState);
            }

            RPI::Ptr<RPI::PipelineStateForDraw>& destPipelineState = GetPipelineState(pipelineStateOptions);

            // Copy from base pipeline state. Skip if it's the base pipeline state 
            if (destPipelineState.get() == nullptr)
            {
                destPipelineState = aznew RPI::PipelineStateForDraw(*basePipelineState.get());
                m_createdPipelineStates.push_back(&destPipelineState);
            }

            // blendMode
            RHI::TargetBlendState& blendState = destPipelineState->RenderStatesOverlay().m_blendState.m_targets[0];
            blendState.m_enable = pipelineStateOptions.m_blendMode == AuxGeomBlendMode::BlendMode_Alpha;
            blendState.m_blendSource = RHI::BlendFactor::AlphaSource;
            blendState.m_blendDest = RHI::BlendFactor::AlphaSourceInverse;

            // primitiveType
            destPipelineState->InputStreamLayout() = m_inputStreamLayout[pipelineStateOptions.m_primitiveType];

            // depthReadType
            // Keep the default depth comparison function and only set it when depth read is off
            // Note: since the default PipelineStateOptions::m_depthReadType is DepthRead_On, the basePipelineState keeps the comparison function read from shader variant
            if (pipelineStateOptions.m_depthReadType == AuxGeomDepthReadType::DepthRead_Off)
            {
                destPipelineState->RenderStatesOverlay().m_depthStencilState.m_depth.m_func = RHI::ComparisonFunc::Always;
            }

            // depthWriteType
            destPipelineState->RenderStatesOverlay().m_depthStencilState.m_depth.m_writeMask =
                ConvertToRHIDepthWriteMask(pipelineStateOptions.m_depthWriteType);

            // faceCullMode
            destPipelineState->RenderStatesOverlay().m_rasterState.m_cullMode = ConvertToRHICullMode(pipelineStateOptions.m_faceCullMode);

            destPipelineState->SetOutputFromScene(m_scene);
            destPipelineState->Finalize();
        }

        void DynamicPrimitiveProcessor::InitShader()
        {
            const char* auxGeomWorldShaderFilePath = "Shaders/auxgeom/auxgeomworld.azshader";

            m_shader = RPI::LoadCriticalShader(auxGeomWorldShaderFilePath);
            if (!m_shader)
            {
                AZ_Error("DynamicPrimitiveProcessor", false, "Failed to get shader");
                return;
            }

            // Get the per-object SRG and store the indices of the data we need to set per object
            m_shaderData.m_perDrawSrgLayout = m_shader->FindShaderResourceGroupLayout(RPI::SrgBindingSlot::Draw); 
            if (!m_shaderData.m_perDrawSrgLayout)
            {
                AZ_Error("DynamicPrimitiveProcessor", false, "Failed to get shader resource group layout");
                return;
            }

            m_shaderData.m_viewProjectionOverrideIndex.Reset();
            m_shaderData.m_pointSizeIndex.Reset();

            // Remember the draw list tag
            m_shaderData.m_drawListTag = m_shader->GetDrawListTag();

            // Create a default SRG for draws that don't use a manual view projection override
            m_shaderData.m_defaultSRG = RPI::ShaderResourceGroup::Create(m_shader->GetAsset(), m_shader->GetSupervariantIndex(), m_shaderData.m_perDrawSrgLayout->GetName());
            AZ_Assert(m_shaderData.m_defaultSRG != nullptr, "Creating the default SRG unexpectedly failed");
            m_shaderData.m_defaultSRG->SetConstant(m_shaderData.m_pointSizeIndex, 10.0f);
            m_shaderData.m_defaultSRG->Compile();

            // Initialize all pipeline states
            PipelineStateOptions pipelineStateOptions;
            // initialize two base pipeline state first to preserve the blend functions
            pipelineStateOptions.m_perpectiveType = PerspectiveType_ViewProjection;
            InitPipelineState(pipelineStateOptions);
            pipelineStateOptions.m_perpectiveType = PerspectiveType_ManualOverride;
            InitPipelineState(pipelineStateOptions);

            // Initialize all pipeline states
            for (uint32_t perspectiveType = 0; perspectiveType < PerspectiveType_Count; perspectiveType++)
            {
                pipelineStateOptions.m_perpectiveType = (AuxGeomShapePerpectiveType)perspectiveType;
                for (uint32_t blendMode = 0; blendMode < BlendMode_Count; blendMode++)
                {
                    pipelineStateOptions.m_blendMode = (AuxGeomBlendMode)blendMode;
                    for (uint32_t primitiveType = 0; primitiveType < PrimitiveType_Count; primitiveType++)
                    {
                        pipelineStateOptions.m_primitiveType = (AuxGeomPrimitiveType)primitiveType;
                        for (uint32_t depthRead = 0; depthRead < DepthRead_Count; depthRead++)
                        {
                            pipelineStateOptions.m_depthReadType = (AuxGeomDepthReadType)depthRead;
                            for (uint32_t depthWrite = 0; depthWrite < DepthWrite_Count; depthWrite++)
                            {
                                pipelineStateOptions.m_depthWriteType = (AuxGeomDepthWriteType)depthWrite;
                                for (uint32_t faceCullMode = 0; faceCullMode < FaceCull_Count; faceCullMode++)
                                {
                                    pipelineStateOptions.m_faceCullMode = (AuxGeomFaceCullMode)faceCullMode;
                                    InitPipelineState(pipelineStateOptions);
                                }
                            }
                        }
                    }
                }
            }
        }

        RHI::ConstPtr<RHI::DrawPacket> DynamicPrimitiveProcessor::BuildDrawPacketForDynamicPrimitive(
            RHI::GeometryView& geometryView,
            const RPI::Ptr<RPI::PipelineStateForDraw>& pipelineState,
            Data::Instance<RPI::ShaderResourceGroup> srg,
            RHI::DrawPacketBuilder& drawPacketBuilder,
            RHI::DrawItemSortKey sortKey)
        {
            drawPacketBuilder.Begin(nullptr);
            drawPacketBuilder.SetGeometryView(&geometryView);
            drawPacketBuilder.AddShaderResourceGroup(srg->GetRHIShaderResourceGroup());

            RHI::DrawPacketBuilder::DrawRequest drawRequest;
            drawRequest.m_listTag = m_shaderData.m_drawListTag;
            drawRequest.m_streamIndices = geometryView.GetFullStreamBufferIndices();
            drawRequest.m_pipelineState = pipelineState->GetRHIPipelineState();
            drawRequest.m_sortKey = sortKey;
            drawPacketBuilder.AddDrawItem(drawRequest);

            return drawPacketBuilder.End();
        }


    } // namespace Render
} // namespace AZ
