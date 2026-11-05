// SPDX-FileCopyrightText: 2024-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

import Waylib.Server
import Treeland
import Treeland.Protocols
import org.deepin.dtk 1.0 as D

Item {
    id: root
    property int aniDuration: 350
    property bool enableAnimation: aniDuration > 0
    property int spacing: 5

    readonly property int panelHorizontalPadding: PopoverStyle.tooltipHorizontalPadding
    readonly property int panelVerticalPadding: PopoverStyle.tooltipVerticalPadding
    readonly property int screenEdgeMargin: 10
    property bool enableBlur: true
    property bool enableShadow: GraphicsInfo.api !== GraphicsInfo.Software
    property bool enableBorder: true
    // set by show()
    property var target
    property point pos
    property int direction
    function isHorizontalDirection(direction) {
        return direction !== ForeignToplevelManagerInterfaceV3.PreviewDirection.left
            && direction !== ForeignToplevelManagerInterfaceV3.PreviewDirection.right;
    }
    readonly property bool isHorizontal: root.isHorizontalDirection(root.direction)
    // auto update by binding
    readonly property var outputItem: target?.ownsOutput?.outputItem ?? null
    readonly property int screenWidth: outputItem && outputItem.width > 0 ? outputItem.width
                                                                          : 9999
    readonly property int screenHeight: outputItem && outputItem.height > 0 ? outputItem.height
                                                                            : 9999
    property size outPutSize: Qt.size(root.screenWidth, root.screenHeight)
    property point outPutPos: {
        if (!outputItem || !root.parent)
            return Qt.point(0, 0);
        return outputItem.mapToItem(root.parent, 0, 0);
    }
    // set by showTooltip(): the client-drawn tooltip surface
    property var tooltipSurface: null
    onTooltipSurfaceChanged: root.applyTooltipSurface(root.tooltipSurface)

    property var liveTooltipLayer: null
    // Natural size of the tooltip that last left.
    property size lastTooltipSize: Qt.size(0, 0)

    property bool isShowing: false

    // Content layout the popover shows. It is a mode, not liveness
    enum Mode { Tooltip, Preview }
    property int mode: DockPreview.Mode.Tooltip
    readonly property bool tooltipLayout: root.mode === DockPreview.Mode.Tooltip
    property bool stripPinToLeft: true
    // The authoritative list of windows the preview shows.
    property var desiredSurfaces: []
    onDesiredSurfacesChanged: root.syncSlots(root.desiredSurfaces)
    // Visibility of the preview content, 0..1. It is the preview's own progress:
    // 1 while the preview is on screen, 0 while the tooltip replaces it.
    property real previewReveal: 0
    // Progress of the preview content as the whole popover shows it: the reveal
    // covers mode switches, the shell fade covers entering and leaving.
    readonly property real contentFade: root.previewReveal * root.shellFade

    // Height of the title row. It is the space the preview layer has to leave
    // free at the top of the frame.
    readonly property real titleRowHeight: titleFontMetrics.height

    // Natural size of one preview slot: the short side is fixed (120 tall on a
    // horizontal dock, 240 wide on a vertical one), the other side follows the
    // window's aspect ratio and is clamped so one slot never dominates the strip.
    function previewSlotSize(wrapper, horizontal) {
        if (horizontal) {
            if (!wrapper || !(wrapper.height > 0))
                return Qt.size(80, 120);
            return Qt.size(Math.max(80, Math.min(240, 120 * wrapper.width / wrapper.height)), 120);
        }
        if (!wrapper || !(wrapper.width > 0))
            return Qt.size(240, 80);
        return Qt.size(240, Math.max(80, Math.min(120, 240 * wrapper.height / wrapper.width)));
    }

    // Fitted size of a thumbnail inside a slot, aspect ratio preserved. Shared
    // by the slot itself and by the layers that outlive it.
    function thumbnailSize(wrapper, slotWidth, slotHeight) {
        if (!wrapper || !(wrapper.width > 0) || !(wrapper.height > 0))
            return Qt.size(0, 0);
        return Qt.size(Math.min(slotWidth, wrapper.width * slotHeight / wrapper.height) - 4,
                       Math.min(slotHeight, wrapper.height * slotWidth / wrapper.width) - 4);
    }

    function contentSizeFor(wrappers, horizontal) {
        const count = wrappers ? wrappers.length : 0;
        let width = 0;
        let height = 0;
        let placed = 0;

        for (let i = 0; i < count; ++i) {
            const wrapper = wrappers[i];
            if (!wrapper)
                continue;

            const slot = root.previewSlotSize(wrapper, horizontal);
            if (placed > 0) {
                if (horizontal)
                    width += root.spacing;
                else
                    height += root.spacing;
            }
            if (horizontal) {
                width += slot.width;
                height = Math.max(height, slot.height);
            } else {
                width = Math.max(width, slot.width);
                height += slot.height;
            }
            ++placed;
        }

        return Qt.size(width, height);
    }

    readonly property size contentSize:
        root.contentSizeFor(root.desiredSurfaces, root.isHorizontal)

    readonly property size liveTooltipSize: root.liveTooltipLayer
                                            ? root.liveTooltipLayer.naturalSize
                                            : Qt.size(0, 0)

    // Natural size of the tooltip on screen, falling back to the one that just
    // left: a switch never has to measure a layer that is already fading out.
    readonly property size tooltipSize:
        root.liveTooltipSize.width > 0 ? root.liveTooltipSize : root.lastTooltipSize

    // Frame size for a preview layout on this output. A strip longer than the
    // output is not squeezed.
    function previewFrameSizeFor(contentSize) {
        const titleSpace = root.titleRowHeight + root.spacing;
        const maxContentWidth = Math.max(0, root.outPutSize.width
                                            - 2 * root.screenEdgeMargin
                                            - 2 * root.panelHorizontalPadding);
        const maxContentHeight = Math.max(0, root.outPutSize.height
                                             - 2 * root.screenEdgeMargin
                                             - 2 * root.panelVerticalPadding
                                             - titleSpace);
        return Qt.size(Math.min(contentSize.width, maxContentWidth)
                           + 2 * root.panelHorizontalPadding,
                       Math.min(contentSize.height, maxContentHeight)
                           + titleSpace + 2 * root.panelVerticalPadding);
    }

    function frameSizeFor(kind) {
        return kind === DockPreview.Mode.Tooltip
                   ? root.tooltipSize
                   : root.previewFrameSizeFor(root.contentSize);
    }

    readonly property size targetFrameSize: root.frameSizeFor(root.mode)

    // Content area the preview settles at, from the content alone.
    readonly property size previewViewport: {
        const frame = root.previewFrameSizeFor(root.contentSize);
        return Qt.size(Math.max(0, frame.width - 2 * root.panelHorizontalPadding),
                       Math.max(0, frame.height - 2 * root.panelVerticalPadding
                                        - root.titleRowHeight - root.spacing));
    }

    // The frame is the only animated geometry.
    property real frameWidth: root.targetFrameSize.width
    property real frameHeight: root.targetFrameSize.height

    // Opacity of the surface the popover paints.
    // Tooltip layers are not included.
    property real shellFade: 0

    function setShowing(showing) {
        if (showing)
            root.visible = true;
        // The states used to carry these two numbers; the Behaviours act on the
        // assignment, so entry and exit read the same here as everywhere else.
        root.shellFade = showing ? 1 : 0;
        scaleTransform.xScale = showing ? 1 : 0.5;
        root.isShowing = showing;
    }

    // The shell fade also owns the popover's lifetime: it hides the item (and
    // drops the state it kept alive) once the fade out is over.
    Behavior on shellFade {
        SequentialAnimation {
            NumberAnimation {
                id: shellFadeAnimation
                duration: root.aniDuration
                easing.type: Easing.OutExpo
            }
            ScriptAction {
                script: {
                    if (!root.isShowing) {
                        root.visible = false;
                        root.setPreviewReveal(0, false);
                        root.tooltipSurface = null;
                        root.mode = DockPreview.Mode.Tooltip;
                        root.desiredSurfaces = [ ];
                        root.destroySlots();
                    }
                }
            }
        }
    }

    // Progress of the preview layout, 1 whenever the preview is the current mode,
    // only animates during a tooltip <-> preview switch.
    NumberAnimation {
        id: previewRevealAnimation
        target: root
        property: "previewReveal"
        duration: root.aniDuration
        easing.type: Easing.OutExpo
        onFinished: {
            if (root.tooltipLayout)
                root.desiredSurfaces = [ ];
        }
    }

    function setPreviewReveal(value, animate) {
        if (!animate || !root.enableAnimation) {
            previewRevealAnimation.stop();
            root.previewReveal = value;
            return;
        }
        if (previewRevealAnimation.running) {
            // The same target is already animating: leave it alone.
            if (previewRevealAnimation.to === value)
                return;
        } else if (root.previewReveal === value) {
            return;
        }
        // Restart from the current value, so a switch that reverses midway does
        // not jump back to where it started.
        previewRevealAnimation.stop();
        previewRevealAnimation.from = root.previewReveal;
        previewRevealAnimation.to = value;
        previewRevealAnimation.start();
    }

    Behavior on frameWidth {
        enabled: root.visible && root.enableAnimation
        NumberAnimation {
            duration: root.aniDuration
            easing.type: Easing.OutExpo
        }
    }

    Behavior on frameHeight {
        enabled: root.visible && root.enableAnimation
        NumberAnimation {
            duration: root.aniDuration
            easing.type: Easing.OutExpo
        }
    }

    // Space the preview layer reserves for the title, growing from zero.
    readonly property real reservedTitleSpace:
        root.previewReveal * (root.titleRowHeight + root.spacing)
    // Vertical rhythm of the frame, top to bottom:
    //   panel padding | title row | spacing | preview content | panel padding
    readonly property real contentLeft: root.panelHorizontalPadding
    readonly property real contentTop: root.panelVerticalPadding + root.reservedTitleSpace
    readonly property real contentWidth:
        Math.max(0, root.frameWidth - 2 * root.panelHorizontalPadding)
    readonly property real contentHeight:
        Math.max(0, root.frameHeight - 2 * root.panelVerticalPadding - root.reservedTitleSpace)

    visible: false

    LoggingCategory {
        id: qLcDockPreview
        name: "treeland.qml.dockpreview"
        defaultLogLevel: LoggingCategory.Info
    }

    component PopoverCloseButton: D.ToolButton {
        id: closeButton

        property int maximumSize: PopoverStyle.closeButtonMaxSize
        // Rounded down to an even number so the circle keeps a whole-pixel centre.
        readonly property int diameter: 2 * Math.floor(maximumSize / 2)

        implicitWidth: diameter
        implicitHeight: diameter
        padding: 0
        icon.name: "close"
        icon.width: Math.round(diameter * 0.6)
        icon.height: Math.round(diameter * 0.6)
        // DTK paints the background as a rounded BoxPanel; half the size is a circle.
        Binding {
            target: closeButton.background
            property: "radius"
            value: closeButton.diameter / 2
            when: closeButton.background
        }
    }

    // Content a slot has stopped showing: the frame and the thumbnail it was
    // drawing, fading from wherever it is and destroying itself. Every layer owns
    // its own fade, so a switch that interrupts another one keeps the content
    // that is still leaving instead of dropping it.
    Component {
        id: retiredContentComponent

        Item {
            id: retired
            property var wrapper: null

            readonly property size fittedSize:
                root.thumbnailSize(wrapper, width, height)

            Rectangle {
                anchors.fill: parent
                radius: slotView.radius
                color: PopoverStyle.itemBackground
            }

            ShaderEffectSource {
                anchors.centerIn: parent
                implicitHeight: retired.fittedSize.height
                implicitWidth: retired.fittedSize.width
                sourceItem: retired.wrapper
            }

            NumberAnimation {
                id: fadeAnimation
                target: retired
                property: "opacity"
                to: 0
                duration: root.aniDuration
                // The cross fade runs on a symmetric curve; Qt wants the two
                // control points and the end of the segment.
                easing.type: Easing.Bezier
                easing.bezierCurve: [0.42, 0, 0.58, 1, 1, 1]
                onFinished: retired.destroy()
            }

            // `from` continues an interrupted cross fade from where the content
            // it copies was drawn, instead of snapping it back to full.
            function fadeOut(from) {
                opacity = from === undefined ? 1 : from;
                fadeAnimation.start();
            }
        }
    }

    // A slot: the window it shows, the rect it is drawn from and the rect it is
    // heading to, and its own animations between them. A slot outlives the list
    // it came from - one that is dropped keeps moving out and destroys itself
    // once it is done, so a switch may reverse at any point.
    Component {
        id: slotComponent

        Item {
            id: slot

            property var wrapper: null
            property rect fromRect: Qt.rect(0, 0, 0, 0)
            property rect toRect: Qt.rect(0, 0, 0, 0)
            property real progress: 1
            // The frame and the content fade on their own values: an added slot
            // brings both in, a swap only replaces the content.
            property real borderOpacity: 1
            property real contentOpacity: 1
            property bool leaving: false
            // Position of the slot in the strip, kept by the layout.
            property int slotIndex: -1

            x: fromRect.x + (toRect.x - fromRect.x) * progress
            y: fromRect.y + (toRect.y - fromRect.y) * progress
            width: fromRect.width + (toRect.width - fromRect.width) * progress
            height: fromRect.height + (toRect.height - fromRect.height) * progress

            readonly property size fittedSize: root.thumbnailSize(wrapper, width, height)

            // Out from behind the edge the strip ended at, to its own place.
            function enter(newWrapper, edge, rect) {
                wrapper = newWrapper;
                if (!root.enableAnimation) {
                    fromRect = rect;
                    toRect = rect;
                    progress = 1;
                    return;
                }
                fromRect = root.isHorizontal
                        ? Qt.rect(edge - rect.width, rect.y, rect.width, rect.height)
                        : Qt.rect(rect.x, edge - rect.height, rect.width, rect.height);
                toRect = rect;
                progress = 0;
                borderOpacity = 0;
                contentOpacity = 0;
                moveAnimation.restart();
                borderReveal.restart();
                contentReveal.restart();
            }

            // The slot keeps its place: a new window replaces the content in it,
            // and it moves to the rect the layout gives it.
            function retarget(newWrapper, rect) {
                if (newWrapper !== wrapper) {
                    if (root.enableAnimation && wrapper)
                        retireContent(contentOpacity);
                    wrapper = newWrapper;
                    if (root.enableAnimation) {
                        contentOpacity = 0;
                        contentReveal.restart();
                    } else {
                        contentOpacity = 1;
                    }
                }
                if (!root.enableAnimation) {
                    fromRect = rect;
                    toRect = rect;
                    progress = 1;
                    return;
                }
                // Carry on from where the slot is drawn right now, so a switch
                // that interrupts another one does not jump.
                fromRect = Qt.rect(x, y, width, height);
                toRect = rect;
                progress = 0;
                moveAnimation.restart();
            }

            // The list no longer has this slot: it closes up towards the slot it
            // followed and is destroyed when the animation is over.
            function leave(collapseRect) {
                leaving = true;
                // Closing up while the slots that stay move over it.
                z = 0;
                if (!root.enableAnimation) {
                    root.dropSlot(slot);
                    return;
                }
                fromRect = Qt.rect(x, y, width, height);
                toRect = collapseRect;
                progress = 0;
                moveAnimation.restart();
                leaveFade.restart();
            }

            // The list wants this slot again before it finished leaving: it stops
            // fading out and heads for the rect the layout gives it.
            function revive(rect) {
                leaving = false;
                leaveFade.stop();
                opacity = 1;
                if (!root.enableAnimation) {
                    fromRect = rect;
                    toRect = rect;
                    progress = 1;
                    return;
                }
                fromRect = Qt.rect(x, y, width, height);
                toRect = rect;
                progress = 0;
                moveAnimation.restart();
                if (borderOpacity < 1)
                    borderReveal.restart();
                if (contentOpacity < 1)
                    contentReveal.restart();
            }

            // One layer per content this slot is still fading out: an animation
            // that interrupts another one adds its layer instead of taking the
            // previous one over.
            function retireContent(from) {
                if (!wrapper)
                    return;
                const layer = retiredContentComponent.createObject(retiringHost,
                                                                 {"wrapper": wrapper});
                if (!layer) {
                    console.warn(qLcDockPreview, "failed to create the leaving layer");
                    return;
                }
                layer.width = Qt.binding(() => retiringHost.width);
                layer.height = Qt.binding(() => retiringHost.height);
                layer.fadeOut(from);
            }

            NumberAnimation {
                id: moveAnimation
                target: slot
                property: "progress"
                to: 1
                duration: root.aniDuration
                easing.type: Easing.OutExpo
            }

            NumberAnimation {
                id: borderReveal
                target: slot
                property: "borderOpacity"
                to: 1
                duration: root.aniDuration
                easing.type: Easing.OutExpo
            }

            NumberAnimation {
                id: contentReveal
                target: slot
                property: "contentOpacity"
                to: 1
                duration: root.aniDuration
                easing.type: Easing.Bezier
                easing.bezierCurve: [0.42, 0, 0.58, 1, 1, 1]
            }

            NumberAnimation {
                id: leaveFade
                target: slot
                property: "opacity"
                to: 0
                duration: root.aniDuration
                easing.type: Easing.OutExpo
                onFinished: root.dropSlot(slot)
            }

            Connections {
                target: slot.wrapper
                function onAboutToBeInvalidated() {
                    root.removeSurface(slot.wrapper)
                }
            }

            HoverHandler {
                id: hoverHandler
                onHoveredChanged: {
                    if (hovered) {
                        root.previewWindow(slot.wrapper)
                        slotView.hoveredSlot = slot.slotIndex
                    }
                }
                cursorShape: Qt.ArrowCursor
            }

            TapHandler {
                onTapped: root.activateWindow(slot.wrapper)
            }

            // The frame the preview draws behind the thumbnail.
            Rectangle {
                anchors.fill: parent
                radius: slotView.radius
                color: PopoverStyle.itemBackground
                opacity: slot.borderOpacity
            }

            ShaderEffectSource {
                anchors.centerIn: parent
                implicitHeight: slot.fittedSize.height
                implicitWidth: slot.fittedSize.width
                opacity: slot.contentOpacity
                sourceItem: slot.wrapper
            }

            // Contents this slot is fading out, bounded by the slot itself.
            Item {
                id: retiringHost
                anchors.fill: parent
            }

            // one close button per preview, shown while hovering this item
            PopoverCloseButton {
                id: closeButton
                anchors {
                    top: parent.top
                    right: parent.right
                    topMargin: 10
                    rightMargin: 10
                }
                visible: hoverHandler.hovered && !slot.leaving
                onClicked: {
                    slotView.hoveredSlot = -1
                    // capture the wrapper now: the deferred callback must not
                    // read the slot again, it may be gone by then
                    const target = slot.wrapper
                    Qt.callLater(()=>root.closeSpecialWindow(target))
                }
            }
        }
    }

    /* --- model begin --- */
    // Slots the preview shows, in layout order, and the ones that are still
    // fading out after the list dropped them.
    property var liveSlots: []
    property var leavingSlots: []

    // Slot rects for a list of windows: pure, from the slot sizes alone.
    function slotRects(wrappers) {
        const rects = [ ];
        let offset = 0;
        for (let i = 0; i < wrappers.length; ++i) {
            const size = root.previewSlotSize(wrappers[i], root.isHorizontal);
            rects.push(root.isHorizontal
                       ? Qt.rect(offset, 0, size.width, size.height)
                       : Qt.rect(0, offset, size.width, size.height));
            offset += root.spacing + (root.isHorizontal ? size.width : size.height);
        }
        return rects;
    }

    function slotTrailingEdge(rect) {
        if (!rect)
            return 0;
        return root.isHorizontal ? rect.x + rect.width : rect.y + rect.height;
    }

    // Bring the slots in line with the windows the preview shows. Every slot keeps
    // its own animation: a switch only updates where it comes from and where it
    // goes, and a slot the list dropped keeps animating out on its own.
    function syncSlots(wrappers) {
        const rects = root.slotRects(wrappers);
        const live = root.liveSlots;
        const last = live.length ? live[live.length - 1] : null;
        // New slots come out from behind the last slot that is on screen.
        const entry = last ? root.slotTrailingEdge(Qt.rect(last.x, last.y, last.width,
                                                           last.height)) : 0;

        for (let i = 0; i < wrappers.length && i < live.length; ++i)
            live[i].retarget(wrappers[i], rects[i]);

        for (let i = live.length; i < wrappers.length; ++i) {
            // A slot that was leaving the same window comes back instead of a new
            // one being created next to it.
            const revived = root.takeLeaving(wrappers[i]);
            if (revived) {
                live.push(revived);
                revived.revive(rects[i]);
                continue;
            }
            const slot = slotComponent.createObject(slotHost);
            if (!slot) {
                console.warn(qLcDockPreview, "failed to create a preview slot");
                break;
            }
            live.push(slot);
            slot.enter(wrappers[i], entry, rects[i]);
        }

        if (live.length > wrappers.length) {
            const lastKept = wrappers.length ? root.slotTrailingEdge(rects[wrappers.length - 1])
                                             : 0;
            const dropped = live.splice(wrappers.length);
            for (const slot of dropped) {
                const rect = slot.toRect;
                root.leavingSlots.push(slot);
                slot.leave(root.isHorizontal
                           ? Qt.rect(lastKept - rect.width, rect.y, rect.width, rect.height)
                           : Qt.rect(rect.x, lastKept - rect.height, rect.width, rect.height));
            }
        }

        for (let i = 0; i < live.length; ++i) {
            live[i].slotIndex = i;
            live[i].z = slotView.slotZBase - i;
        }
        if (slotView.hoveredSlot >= live.length)
            slotView.hoveredSlot = -1;
    }

    // A slot that the same window is coming back to, if it is still leaving.
    function takeLeaving(wrapper) {
        const leaving = root.leavingSlots;
        for (let i = 0; i < leaving.length; ++i) {
            const slot = leaving[i];
            if (slot.wrapper === wrapper) {
                leaving.splice(i, 1);
                return slot;
            }
        }
        return null;
    }

    // A slot that is done takes itself out of both lists.
    function dropSlot(slot) {
        const live = root.liveSlots.indexOf(slot);
        if (live >= 0)
            root.liveSlots.splice(live, 1);
        const leaving = root.leavingSlots.indexOf(slot);
        if (leaving >= 0)
            root.leavingSlots.splice(leaving, 1);
        slot.destroy();
    }

    function destroySlots() {
        const slots = root.liveSlots.concat(root.leavingSlots);
        root.liveSlots = [ ];
        root.leavingSlots = [ ];
        for (const slot of slots)
            slot.destroy();
    }

    function removeSurface(surfaceWrapper) {
        if (!surfaceWrapper)
            return;

        const index = root.desiredSurfaces.indexOf(surfaceWrapper);
        if (index < 0)
            return;

        // Copy instead of splice: the assignment is what syncs the slots.
        const surfaces = root.desiredSurfaces.slice();
        surfaces.splice(index, 1);
        root.desiredSurfaces = surfaces;
    }

    function activateWindow(surfaceWrapper) {
        console.debug(qLcDockPreview, "activate preview window: ", surfaceWrapper)
        WaylandServer.forceActivateSurface(surfaceWrapper)
        ForeignToplevelManagerInterfaceV3.leaveDockPreview(root.target.shellSurface.surface)
        root.close();
    }

    function previewWindow(surfaceWrapper) {
        console.debug(qLcDockPreview, "start preview window: ", surfaceWrapper)
        WaylandServer.workspace.startPreviewing(surfaceWrapper);
    }

    function stopPreviewWindow() {
        console.debug(qLcDockPreview, "stop preview window")
        WaylandServer.workspace.stopPreviewing();
    }

    function closeAllWindow() {
        console.debug(qLcDockPreview, "close all preview windows")
        const surfaces = root.desiredSurfaces;
        root.desiredSurfaces = [ ];
        for (let surface of surfaces) {
            if (surface && surface.shellSurface)
                surface.shellSurface.close();
        }
        root.close();
    }

    function closeSpecialWindow(surfaceWrapper) {
        if (!surfaceWrapper)
            return;

        console.debug(qLcDockPreview, "close a special window: ", surfaceWrapper)
        root.removeSurface(surfaceWrapper)
        if (surfaceWrapper.shellSurface)
            surfaceWrapper.shellSurface.close()
    }
    function show(surfaces, target, pos, direction) {
        console.info(qLcDockPreview, "start show windows: ", surfaces)
        const wasVisible = root.visible;
        root.parent = target.parent;

        // Keep the strip anchored to the dock side as much as possible, the
        // switch animations rely on it. Only a `dock preview` <-> `tooltip`
        // switch may anchor it to the other side.
        if (root.isShowing && root.tooltipLayout)
            root.stripPinToLeft = root.pos?.x - pos?.x <= 0;
        else
            root.stripPinToLeft = true

        root.pos = pos;
        root.direction = direction;
        root.target = target;
        root.mode = DockPreview.Mode.Preview;
        // Drop empty entries once, at the only entry that takes an external list.
        const windows = [];
        for (let i = 0; i < surfaces.length; ++i) {
            if (surfaces[i])
                windows.push(surfaces[i]);
        }
        root.desiredSurfaces = windows;
        // Same anchor the list had: left/bottom by default, the opposite side for
        // a horizontal strip that switched towards the dock.
        slotView.contentX = root.isHorizontal && !root.stripPinToLeft
                ? Math.max(0, root.contentSize.width - root.previewViewport.width) : 0;
        slotView.contentY = root.isHorizontal
                ? 0 : Math.max(0, root.contentSize.height - root.previewViewport.height);
        // A popover that is already on screen switches with a curve; one that is
        // entering from nothing lands on the final layout right away.
        root.setPreviewReveal(1, wasVisible);
        root.setShowing(true);
        root.tooltipSurface = null;
    }

    // Every tooltip the dock stops showing becomes a frozen layer.
    function demoteTooltipLayer(layer) {
        if (!layer)
            return;

        // Snapshot the size before clearing the live layer: tooltipSize falls
        // back to lastTooltipSize whenever there is no live one, so this keeps
        // the frame target from dipping during a switch.
        const size = layer.naturalSize;
        if (size.width > 0 && size.height > 0)
            root.lastTooltipSize = size;
        if (root.liveTooltipLayer === layer)
            root.liveTooltipLayer = null;
        layer.freeze();
        layer.fadeOut();
    }

    function createLiveTooltip(surface) {
        const layer = tooltipLayerComponent.createObject(contentHost);
        if (!layer) {
            console.warn(qLcDockPreview, "failed to create the tooltip layer");
            return;
        }
        const item = tooltipSurfaceComponent.createObject(layer);
        if (!item) {
            console.warn(qLcDockPreview, "failed to create the tooltip surface item");
            layer.destroy();
            return;
        }
        // Set the surface on an already completed item, so it is initialized the
        // same way the previously static item was, and has its size right away.
        item.surface = surface;
        layer.surfaceItem = item;
        root.liveTooltipLayer = layer;
        layer.fadeIn();
    }

    function applyTooltipSurface(surface) {
        // Only the change handler calls this.
        const live = root.liveTooltipLayer;
        if (surface)
            root.createLiveTooltip(surface);
        root.demoteTooltipLayer(live);
    }

    function showTooltip(surface, target, pos, direction) {
        console.info(qLcDockPreview, "start show tooltip: ", surface, target, pos, direction)
        const wasVisible = root.visible;
        root.parent = target.parent;
        if (root.isShowing && !root.tooltipLayout)
            root.stripPinToLeft = root.pos?.x - pos?.x >= 0;
        else
            root.stripPinToLeft = true

        root.pos = pos;
        root.direction = direction;
        root.target = target;
        // Publish the surface before the mode: tooltipSize reads the live layer,
        // and switching mode first would let the frame target fall back to the
        // previous tooltip for a moment.
        root.tooltipSurface = surface;
        root.mode = DockPreview.Mode.Tooltip;
        // Same as show(): a switch animates the preview away, an entry from
        // nothing starts with the preview already collapsed.
        root.setPreviewReveal(0, wasVisible);
        root.setShowing(true);
        if (!root.enableAnimation)
            root.desiredSurfaces = [ ];
    }

    function close() {
        console.info(qLcDockPreview, "stop dock preview")
        root.setShowing(false);
        root.tooltipSurface = null;
    }
    /* --- model end --- */

    /* --- global position begin --- */
    implicitWidth: root.targetFrameSize.width
    implicitHeight: root.targetFrameSize.height

    // Where the frame settles in the parent's coordinates.
    readonly property real unadjustedFrameX: {
        if (!root.target)
            return 0;

        const frameWidth = root.targetFrameSize.width;
        switch (root.direction) {
        case ForeignToplevelManagerInterfaceV3.PreviewDirection.left:
            return root.target.x + root.pos.x;
        case ForeignToplevelManagerInterfaceV3.PreviewDirection.right:
            return root.target.x + root.pos.x - frameWidth;
        default: // bottom / top: the frame's center follows the dock icon
            return root.target.x + root.pos.x - frameWidth / 2;
        }
    }
    readonly property real unadjustedFrameY: {
        if (!root.target)
            return 0;

        const frameHeight = root.targetFrameSize.height;
        switch (root.direction) {
        case ForeignToplevelManagerInterfaceV3.PreviewDirection.bottom:
            return root.target.y + root.pos.y - frameHeight;
        case ForeignToplevelManagerInterfaceV3.PreviewDirection.top:
            return root.target.y + root.pos.y;
        default: // left / right: the frame's center follows the dock icon
            return root.target.y + root.pos.y - frameHeight / 2;
        }
    }

    // Keep the settled frame inside the output, with a margin on every side.
    readonly property real horizontalCenterOffsetAdjust: {
        const leftSpace = root.unadjustedFrameX - root.outPutPos.x;
        if (leftSpace < 0)
            return -leftSpace + root.screenEdgeMargin;

        const rightSpace = root.outPutPos.x + root.outPutSize.width
                           - (root.unadjustedFrameX + root.targetFrameSize.width);
        if (rightSpace < 0)
            return rightSpace - root.screenEdgeMargin;

        return 0;
    }
    readonly property real verticalCenterOffsetAdjust: {
        const topSpace = root.unadjustedFrameY - root.outPutPos.y;
        if (topSpace < 0)
            return -topSpace + root.screenEdgeMargin;

        const bottomSpace = root.outPutPos.y + root.outPutSize.height
                            - (root.unadjustedFrameY + root.targetFrameSize.height);
        if (bottomSpace < 0)
            return bottomSpace - root.screenEdgeMargin;

        return 0;
    }

    states: [
        State {
            name: "dock_bottom"
            when: direction === ForeignToplevelManagerInterfaceV3.PreviewDirection.bottom
            AnchorChanges {
                target: root
                anchors.horizontalCenter: root.target?.horizontalCenter
                anchors.bottom: root.target?.top
            }
            AnchorChanges {
                target: background
                anchors.horizontalCenter: root.horizontalCenter
                anchors.bottom: root.bottom
                // Pin the background's dock-facing edge to root's.
            }
            PropertyChanges {
                target: root
                anchors.horizontalCenterOffset: root.pos.x - root.target?.width / 2 + horizontalCenterOffsetAdjust
                anchors.bottomMargin: -root.pos.y
                restoreEntryValues: false
            }
            PropertyChanges {
                target: scaleTransform
                origin.x: root.frameWidth / 2
                origin.y: root.frameHeight
            }
        },
        State {
            name: "dock_left"
            when: direction === ForeignToplevelManagerInterfaceV3.PreviewDirection.left
            AnchorChanges {
                target: root
                anchors.verticalCenter: root.target?.verticalCenter
                anchors.left: root.target?.left
            }
            AnchorChanges {
                target: background
                anchors.verticalCenter: root.verticalCenter
                anchors.left: root.left
            }
            PropertyChanges {
                target: root
                anchors.verticalCenterOffset: root.pos.y - root.target?.height / 2 + verticalCenterOffsetAdjust
                anchors.leftMargin: root.pos.x
                restoreEntryValues: false
            }
            PropertyChanges {
                target: scaleTransform
                origin.x: 0
                origin.y: root.frameHeight / 2
            }
        },
        State {
            name: "dock_top"
            when: direction === ForeignToplevelManagerInterfaceV3.PreviewDirection.top
            AnchorChanges {
                target: root
                anchors.horizontalCenter: root.target?.horizontalCenter
                anchors.top: root.target?.top
            }
            AnchorChanges {
                target: background
                anchors.horizontalCenter: root.horizontalCenter
                anchors.top: root.top
            }
            PropertyChanges {
                target: root
                anchors.horizontalCenterOffset: root.pos.x - root.target?.width / 2 + horizontalCenterOffsetAdjust
                anchors.topMargin: root.pos.y
                restoreEntryValues: false
            }
            PropertyChanges {
                target: scaleTransform
                origin.x: root.frameWidth / 2
                origin.y: 0
            }
        },
        State {
            name: "dock_right"
            when: direction === ForeignToplevelManagerInterfaceV3.PreviewDirection.right
            AnchorChanges {
                target: root
                anchors.verticalCenter: root.target?.verticalCenter
                anchors.right: root.target?.left
            }
            AnchorChanges {
                target: background
                anchors.verticalCenter: root.verticalCenter
                anchors.right: root.right
            }
            PropertyChanges {
                target: root
                anchors.verticalCenterOffset: root.pos.y - root.target?.height / 2 + verticalCenterOffsetAdjust
                anchors.rightMargin: -root.pos.x
                restoreEntryValues: false
            }
            PropertyChanges {
                target: scaleTransform
                origin.x: root.frameWidth
                origin.y: root.frameHeight / 2
            }
        }
    ]

    Behavior on anchors.horizontalCenterOffset {
        enabled: root.visible && root.enableAnimation
        NumberAnimation {
            duration: root.aniDuration
            easing.type: Easing.OutExpo
        }
    }

    Behavior on anchors.verticalCenterOffset {
        enabled: root.visible && root.enableAnimation
        NumberAnimation {
            duration: root.aniDuration
            easing.type: Easing.OutExpo
        }
    }
    /* --- global position end --- */

    // Line height of the title font, measured without a real string.
    FontMetrics {
        id: titleFontMetrics
        font: PopoverStyle.titleFont
    }
    // The preview strip: a clipped, flickable viewport holding one item per slot.
    // Each slot owns the animation between the rect it had and the rect the
    // layout gives it now, so nothing else has to position them.
    Flickable {
        id: slotView
        // Outside the shell group: the preview content owns its own opacity, so
        // it does not multiply into the shell fade.
        parent: contentHost

        property int radius: 5
        // Topmost z of the preview items.
        readonly property int slotZBase: 10000
        // Row of the slot the pointer is on, -1 when it is on none of them.
        property int hoveredSlot: -1

        clip: true
        enabled: !root.tooltipLayout
        // The view is held at the size the preview settles at, anchored to the
        // dock side. Entering the popover and swapping it between tooltip and
        // preview scale the whole view into the frame it is growing into.
        width: root.previewViewport.width
        height: root.previewViewport.height
        x: root.stripPinToLeft ? root.contentLeft
                               : root.contentLeft + root.contentWidth - width
        y: root.contentTop + root.contentHeight - height
        transformOrigin: root.stripPinToLeft ? Item.BottomLeft : Item.BottomRight
        contentWidth: root.contentSize.width
        contentHeight: root.contentSize.height
        boundsBehavior: Flickable.StopAtBounds
        interactive: true
        scale: {
            if (!(width > 0) || !(height > 0))
                return 0;
            // Tooltip <-> preview and the entry scale the view into the frame it
            // grows into. Between two previews the view keeps scale 1: the slots
            // move and resize themselves, so their size never comes from a scale.
            if (root.contentFade < 1)
                return Math.max(0, Math.min(1, root.contentWidth / width,
                                             root.contentHeight / height));
            return 1;
        }
        // The reveal covers mode switches; the shell fade covers entering and
        // leaving the popover.
        opacity: root.contentFade

        // Hosts the slots. They are created, moved and destroyed by the layout,
        // so a slot the list dropped can finish its own animation first.
        Item {
            id: slotHost
            width: slotView.contentWidth
            height: slotView.contentHeight
        }

        // Hover border, over every slot. Purely visual: it is kept out of hover
        // competition so that the slot underneath still gets its own hover.
        Control {
            objectName: "highlight"
            id: hoverBorder
            readonly property Item slot:
                slotView.hoveredSlot >= 0 && slotView.hoveredSlot < root.liveSlots.length
                    ? root.liveSlots[slotView.hoveredSlot] : null
            visible: slot !== null
            hoverEnabled: false
            z: slotView.slotZBase + 1
            x: slot ? slot.x : 0
            y: slot ? slot.y : 0
            width: slot ? slot.width : 0
            height: slot ? slot.height : 0

            contentItem: Rectangle {
                anchors.fill: parent
                color: "transparent";
                radius: slotView.radius + border.width
                property D.Palette borderPalette: D.Palette {
                    normal {
                        common: Qt.rgba(0, 0, 0, 0.2)
                    }
                    normalDark {
                        common: Qt.rgba(1, 1, 1, 0.3)
                    }
                }

                border.color: D.ColorSelector.borderPalette
                border.width: 4
            }
        }

        HoverHandler {
            onHoveredChanged: {
                if (!hovered) {
                    slotView.hoveredSlot = -1
                    root.stopPreviewWindow()
                }
            }
            cursorShape: Qt.ArrowCursor
        }
    }

    Rectangle {
        id: background
        z: root.z
        // Only the frame geometry.
        color: "transparent"
        width: root.frameWidth
        height: root.frameHeight

        radius: PopoverStyle.radius
        clip: false
        parent: root.parent
        visible: root.visible

        // Host of every content layer. Each layer owns its opacity, so nothing
        // here multiplies into the shell's fade. The shell paints below them.
        Item {
            id: contentHost
            anchors.fill: parent
            z: 0
        }

        // The group fades on its own value.
        Rectangle {
            id: shell
            anchors.fill: parent
            z: -1
            opacity: root.shellFade
            clip: false
            color: PopoverStyle.tintFor(root.enableBlur && backgroundBlur.blurEnabled)
            radius: background.radius

            XdgShadow {
                visible: root.enableShadow
                cornerRadius: background.radius
                shadowBlur: PopoverStyle.shadowBlur
                shadowOffsetX: 0
                shadowOffsetY: PopoverStyle.shadowOffsetY
                shadowColor: PopoverStyle.shadowColor
            }

            Blur {
                id: backgroundBlur
                anchors.fill: parent
                visible: root.enableBlur
                radius: background.radius
            }

            Border {
                anchors.fill: parent
                visible: root.enableBorder
                radius: background.radius
                outsideColor: PopoverStyle.borderOutsideColor
                insideColor: PopoverStyle.borderInsideColor
            }
        }

        HoverHandler {
            id: titleHover
            enabled: root.desiredSurfaces.length !== 0
            onHoveredChanged: {
                if (!hovered) {
                    ForeignToplevelManagerInterfaceV3.leaveDockPreview(root.target.shellSurface.surface)
                } else {
                    ForeignToplevelManagerInterfaceV3.enterDockPreview(root.target.shellSurface.surface)
                }
            }
            cursorShape: Qt.ArrowCursor
        }

        RowLayout {
            id: headLayout
            spacing: 4
            parent: shell
            height: root.titleRowHeight
            anchors {
                left: shell.left
                right: shell.right
                top: shell.top
                leftMargin: root.panelHorizontalPadding
                rightMargin: root.panelHorizontalPadding
                topMargin: root.panelVerticalPadding
            }

            Item {
                id: titleTextArea
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitWidth: titleText.implicitWidth

                Text {
                    id: titleText
                    anchors.left: parent.left
                    anchors.top: parent.top
                    width: parent.width
                    text: {
                        const surfaces = root.desiredSurfaces;
                        if (surfaces.length === 0)
                            return "";
                        const index = Math.min(Math.max(slotView.hoveredSlot, 0),
                                               surfaces.length - 1);
                        const wrapper = surfaces[index];
                        return wrapper && wrapper.shellSurface
                               ? wrapper.shellSurface.title : "";
                    }
                    font: PopoverStyle.titleFont
                    color: PopoverStyle.titleColor
                    horizontalAlignment: Text.AlignLeft
                    elide: Text.ElideRight
                    opacity: root.previewReveal
                }
            }

            PopoverCloseButton {
                // Never taller than the title row it appears in.
                maximumSize: Math.min(PopoverStyle.closeButtonMaxSize, root.titleRowHeight)
                // Only the preview state has a close-all action: in the tooltip
                // state the button would take width from the text.
                visible: !root.tooltipLayout && titleHover.hovered
                Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                onClicked: {
                    root.closeAllWindow()
                }
            }
        }

        Component {
            id: tooltipSurfaceComponent
            SurfaceItem {
                flags: SurfaceItem.RejectEvent
                // Its natural size, never stretched; the owning layer is what gets
                // positioned and compressed.
                width: implicitWidth
                height: implicitHeight
            }
        }

        // One layer per shown tooltip surface.
        Component {
            id: tooltipLayerComponent

            Item {
                id: layer
                z: 1
                // The frame this layer shows.
                property Item surfaceItem: null
                property bool fadingOut: false

                readonly property size naturalSize: surfaceItem
                                                     ? Qt.size(surfaceItem.implicitWidth,
                                                               surfaceItem.implicitHeight)
                                                     : Qt.size(0, 0)

                // Where the layer sits inside the frame, for the current
                // direction. Position and the scale origin both read the same
                // animated frame on root, so they cannot disagree.
                readonly property real layerX: {
                    switch (root.direction) {
                    case ForeignToplevelManagerInterfaceV3.PreviewDirection.left:
                        return 0;
                    case ForeignToplevelManagerInterfaceV3.PreviewDirection.right:
                        return root.frameWidth - width;
                    default: // top / bottom: centred on the dock icon
                        return (root.frameWidth - width) / 2;
                    }
                }

                readonly property real layerY: {
                    switch (root.direction) {
                    case ForeignToplevelManagerInterfaceV3.PreviewDirection.top:
                        return 0;
                    case ForeignToplevelManagerInterfaceV3.PreviewDirection.bottom:
                        return root.frameHeight - height;
                    default: // left / right: centred on the dock icon
                        return (root.frameHeight - height) / 2;
                    }
                }

                // Every layer shrinks towards the edge the tooltip is glued to.
                readonly property point scaleOrigin: {
                    switch (root.direction) {
                    case ForeignToplevelManagerInterfaceV3.PreviewDirection.left:
                        return Qt.point(0, height / 2);
                    case ForeignToplevelManagerInterfaceV3.PreviewDirection.right:
                        return Qt.point(width, height / 2);
                    case ForeignToplevelManagerInterfaceV3.PreviewDirection.top:
                        return Qt.point(width / 2, 0);
                    default: // bottom
                        return Qt.point(width / 2, height);
                    }
                }
                // Fit into the frame without ever stretching it.
                readonly property real compressionX:
                    width > 0 && root.frameWidth > 0 ? Math.min(1, root.frameWidth / width) : 1
                readonly property real compressionY:
                    height > 0 && root.frameHeight > 0 ? Math.min(1, root.frameHeight / height) : 1

                visible: surfaceItem !== null
                width: naturalSize.width
                height: naturalSize.height

                x: layer.layerX
                y: layer.layerY

                transform: Scale {
                    origin.x: layer.scaleOrigin.x
                    origin.y: layer.scaleOrigin.y
                    xScale: layer.compressionX
                    yScale: layer.compressionY
                }

                NumberAnimation {
                    id: fadeAnimation
                    target: layer
                    property: "opacity"
                    duration: root.aniDuration
                    easing.type: Easing.OutExpo
                    onFinished: {
                        if (layer.fadingOut)
                            layer.destroy();
                    }
                }

                function freeze() {
                    if (!surfaceItem)
                        return;
                    surfaceItem.surface = null;
                    surfaceItem.enabled = false;
                }

                function fadeIn() {
                    fadingOut = false;
                    if (!root.enableAnimation) {
                        opacity = 1;
                        return;
                    }
                    opacity = 0;
                    fadeAnimation.from = 0;
                    fadeAnimation.to = 1;
                    fadeAnimation.restart();
                }

                // A demoted layer only owns its fade from here on.
                function fadeOut() {
                    fadingOut = true;
                    if (!root.enableAnimation) {
                        layer.destroy();
                        return;
                    }
                    fadeAnimation.from = opacity;
                    fadeAnimation.to = 0;
                    fadeAnimation.restart();
                }
            }
        }

        transform: Scale {
            id: scaleTransform
            xScale: 0.5
            yScale: xScale

            Behavior on xScale {
                NumberAnimation {
                    id: scaleAnimation
                    duration: root.aniDuration
                    easing.type: Easing.OutExpo
                }
            }
        }
    }
}
