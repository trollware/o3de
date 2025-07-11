/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#pragma once

#include <GraphCanvas/Widgets/EditorContextMenu/ContextMenuActions/ContextMenuAction.h>

#include <GraphCanvas/Components/SceneBus.h>

namespace GraphCanvas
{
    class BreakpointContextMenuAction : public ContextMenuAction
    {
    protected:
        BreakpointContextMenuAction(AZStd::string_view actionName, QObject* parent)
            : ContextMenuAction(actionName, parent)
        {
        }

        using ContextMenuAction::RefreshAction;
        void RefreshAction([[maybe_unused]] const GraphId& graphId, [[maybe_unused]] const AZ::EntityId& targetId) override
        {
            /*
            bool hasSelectedItems = false;
            SceneRequestBus::EventResult(hasSelectedItems, graphId, &SceneRequests::HasSelectedItems);

            setEnabled(hasSelectedItems);
            */

            setEnabled(false);
            setToolTip("Feature in development. Adding breakpoint works but will freeze the editor when breakpoint is hit.");
        }

    public:
        static ActionGroupId GetBreakpointContextMenuActionGroupId()
        {
            return AZ_CRC_CE("BreakpointActionGroup");
        }

        ActionGroupId GetActionGroupId() const override
        {
            return GetBreakpointContextMenuActionGroupId();
        }
    };
} // namespace GraphCanvas
