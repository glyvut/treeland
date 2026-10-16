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
    property int aniDuration: 200
    // whether animations are enabled; default to true when aniDuration > 0
    property bool enableAnimation: aniDuration > 0
    property alias spacing: listview.spacing
    property bool enableBlur: true
    // Whether the window list is laid out horizontally. Determined by the dock
    // direction, except in list mode where windows are always stacked vertically.
    readonly property bool isHorizontal: (root.direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.bottom
                                          || root.direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.top)
                                         && !root.listMode
    // List mode: too many windows to fit as thumbnails, show a vertical list of
    // window titles instead (no live preview).
    property bool listMode: false
    readonly property int listRowHeight: 36
    readonly property int listWidth: Math.min(320, Math.max(160, root.screenWidth - 60))
    // Duration of the preview <-> list content morph. Single knob so the pacing
    // is easy to tune; the reference dock animation is a slow ease-out morph
    // (about one second), not a quick collapse.
    property int contentMorphDuration: 1000
    // --- preview <-> list morph state ---
    // The transition is one continuous morph: the panel takes the new content
    // from the start (so its target size is known immediately and the border
    // animates towards it), while the previous content is kept in a second
    // layer that shrinks/fades out. A single progress value drives the fade.
    property bool morphing: false
    property real morphProgress: 0
    property bool outgoingListMode: false
    property real morphFromWidth: 0
    property real morphFromHeight: 0
    readonly property real incomingOpacity: root.morphing ? root.morphProgress : 1
    readonly property real outgoingOpacity: root.morphing ? (1 - root.morphProgress) : 0
    readonly property real outgoingHeightScale: root.morphFromHeight > 0 ? background.implicitHeight / root.morphFromHeight : 1
    // An incoming list grows in sync with the panel height: the content is laid
    // out at the target size, so scale it by the fraction of the target height
    // the panel currently shows. (Only meaningful while morphing into list
    // mode; the panel and this value reach 1 together at the end.)
    readonly property real incomingHeightScale: {
        if (!root.morphing || !root.listMode)
            return 1;
        const targetHeight = root.implicitHeight + 2 * listview.spacing - headLayout.implicitHeight;
        if (targetHeight <= 0)
            return 1;
        return Math.min(1, (background.implicitHeight - headLayout.implicitHeight) / targetHeight);
    }
    readonly property bool outgoingHorizontal: (root.direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.bottom
                                                || root.direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.top)
                                               && !root.outgoingListMode
    // set by show()
    property var target
    property point pos
    property int direction
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
    property int horizontalCenterOffsetAdjust: 0
    property int verticalCenterOffsetAdjust: 0
    // set by showTooltip()
    property string tooltip: ""
    // handle by workspace

    property bool isShowing: false
    property bool isTooltip: false
    property bool isNewDockPreview: false
    property bool listviewPinToLeft: true

    visible: false

    LoggingCategory {
        id: qLcDockPreview
        name: "treeland.qml.dockpreview"
        defaultLogLevel: LoggingCategory.Info
    }

    /* --- model begin --- */
    ListModel {
        id: filterSurfaceModel
        property int lastSize: 0
        onCountChanged: {
            Qt.callLater(()=> { lastSize = count })
        }

        property var desiredSurfaces: []
        onDesiredSurfacesChanged:  {
            updateModel();
        }

        function updateModel() {
            var newSize = desiredSurfaces.length;
            var oldSize = filterSurfaceModel.count;
            root.isNewDockPreview = oldSize === 0 && newSize !== 0;
            for (let i = 0; i < Math.min(newSize, oldSize); i++) {
                filterSurfaceModel.set(i, {"wrapper": desiredSurfaces[i]})
            }
            if (newSize > oldSize) {
                for (let index = oldSize; index < newSize; index++) {
                    filterSurfaceModel.append({"wrapper": desiredSurfaces[index]});
                }
            }
            if (newSize < oldSize) {
                filterSurfaceModel.remove(newSize, oldSize - newSize)
            }
        }

        function removeSurface(surfaceWrapper) {
            if (!surfaceWrapper)
                return;

            var index = desiredSurfaces.indexOf(surfaceWrapper);
            if (index < 0)
                return;

            desiredSurfaces.splice(index, 1);
            updateModel();
        }

        function activateWindow(surfaceWrapper) {
            console.debug(qLcDockPreview, "activate preview window: ", surfaceWrapper)
            WaylandServer.forceActivateSurface(surfaceWrapper)
            ForeignToplevelManagerInterfaceV2.leaveDockPreview(root.target.shellSurface.surface)
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
            var tmp = filterSurfaceModel.desiredSurfaces;
            filterSurfaceModel.desiredSurfaces = [ ];
            for (let surface of tmp) {
                surface.shellSurface.close();
            }
            root.close();
        }

        function closeSpecialWindow(surfaceWrapper) {
            if (!surfaceWrapper)
                return;

            console.debug(qLcDockPreview, "close a special window: ", surfaceWrapper)
            filterSurfaceModel.removeSurface(surfaceWrapper)
            if (surfaceWrapper.shellSurface)
                surfaceWrapper.shellSurface.close()
        }
    }

    // Snapshot of the content that is leaving during a preview <-> list morph.
    ListModel {
        id: outgoingModel
    }

    // Begin a preview <-> list morph. The previous content is snapshotted into
    // the outgoing layer, the new content is applied immediately (so the panel
    // starts resizing from the very first frame), and one progress animation
    // drives the whole cross-fade so nothing happens in disconnected steps.
    function startContentMorph(surfaces, useList) {
        const oldSurfaces = filterSurfaceModel.desiredSurfaces;
        outgoingModel.clear();
        for (let i = 0; i < oldSurfaces.length; ++i)
            outgoingModel.append({ "wrapper": oldSurfaces[i] });

        root.outgoingListMode = root.listMode;
        root.morphFromWidth = background.implicitWidth;
        root.morphFromHeight = background.implicitHeight;
        root.morphProgress = 0;
        root.morphing = true;

        // Apply the incoming content right away so the panel has a target size
        // to morph towards while the old content is still fading out.
        root.listMode = useList;
        filterSurfaceModel.desiredSurfaces = surfaces;

        contentMorphAnimation.restart();
    }

    function show(surfaces, target, pos, direction) {
        console.info(qLcDockPreview, "start show windows: ", surfaces)
        root.parent = target.parent;

        // We want keep listview ancher to left as possible, many animations rely on it
        // Only when show `dock preview` <-> 'tooltip' animation ancher to right
        if (root.isShowing && root.isTooltip)
            root.listviewPinToLeft = root.pos?.x - pos?.x <= 0;
        else
            root.listviewPinToLeft = true

        const wasShowing = root.isShowing;
        const wasTooltip = root.isTooltip;
        const wasList = root.listMode;

        // Update the target state first: shouldUseList() reads the output rect
        // (outPutPos/outPutSize), which is derived from root.target.
        root.pos = pos;
        root.direction = direction;
        root.target = target;

        const useList = shouldUseList(surfaces, target, pos, direction);

        const switchContentMode = wasShowing && !wasTooltip && wasList !== useList;

        if (switchContentMode && root.enableAnimation) {
            startContentMorph(surfaces, useList);
        } else {
            contentMorphAnimation.stop();
            root.morphing = false;
            outgoingModel.clear();
            root.listMode = useList;
            filterSurfaceModel.desiredSurfaces = surfaces;
        }

        if (wasShowing && wasTooltip) {
            if (root.enableAnimation) {
                tooltip2PreViewAnimation.start();
            } else {
                root.tooltip = ""
            }
        }
        root.isTooltip = false;
        root.isShowing = true;
    }

    // Decide whether the thumbnails would not fit on screen and a vertical
    // title list has to be used instead.
    function shouldUseList(surfaces, target, pos, direction) {
        if (!surfaces || surfaces.length === 0)
            return false;

        const horizontalDock = direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.bottom
                               || direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.top;
        const screenExtent = horizontalDock ? root.screenWidth : root.screenHeight;

        // The panel stays centred on the dock icon, so its usable span is
        // symmetric: it can only grow to twice the icon's distance to the
        // nearest screen edge before it runs off screen (and the edge clamp
        // would then push the whole panel away from the icon). Base the
        // overflow decision on that span so thumbnail mode always stays centred
        // on the icon instead of being shifted aside.
        const center = horizontalDock ? (target?.x ?? 0) + (pos?.x ?? 0)
                                      : (target?.y ?? 0) + (pos?.y ?? 0);
        const nearEdge = horizontalDock ? outPutPos.x : outPutPos.y;
        const farEdge = horizontalDock ? outPutPos.x + outPutSize.width
                                       : outPutPos.y + outPutSize.height;
        const span = 2 * Math.min(center - nearEdge, farEdge - center) - 40;
        // Fall back to the raw screen size if the icon geometry is unusable.
        const available = (center > nearEdge && center < farEdge && span > 0)
                          ? span : screenExtent - 40;

        let extent = listview.spacing * (surfaces.length + 1);
        for (let i = 0; i < surfaces.length; ++i) {
            const width = surfaces[i].width;
            const height = surfaces[i].height;
            if (!width || !height) {
                extent += 120;
                continue;
            }
            if (horizontalDock)
                extent += Math.max(80, Math.min(240, 120 * width / height));
            else
                extent += Math.max(80, Math.min(120, 240 * height / width));
        }
        const result = extent > available;
        console.info(qLcDockPreview, "dock preview extent", extent, "available", available,
                     "center", center, "direction", direction, "windows", surfaces.length,
                     "list mode", result);
        return result;
    }

    function showTooltip(tooltip, target, pos, direction) {
        console.info(qLcDockPreview, "start show tooltip: ", tooltip, target, pos, direction)
        root.parent = target.parent;
        if (root.isShowing && !root.isTooltip)
            root.listviewPinToLeft = root.pos?.x - pos?.x >= 0;
        else
            root.listviewPinToLeft = true
        root.tooltip = tooltip;
        root.pos = pos;
        root.direction = direction;
        root.target = target;
        if (root.isShowing && !root.isTooltip) {
            if (root.enableAnimation) {
                preView2TooltipAnimation.start();
            }else {
                filterSurfaceModel.desiredSurfaces = [ ];
            }
        }
        root.isTooltip = true;
        root.isShowing = true;
    }

    function close() {
        console.info(qLcDockPreview, "stop dock preview")
        root.isShowing = false;
    }
    /* --- model end --- */

    /* --- global position begin --- */
    implicitWidth: getWidth(false)
    implicitHeight: getHeight(false)

    onWidthChanged: {
        var leftSpace = root.x - outPutPos.x;
        if (leftSpace < 0) {
            horizontalCenterOffsetAdjust = -leftSpace + 10;
            return;
        }
        var rightSpace = outPutPos.x + outPutSize.width - (root.x + root.width);
        if (rightSpace < 0) {
            horizontalCenterOffsetAdjust = rightSpace - 10;
            return;
        }
        horizontalCenterOffsetAdjust = 0
    }
    onHeightChanged: {
        var topSpace = root.y - outPutPos.y;
        if (topSpace < 0) {
            verticalCenterOffsetAdjust = -topSpace;
            return;
        }
        var bottomSpace = outPutPos.y + outPutSize.height - (root.y + root.height);
        if (bottomSpace < 0) {
            verticalCenterOffsetAdjust = bottomSpace;
            return;
        }
        verticalCenterOffsetAdjust = 0
    }

    states: [
        State {
            name: "dock_bottom"
            when: direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.bottom
            AnchorChanges {
                target: root
                anchors.horizontalCenter: root.target?.horizontalCenter
                anchors.bottom: root.target?.top
            }
            AnchorChanges {
                target: background
                anchors.horizontalCenter: root.horizontalCenter
                anchors.bottom: root.bottom
                // horizontalCenterOffset always follow root.horizontalCenterOffset
                // bottomMargin always set to zero
            }
            PropertyChanges {
                target: root
                anchors.horizontalCenterOffset: root.pos.x - root.target?.width / 2 + horizontalCenterOffsetAdjust
                anchors.bottomMargin: -root.pos.y
                restoreEntryValues: false
            }
            PropertyChanges {
                target: scaleTransform
                origin.x: background.implicitWidth / 2
                origin.y: background.implicitHeight
            }
        },
        State {
            name: "dock_left"
            when: direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.left
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
                origin.y: background.implicitHeight / 2
            }
        },
        State {
            name: "dock_top"
            when: direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.top
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
                origin.x: background.implicitWidth / 2
                origin.y: 0
            }
        },
        State {
            name: "dock_right"
            when: direction === ForeignToplevelManagerInterfaceV2.PreviewDirection.right
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
                origin.x: background.implicitWidth
                origin.y: background.implicitHeight / 2
            }
        }
    ]

    Behavior on anchors.horizontalCenterOffset {
        enabled: root.visible && root.enableAnimation
        NumberAnimation {
            duration: root.enableAnimation ? root.aniDuration : 0
        }
    }

    Behavior on anchors.verticalCenterOffset {
        enabled: root.visible && root.enableAnimation
        NumberAnimation {
            duration: root.enableAnimation ? root.aniDuration : 0
        }
    }
    /* --- global position end --- */

    TextMetrics {
        id: tooltipMetrics
        font.pointSize: 12
        text: root.tooltip
    }

    function getWidth(removing) {
        let tooltipWidth = Math.min(tooltipMetrics.width + listview.spacing * 2, root.outPutSize.width);
        if (removing && root.isTooltip)
            return tooltipWidth;

        let width = 0
        var reseverWidth = listview.orientation === ListView.Horizontal ? -listview.spacing : 0

        for (let child of listview.contentItem.visibleChildren) {
            if (child.objectName === "highlight" || (removing && child.isRemoving)) continue
            if (listview.orientation === ListView.Horizontal) {
                let tmp = width + (child.implicitWidth + listview.spacing)
                if (tmp > outPutSize.width) {
                    return width + reseverWidth
                } else {
                    width = tmp
                }
            } else {
                width = Math.max(width, child.implicitWidth)
            }
        }

        width += reseverWidth + 2 * listview.spacing

        if (width < tooltipWidth)
            width = tooltipWidth;
        return width
    }

    function getHeight(removing) {
        if (removing && root.isTooltip)
            return tooltipMetrics.height;

        let height = 0
        let reseverHeight = headLayout.implicitHeight + (listview.orientation === ListView.Vertical ? 0 : listview.spacing)

        for (let child of listview.contentItem.visibleChildren) {
            if (child.objectName === "highlight" || (removing && child.isRemoving)) continue
            if (listview.orientation === ListView.Vertical) {
                let tmp = height + (child.implicitHeight + listview.spacing)
                if (tmp > outPutSize.height) {
                    return height + reseverHeight
                } else {
                    height = tmp
                }
            } else {
                height = Math.max(height, child.implicitHeight)
            }
        }
        height += reseverHeight + listview.spacing
        return height;
    }

    onListviewPinToLeftChanged: {
        // We must clear all left/right anchors then reset them
        listview.anchors.left = undefined
        listview.anchors.right = undefined
        listview.anchors.left = root.listviewPinToLeft ? listview.parent.left : undefined
        listview.anchors.right = root.listviewPinToLeft ? undefined : listview.parent.right
    }

    ListView {
        id: listview
        model: filterSurfaceModel
        parent: background

        property size lastSize: Qt.size(0, 0)
        property int radius: 5
        // Topmost z of the preview items (delegates).
        readonly property int delegateZBase: 10000
        clip: false
        opacity: root.incomingOpacity
        anchors.left: parent.left // Maybe changed in `onListviewPinToLeftChanged`
        anchors.leftMargin: listview.spacing
        anchors.rightMargin: listview.spacing
        anchors.bottom: parent.bottom
        transformOrigin: root.listviewPinToLeft ? Item.BottomLeft : Item.BottomRight
        // An incoming list grows together with the panel height instead of just
        // fading in at full size. (Identity while not list-morphing.)
        transform: Scale {
            origin.x: 0
            origin.y: listview.height
            xScale: 1
            yScale: root.incomingHeightScale
        }

        orientation: root.isHorizontal ? ListView.Horizontal : ListView.Vertical
        layoutDirection: Qt.LeftToRight
        verticalLayoutDirection: ListView.TopToBottom
        boundsBehavior: Flickable.StopAtBounds
        interactive: true
        highlightFollowsCurrentItem: true
        implicitHeight: root.implicitHeight - headLayout.implicitHeight
        implicitWidth: root.implicitWidth - listview.spacing * 2
        highlightMoveDuration: root.enableAnimation ? root.aniDuration/2 : 0
        spacing: 5
        highlight: Control {
            objectName: "highlight"
            id: hoverBorder
            visible: false
            // purely visual overlay: keep it out of hover competition so that
            // the preview item underneath still gets its own hover
            hoverEnabled: false
            // keep the hover border above every preview
            z: listview.delegateZBase + 1
            anchors {
                verticalCenter: listview.orientation === ListView.Horizontal && parent ? parent.verticalCenter : undefined
                horizontalCenter: listview.orientation === ListView.Vertical && parent ? parent.horizontalCenter : undefined
            }

            contentItem: Rectangle {
                anchors.fill: parent
                color: "transparent";
                radius: listview.radius + border.width
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
        focus: true
        delegate: Rectangle {
            id: delegate
            objectName: "delegate"
            // ensure removing items are below non-removing ones
            z: (isRemoving ? -index : (listview.delegateZBase - index))
            visible: true
            radius: listview.radius
            color: Qt.rgba(0, 0, 0, 0.05) // TODO: dark mode

            required property var index
            required property var wrapper // from filterSurfaceModel
            property bool isRemoving: false

            // Incoming thumbnails start stacked together and spread out as the
            // morph progresses (they are only affected by the panel width).
            transform: Translate {
                x: (root.morphing && !root.listMode && root.isHorizontal) ? -(1 - root.morphProgress) * index * implicitWidth : 0
                y: (root.morphing && !root.listMode && !root.isHorizontal) ? -(1 - root.morphProgress) * index * implicitHeight : 0
            }

            implicitHeight: root.listMode ? root.listRowHeight
                                           : (root.isHorizontal ? 120 : Math.max(80, Math.min(120, 240 * wrapper.height / wrapper.width)))
            implicitWidth: root.listMode ? root.listWidth
                                          : (root.isHorizontal ? Math.max(80, Math.min(240, 120 * wrapper.width / wrapper.height)) : 240)

            Connections {
                target: wrapper
                function onAboutToBeInvalidated() {
                    listview.model.removeSurface(wrapper)
                }
            }

            ListView.onRemove: {
                if (filterSurfaceModel.count && !root.morphing) {
                    isRemoving = true
                    removeAnimation.start()
                }
            }

            SequentialAnimation {
                id: removeAnimation
                PropertyAction { target: delegate; property: "ListView.delayRemove"; value: true }
                NumberAnimation {
                    target: delegate
                    property: root.isHorizontal ? "x" : "y"
                    to: {
                        var item = listview.itemAtIndex(listview.model.count - 1)
                        if (!item)
                            return root.isHorizontal ? delegate.x : delegate.y
                        return root.isHorizontal
                            ? item.x + item.implicitWidth - delegate.implicitWidth
                            : item.y + item.implicitHeight - delegate.implicitHeight
                    }
                    duration: root.enableAnimation ? root.aniDuration : 0
                    easing.type: Easing.OutExpo
                }
                PropertyAction { target: delegate; property: "ListView.delayRemove"; value: false }
            }

            HoverHandler {
                id: hoverHandler
                onHoveredChanged: {
                    if (hovered) {
                        listview.model.previewWindow(wrapper)
                        listview.currentIndex = index
                        if (listview.highlightItem)
                            listview.highlightItem.visible = true
                    }
                }
                cursorShape: Qt.ArrowCursor
            }

            TapHandler {
                onTapped: {
                    listview.model.activateWindow(wrapper)
                }
            }

            Loader {
                id: contentLoader
                anchors.fill: parent
                sourceComponent: root.listMode ? listRowComponent : thumbnailComponent
            }

            Component {
                id: thumbnailComponent
                ShaderEffectSource {
                    anchors.centerIn: parent
                    implicitHeight: Math.min(parent.height, wrapper.height * parent.width / wrapper.width) - 4
                    implicitWidth: Math.min(parent.width, wrapper.width * parent.height / wrapper.height) - 4
                    live: true
                    hideSource: false
                    smooth: true
                    sourceItem: wrapper
                }
            }

            Component {
                id: listRowComponent
                Item {
                    anchors.fill: parent
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: listview.spacing
                        anchors.rightMargin: listview.spacing
                        text: wrapper.shellSurface.title
                        font.pointSize: 14
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignLeft
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            // one close button per preview, shown while hovering this item
            D.ToolButton {
                id: closeButton
                anchors {
                    top: parent.top
                    right: parent.right
                    topMargin: 10
                    rightMargin: 10
                }
                implicitWidth: 24
                implicitHeight: 24
                icon.name: "close"
                icon.width: 16
                icon.height: 16
                visible: hoverHandler.hovered && !isRemoving
                onClicked: {
                    listview.highlightItem.visible = false
                    // capture the wrapper now: the deferred callback must not
                    // read the delegate's row again, the rows may have been
                    // removed or refreshed by then
                    const target = wrapper
                    Qt.callLater(()=>listview.model.closeSpecialWindow(target))
                }
            }
        }

        add: Transition {
            id: addTransition
            enabled: !root.isNewDockPreview && !root.morphing

            ParallelAnimation {
                NumberAnimation {
                    property: "opacity"
                    from: 0.3
                    to: 1
                    duration: root.enableAnimation ? root.aniDuration : 0
                }

                NumberAnimation {
                    property: root.isHorizontal ? "x" : "y"
                    from: {
                        var item = listview.itemAtIndex(listview.model.lastSize - 1)
                        if (!item)
                            return 0
                        return root.isHorizontal
                            ? item.x + item.implicitWidth - addTransition.ViewTransition.item?.implicitWidth
                            : item.y + item.implicitHeight - addTransition.ViewTransition.item?.implicitHeight
                    }
                    duration: root.enableAnimation ? root.aniDuration : 0
                    easing.type: Easing.OutExpo
                }
            }
        }

        HoverHandler {
            onHoveredChanged: {
                if (!hovered) {
                    if (listview.highlightItem)
                        listview.highlightItem.visible = false
                    listview.model.stopPreviewWindow()
                }
            }
            cursorShape: Qt.ArrowCursor
        }
    }

    // Previous content, shown only while morphing. It keeps the outgoing mode
    // (a list shrinks with the panel height, a thumbnail strip converges into a
    // stack) and fades out while the main listview fades the new content in.
    ListView {
        id: outgoingView
        parent: background
        model: outgoingModel
        visible: root.morphing
        enabled: false
        opacity: root.outgoingOpacity
        z: listview.z + 1
        clip: false
        interactive: false
        focus: false

        anchors.left: parent.left
        anchors.leftMargin: listview.spacing
        anchors.rightMargin: listview.spacing
        anchors.bottom: parent.bottom

        orientation: root.outgoingHorizontal ? ListView.Horizontal : ListView.Vertical
        layoutDirection: Qt.LeftToRight
        verticalLayoutDirection: ListView.TopToBottom
        boundsBehavior: Flickable.StopAtBounds
        spacing: listview.spacing
        transformOrigin: Item.BottomLeft
        implicitHeight: root.morphing ? (root.morphFromHeight - headLayout.implicitHeight) : 0
        implicitWidth: root.morphing ? (root.morphFromWidth - listview.spacing * 2) : 0

        // A leaving list shrinks with the panel height; a leaving thumbnail strip
        // keeps its size here and converges item-by-item in the delegate below.
        transform: Scale {
            origin.x: 0
            origin.y: outgoingView.height
            xScale: 1
            yScale: root.outgoingListMode ? root.outgoingHeightScale : 1
        }

        delegate: Rectangle {
            id: outgoingDelegate
            radius: listview.radius
            color: Qt.rgba(0, 0, 0, 0.05)

            required property var index
            required property var wrapper

            // Leaving thumbnails converge back into a stack as the morph
            // progresses, mirroring how the incoming thumbnails spread out
            // (instead of the whole strip being squeezed horizontally).
            transform: Translate {
                x: (root.outgoingHorizontal && !root.outgoingListMode) ? -root.morphProgress * index * implicitWidth : 0
                y: (!root.outgoingHorizontal && !root.outgoingListMode) ? -root.morphProgress * index * implicitHeight : 0
            }

            implicitHeight: root.outgoingListMode ? root.listRowHeight
                                                  : (root.outgoingHorizontal ? 120 : Math.max(80, Math.min(120, 240 * wrapper.height / wrapper.width)))
            implicitWidth: root.outgoingListMode ? root.listWidth
                                                 : (root.outgoingHorizontal ? Math.max(80, Math.min(240, 120 * wrapper.width / wrapper.height)) : 240)

            ShaderEffectSource {
                anchors.centerIn: parent
                visible: !root.outgoingListMode
                implicitHeight: Math.min(parent.height, wrapper.height * parent.width / wrapper.width) - 4
                implicitWidth: Math.min(parent.width, wrapper.width * parent.height / wrapper.height) - 4
                live: true
                hideSource: false
                smooth: true
                sourceItem: wrapper
            }

            Text {
                anchors.fill: parent
                visible: root.outgoingListMode
                anchors.leftMargin: listview.spacing
                anchors.rightMargin: listview.spacing
                text: wrapper.shellSurface.title
                font.pointSize: 14
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    SequentialAnimation {
        id: preView2TooltipAnimation
        ParallelAnimation {
            NumberAnimation {
                target: listview
                property: "scale"
                from: 1
                to: 0
                duration: root.enableAnimation ? root.aniDuration : 0
            }
        }
        ScriptAction {
            script: {
                filterSurfaceModel.desiredSurfaces = [ ];
                listview.scale = 1 // restore scale property after animation
            }
        }
    }

    SequentialAnimation {
        id: tooltip2PreViewAnimation
        ParallelAnimation {
            NumberAnimation {
                target: listview
                property: "scale"
                from: 0
                to: 1
                duration: root.enableAnimation ? root.aniDuration : 0
            }
        }
        PropertyAction { target: root; property: "tooltip"; value: "" }
    }

    // Single driver for the preview <-> list morph. The panel size (through the
    // background Behavior), the outgoing layer fade/scale and the incoming layer
    // fade/spread are all timed off this, so the transition reads as one
    // continuous motion instead of disconnected steps.
    NumberAnimation {
        id: contentMorphAnimation
        target: root
        property: "morphProgress"
        from: 0
        to: 1
        duration: root.enableAnimation ? root.contentMorphDuration : 0
        easing.type: Easing.OutCubic
        onFinished: {
            root.morphing = false;
            outgoingModel.clear();
        }
    }

    Rectangle {
        id: background
        z: root.z
        color: "#33ffffff"
        implicitWidth: {
            let newWidth = getWidth(true);
            // TODO: remove if Qt fix QTBUG-132588(maybe in Qt 6.10 or 6.8)
            if (!Number.isFinite(newWidth))
                return implicitWidth;
            return newWidth;
        }
        implicitHeight: {
            let newHiehgt = getHeight(true) + 2 * listview.spacing;
            // TODO: remove if Qt fix QTBUG-132588(maybe in Qt 6.10 or 6.8)
            if (!Number.isFinite(newHiehgt))
                return implicitHeight;
            return newHiehgt;
        }

        radius: listview.radius
        clip: false
        parent: root.parent
        visible: root.visible

        Blur {
            anchors.fill: parent
            visible: root.enableBlur
            radius: listview.radius
            blurEnabled: root.enableBlur
        }

        HoverHandler {
            id: titleHover
            enabled: listview.count !== 0
            onHoveredChanged: {
                if (!hovered) {
                    ForeignToplevelManagerInterfaceV2.leaveDockPreview(root.target.shellSurface.surface)
                } else {
                    ForeignToplevelManagerInterfaceV2.enterDockPreview(root.target.shellSurface.surface)
                }
            }
            cursorShape: Qt.ArrowCursor
        }

        RowLayout {
            id: headLayout
            spacing: 4

            parent: background
            anchors {
                left: background.left
                right: background.right
                top: background.top
                leftMargin: listview.spacing
                rightMargin: listview.spacing
                topMargin: listview.spacing
            }

            Rectangle {
                id: titleOrTooltipRect
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitWidth: root.isTooltip ?  tooltipText.implicitWidth : titleIconText.implicitWidth
                implicitHeight: root.isTooltip ?  tooltipText.implicitHeight : titleIconText.implicitHeight
                color: "transparent"

                states: [
                    State {
                        name: "tooltip_visible"
                        when: root.isShowing && root.isTooltip
                        PropertyChanges {
                            restoreEntryValues: false // don't restore visible
                            tooltipText {
                                scale: 1
                                opacity: 1
                            }
                            titleIconText {
                                scale: 0.2
                                opacity: 0
                            }
                        }
                    },
                    State {
                        name: "title_visible"
                        when: root.isShowing && !root.isTooltip
                        PropertyChanges {
                            restoreEntryValues: false
                            tooltipText {
                                scale: 0.2
                                opacity: 0
                            }
                            titleIconText {
                                scale: 1
                                opacity: 1
                            }
                        }
                    },
                    State {
                        name: "all_hide"
                        when: !root.isShowing
                        PropertyChanges {
                            restoreEntryValues: false
                            tooltipText {
                                scale: 1
                                opacity: 0
                            }
                            titleIconText {
                                scale: 1
                                opacity: 0
                            }
                        }
                    }
                ]

                Text {
                    id: tooltipText
                    anchors.fill: parent
                    text: root.tooltip
                    font.pointSize: 12  // FIXME: D.DTK.fontManager.t6 can't work under waylib qpa
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    transformOrigin: Item.BottomLeft
                    Behavior on opacity {
                        enabled: root.isShowing && root.enableAnimation
                        SequentialAnimation {
                            NumberAnimation { duration: root.enableAnimation ? root.aniDuration : 0 }
                        }
                    }
                    Behavior on scale {
                        enabled: root.isShowing && root.enableAnimation
                        NumberAnimation { duration: root.enableAnimation ? root.aniDuration : 0 }
                    }
                }

                RowLayout {
                    id: titleIconText
                    anchors.fill: parent
                    spacing: 2
                    transformOrigin: Item.BottomLeft
                    /*
                    Rectangle { // TODO: We can't get app icon now, use Rectangle as fallback!
                        width: 24
                        height: 24
                        color: "yellow"
                    }
                    */
                    Text {
                        text: filterSurfaceModel.count ?
                                  filterSurfaceModel.get(Math.max(listview.currentIndex, 0)).wrapper.shellSurface.title : ""
                        font.pointSize: 14  // FIXME: D.DTK.fontManager can't work under waylib qpa
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignLeft
                        verticalAlignment: Text.AlignVCenter
                        Layout.fillWidth: true
                    }

                    Behavior on opacity {
                        enabled: root.isShowing && root.enableAnimation
                        SequentialAnimation {
                            NumberAnimation { duration: root.enableAnimation ? root.aniDuration : 0 }
                        }
                    }
                    Behavior on scale {
                        enabled: root.isShowing && root.enableAnimation
                        NumberAnimation { duration: root.enableAnimation ? root.aniDuration : 0 }
                    }
                }
            }

            D.ToolButton {
                implicitWidth: 24
                implicitHeight: 24
                visible: titleHover.hovered
                Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                Layout.rightMargin: 6
                icon.name: "close"
                icon.width: 16
                icon.height: 16
                onClicked: {
                    listview.model.closeAllWindow()
                }
            }
        }

        Behavior on implicitHeight {
            enabled: background.visible && root.enableAnimation
            NumberAnimation {
                duration: root.enableAnimation ? (root.morphing ? root.contentMorphDuration : root.aniDuration) : 0
                easing.type: root.morphing ? Easing.OutCubic : Easing.OutExpo
            }
        }

        Behavior on implicitWidth {
            enabled: background.visible && root.enableAnimation
            NumberAnimation {
                duration: root.enableAnimation ? (root.morphing ? root.contentMorphDuration : root.aniDuration) : 0
                easing.type: root.morphing ? Easing.OutCubic : Easing.OutExpo
            }
        }

        Behavior on anchors.horizontalCenterOffset {
            enabled: root.visible && root.enableAnimation
            NumberAnimation {
                duration: root.enableAnimation ? root.aniDuration : 0
                easing.type: Easing.OutExpo
            }
        }

        Behavior on anchors.verticalCenterOffset {
            enabled: root.visible && root.enableAnimation
            NumberAnimation {
                duration: root.enableAnimation ? root.aniDuration : 0
                easing.type: Easing.OutExpo
            }
        }

        transform: Scale {
            id: scaleTransform
            xScale: 0.5
            yScale: xScale

            Behavior on xScale {
                NumberAnimation {
                    duration: root.enableAnimation ? root.aniDuration : 0
                    easing.type: root.isShowing ? Easing.OutExpo : Easing.InExpo
                }
            }
        }

        Behavior on opacity {
            SequentialAnimation {
                ScriptAction {
                    script: {
                        if (root.isShowing)
                          root.visible = true;
                    }
                }
                NumberAnimation {
                    duration: root.enableAnimation ? root.aniDuration : 0
                    easing.type: root.isShowing ? Easing.OutExpo : Easing.InExpo
                }
                ScriptAction {
                    script: {
                        if (!root.isShowing) {
                            root.visible = false;
                            root.tooltip = ""
                            filterSurfaceModel.desiredSurfaces = [ ];
                        }
                    }
                }
            }
        }

        states: [
            State {
                name: "invisible"
                when: !root.isShowing
                PropertyChanges {
                    scaleTransform {
                        xScale: 0.5
                    }

                    background {
                        opacity: 0.0
                    }
                }
            },
            State {
                name: "visible"
                when: root.isShowing
                PropertyChanges {
                    scaleTransform {
                        xScale: 1.0
                    }

                    background {
                        opacity: 1.0
                    }
                }
            }
        ]
    }
}
