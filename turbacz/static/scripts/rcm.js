// ============================================
// RELAY CONTROL MANAGER (RCM)
// Main application for managing switches and relays
// ============================================

// ========== GLOBAL STATE ==========
const State = {
    // Core instances
    jsPlumb: null,
    wsManager: null,

    // Device collections
    switches: {},
    relays: {},
    connections: {},
    lights: {},

    // Status tracking
    devicesRssi: {},
    onlineRelays: new Set(),
    onlineSwitches: new Set(),
    upToDateDevices: {},
    hiddenDevices: new Set(),
    root_id: null,

    // UI state
    pendingClicks: new Set(),
    currentEditTarget: null,
    highlightedDevice: null,
    isLoadingConnections: false,
    isCardDragging: false,

    // Caching
    cachedElements: {},
    connectionLookupMap: {},

    // Button types: 0 = momentary, 1 = toggle
    buttonTypes: {},

    // Hidden buttons: Set of 'switchId-buttonId'
    hiddenButtons: new Set()
}

// Browser detection for performance optimizations
const isFirefox = navigator.userAgent.toLowerCase().includes('firefox')
if (isFirefox) {
    document.documentElement.classList.add('firefox')
}

// ========== CANVAS VIEW & ZOOM SYSTEM ==========
const CanvasView = {
    // Constants
    DEFAULT_ZOOM: 0.4,
    DEVICE_CENTER_X: 25000,
    DEVICE_CENTER_Y: 25000,
    MIN_ZOOM: 0.2,
    MAX_ZOOM: 3,

    // State
    zoomLevel: 0.4,
    panX: 0,
    panY: 0,
    isPanning: false,
    isPinching: false,
    startX: 0,
    startY: 0,
    lastPinchDistance: 0,
    lastTransformTime: 0,
    rafId: null,
    lastDisplayedZoom: -1,

    // DOM elements
    canvasElement: null,
    zoomLevelElement: null,

    init() {
        const saved = localStorage.getItem('rcm_canvas_view')
        if (saved) {
            const view = JSON.parse(saved)
            this.zoomLevel = view.zoomLevel || this.DEFAULT_ZOOM
            this.panX = view.panX !== undefined ? view.panX : this.getDefaultPanX()
            this.panY = view.panY !== undefined ? view.panY : this.getDefaultPanY()
        } else {
            this.zoomLevel = this.DEFAULT_ZOOM
            this.panX = this.getDefaultPanX()
            this.panY = this.getDefaultPanY()
        }
    },

    getDefaultPanX() {
        const viewportCenterX = window.innerWidth / 2
        return viewportCenterX - (this.DEVICE_CENTER_X * this.DEFAULT_ZOOM)
    },

    getDefaultPanY() {
        const viewportCenterY = (window.innerHeight - 90) / 2
        return viewportCenterY - (this.DEVICE_CENTER_Y * this.DEFAULT_ZOOM)
    },

    save() {
        localStorage.setItem('rcm_canvas_view', JSON.stringify({
            zoomLevel: this.zoomLevel,
            panX: this.panX,
            panY: this.panY
        }))
    }
}

// Initialize canvas view
CanvasView.init()

// Legacy variables for compatibility
let jsPlumbInstance = State.jsPlumb
let switches = State.switches
let relays = State.relays
let connections = State.connections
let lights = State.lights
let devices_rssi = State.devicesRssi
let online_relays = State.onlineRelays
let online_switches = State.onlineSwitches
let up_to_date_devices = State.upToDateDevices
let hiddenDevices = State.hiddenDevices
let pendingClicks = State.pendingClicks
let currentEditTarget = State.currentEditTarget
let highlightedDevice = State.highlightedDevice
let isLoadingConnections = State.isLoadingConnections
let cachedElements = State.cachedElements
let connectionLookupMap = State.connectionLookupMap
let buttonTypes = State.buttonTypes
let hiddenButtons = State.hiddenButtons
let zoomLevel = CanvasView.zoomLevel
let panX = CanvasView.panX
let panY = CanvasView.panY
let isPanning = CanvasView.isPanning
let isPinching = CanvasView.isPinching
let startX = CanvasView.startX
let startY = CanvasView.startY
let lastPinchDistance = CanvasView.lastPinchDistance
let canvasElement = CanvasView.canvasElement
let zoomLevelElement = CanvasView.zoomLevelElement
let lastDisplayedZoom = CanvasView.lastDisplayedZoom
let rafId = CanvasView.rafId
let isCardDragging = State.isCardDragging
let root_id = State.root_id

const API_BASE_URL = `https://${window.location.host}`
const FIREFOX_THROTTLE_MS = 16

// ========== UTILITY FUNCTIONS ==========
function throttle(func, delay) {
    let lastCall = 0
    return function (...args) {
        const now = Date.now()
        if (now - lastCall >= delay) {
            lastCall = now
            return func.apply(this, args)
        }
    }
}

function debounce(func, delay) {
    let timeoutId
    return function (...args) {
        clearTimeout(timeoutId)
        timeoutId = setTimeout(() => func.apply(this, args), delay)
    }
}

// Initialize WebSocket manager
const wsManager = new WebSocketManager('/rcm/ws/', function (event) {
    const msg = JSON.parse(event.data)
    console.log(msg)

    // Handle different message types
    if (msg.type === "update") {
        loadConfiguration()
        return
    }

    if (msg.type === "light_state") {
        pendingClicks.delete(`${msg.relay_id}-${msg.output_id}`)
        lights[`${msg.relay_id}-${msg.output_id}`] = msg.state
        updateLightUI(msg.relay_id, msg.output_id, msg.state)
        return
    }

    if (msg.type === "online_status") {
        online_relays = new Set(msg.online_relays)
        online_switches = new Set(msg.online_switches)
        up_to_date_devices = msg.up_to_date_devices || {}
        root_id = msg.root_id || null
        devices_rssi = msg.devices_rssi || {}
        State.root_id = root_id
        State.onlineRelays = online_relays
        State.onlineSwitches = online_switches
        State.upToDateDevices = up_to_date_devices
        State.devicesRssi = devices_rssi
        console.log('Online relays:', online_relays)
        console.log('Online switches:', online_switches)
        console.log('Up to date devices:', up_to_date_devices)
        console.log('Root device ID:', root_id)
        console.log('Devices RSSI:', devices_rssi)
        clearRootHighlight()
        highlightRoot()
        updateOnlineStatus()
        return
    }

    if (msg.type === "switch_state" && msg.switch_id && msg.button_id) {
        highlightButton(msg.switch_id, msg.button_id)
        setTimeout(() => clearButtonHighlight(msg.switch_id, msg.button_id), 5000)
        return
    }
})

// Connect and start connection monitoring
wsManager.connect()
wsManager.startConnectionCheck()

function showLoading() {
    document.getElementById('loading').classList.add('active')
}

function hideLoading() {
    document.getElementById('loading').classList.remove('active')
}

// =================== OPTIMIZED PAN/ZOOM SYSTEM ===================

// ------------------- GPU TRANSFORM -------------------
let lastTransformTime = 0

function applyCanvasTransform() {
    if (rafId) return // Already scheduled

    // Throttle more aggressively on Firefox at high zoom
    if (isFirefox && zoomLevel > 0.8) {
        const now = Date.now()
        if (now - lastTransformTime < FIREFOX_THROTTLE_MS) {
            return
        }
        lastTransformTime = now
    }

    rafId = requestAnimationFrame(() => {
        canvasElement.style.transform = `translate3d(${panX}px, ${panY}px, 0) scale(${zoomLevel})`
        rafId = null
    })
}

function applyCanvasTransformImmediate() {
    if (rafId) {
        cancelAnimationFrame(rafId)
        rafId = null
    }
    canvasElement.style.transform = `translate3d(${panX}px, ${panY}px, 0) scale(${zoomLevel})`
}

function updateZoomDisplay() {
    const displayZoom = Math.round(zoomLevel * 100)
    if (displayZoom !== lastDisplayedZoom) {
        zoomLevelElement.innerText = `${displayZoom}%`
        lastDisplayedZoom = displayZoom
    }
}

function applyVisualTransform() {
    applyCanvasTransformImmediate()
    updateZoomDisplay()
}

function suspendJsPlumb() {
    if (jsPlumbInstance) {
        jsPlumbInstance.setSuspendDrawing(true)
    }
    // Disable pointer events during transform for better Firefox performance
    if (isFirefox && canvasElement) {
        canvasElement.style.pointerEvents = 'none'
    }
}

function resumeJsPlumb() {
    if (rafId) {
        cancelAnimationFrame(rafId)
        rafId = null
    }
    applyCanvasTransformImmediate()

    // Re-enable pointer events
    if (isFirefox && canvasElement) {
        canvasElement.style.pointerEvents = ''
    }

    if (jsPlumbInstance) {
        // Ensure jsPlumb uses the current zoom before resuming drawing
        jsPlumbInstance.setZoom(zoomLevel)
        // On Firefox, delay repaint slightly to avoid blocking the UI
        if (isFirefox) {
            jsPlumbInstance.setSuspendDrawing(false, false)
            setTimeout(() => jsPlumbInstance.repaintEverything(), 50)
        } else {
            jsPlumbInstance.setSuspendDrawing(false, true)
        }
    }
}

// ------------------- JSPLUMB SYNC -------------------
function syncJsPlumb() {
    if (!jsPlumbInstance) return
    jsPlumbInstance.setZoom(zoomLevel)
    jsPlumbInstance.repaintEverything()
}

// ------------------- DEBOUNCED SYNC -------------------
let syncTimeout = null
function debouncedSyncJsPlumb(delay = 500) {
    clearTimeout(syncTimeout)
    syncTimeout = setTimeout(syncJsPlumb, delay)
}

// ------------------- ZOOM AT POINT -------------------
function zoomAtPoint(factor, centerX, centerY, commit = false) {
    const prevScale = zoomLevel
    zoomLevel *= factor
    zoomLevel = Math.min(Math.max(zoomLevel, 0.2), 3)

    panX = centerX - (centerX - panX) * (zoomLevel / prevScale)
    panY = centerY - (centerY - panY) * (zoomLevel / prevScale)

    applyCanvasTransform()
    updateZoomDisplay()
    if (commit) {
        syncJsPlumb()
    } else if (!isPinching) {
        // Avoid scheduling jsPlumb syncs while pinching; resume handles it
        debouncedSyncJsPlumb()
    }
}

// ------------------- RESET / BUTTON ZOOM -------------------
function resetZoom() {
    zoomLevel = 0.4
    panX = getDefaultPanX()
    panY = getDefaultPanY()
    applyVisualTransform()
    syncJsPlumb()
    saveCanvasView()
}
function zoomIn() { zoomAtPoint(1.1, window.innerWidth / 2, window.innerHeight / 2, true) }
function zoomOut() { zoomAtPoint(0.9, window.innerWidth / 2, window.innerHeight / 2, true) }

// ------------------- INIT PANNING / ZOOM -------------------
function initPanning() {
    const wrapper = document.getElementById('canvas-wrapper')
    canvasElement = document.getElementById('canvas')
    zoomLevelElement = document.getElementById('zoomLevel')

    // Add GPU acceleration hints
    canvasElement.style.backfaceVisibility = 'hidden'
    canvasElement.style.transformOrigin = '0 0'
    canvasElement.style.transform = 'translateZ(0)'

    // ----------------- MOUSE PANNING -----------------
    wrapper.addEventListener('mousedown', e => {
        if (e.target !== wrapper && e.target !== canvasElement) return
        isPanning = true
        startX = e.clientX - panX
        startY = e.clientY - panY
        wrapper.classList.add('grabbing')
        suspendJsPlumb()
    })
    const mouseMoveHandler = isFirefox ? throttle((e) => {
        if (!isPanning) return
        panX = e.clientX - startX
        panY = e.clientY - startY
        applyCanvasTransform()
    }, 16) : (e) => {
        if (!isPanning) return
        panX = e.clientX - startX
        panY = e.clientY - startY
        applyCanvasTransform()
    }
    document.addEventListener('mousemove', mouseMoveHandler)
    document.addEventListener('mouseup', () => {
        if (!isPanning) return
        isPanning = false
        wrapper.classList.remove('grabbing')
        resumeJsPlumb()
        saveCanvasView()
    })

    // ----------------- CANVAS CLICK (CLEAR HIGHLIGHTS) -----------------
    wrapper.addEventListener('click', e => {
        // Only clear highlights if clicking directly on wrapper or canvas (not on devices)
        if (e.target === wrapper || e.target === canvasElement) {
            clearHighlights()
        }
    })

    // ----------------- TOUCH PANNING -----------------
    wrapper.addEventListener('touchstart', e => {
        if (e.touches.length === 1 && (e.target === wrapper || e.target === canvasElement)) {
            isPanning = true
            const touch = e.touches[0]
            startX = touch.clientX - panX
            startY = touch.clientY - panY
            wrapper.classList.add('grabbing')
            suspendJsPlumb()
            e.preventDefault()
        }
        if (e.touches.length === 2) { // pinch start
            isPinching = true
            isPanning = false
            wrapper.classList.remove('grabbing')
            const t1 = e.touches[0], t2 = e.touches[1]
            lastPinchDistance = Math.hypot(t2.clientX - t1.clientX, t2.clientY - t1.clientY)
            // Suspend jsPlumb during pinch zoom for smoothness
            suspendJsPlumb()
            e.preventDefault()
        }
    }, { passive: false })

    const touchMoveHandler = (e) => {
        if (isPanning && e.touches.length === 1) {
            const touch = e.touches[0]
            panX = touch.clientX - startX
            panY = touch.clientY - startY
            if (!isFirefox || Date.now() - lastTransformTime >= FIREFOX_THROTTLE_MS) {
                applyCanvasTransform()
            }
            e.preventDefault()
        }
        if (isPinching && e.touches.length === 2) {
            const t1 = e.touches[0], t2 = e.touches[1]
            const distance = Math.hypot(t2.clientX - t1.clientX, t2.clientY - t1.clientY)
            if (lastPinchDistance > 0) {
                const center = {
                    x: (t1.clientX + t2.clientX) / 2,
                    y: (t1.clientY + t2.clientY) / 2
                }
                const factor = distance / lastPinchDistance
                zoomAtPoint(factor, center.x, center.y, false)
            }
            lastPinchDistance = distance
            e.preventDefault()
        }
    }
    document.addEventListener('touchmove', touchMoveHandler, { passive: false })

    document.addEventListener('touchend', e => {
        if (isPanning) {
            isPanning = false
            wrapper.classList.remove('grabbing')
            resumeJsPlumb()
            saveCanvasView()
        }
        if (isPinching) {
            isPinching = false
            lastPinchDistance = 0
            // Resume jsPlumb and apply current zoom
            resumeJsPlumb()
            setTimeout(saveCanvasView, 150)
        }
    })

    // ----------------- TOUCH CLICK (CLEAR HIGHLIGHTS) -----------------
    let touchStartTarget = null
    wrapper.addEventListener('touchstart', e => {
        if (e.touches.length === 1) {
            touchStartTarget = e.target
        }
    }, { passive: true })

    wrapper.addEventListener('touchend', e => {
        // Only clear highlights if tap on wrapper or canvas (not on devices or during pan/pinch)
        if (e.changedTouches.length === 1 &&
            touchStartTarget === e.target &&
            (e.target === wrapper || e.target === canvasElement) &&
            !isPanning && !isPinching) {
            clearHighlights()
        }
        touchStartTarget = null
    }, { passive: true })

    // ----------------- MOUSE WHEEL ZOOM -----------------
    let wheelTimeout = null
    let lastWheelTime = 0
    const wheelHandler = e => {
        // Throttle wheel events on Firefox
        if (isFirefox) {
            const now = Date.now()
            if (now - lastWheelTime < 30) {
                e.preventDefault()
                return
            }
            lastWheelTime = now
        }

        if (!wheelTimeout) suspendJsPlumb()
        clearTimeout(wheelTimeout)

        const rect = wrapper.getBoundingClientRect()
        const centerX = e.clientX - rect.left
        const centerY = e.clientY - rect.top
        const factor = e.deltaY < 0 ? 1.1 : 0.9

        const prevScale = zoomLevel
        zoomLevel *= factor
        zoomLevel = Math.min(Math.max(zoomLevel, 0.2), 3)
        panX = centerX - (centerX - panX) * (zoomLevel / prevScale)
        panY = centerY - (centerY - panY) * (zoomLevel / prevScale)

        applyCanvasTransform()
        updateZoomDisplay()

        wheelTimeout = setTimeout(() => {
            resumeJsPlumb()
            saveCanvasView()
            wheelTimeout = null
        }, isFirefox ? 300 : 200)

        e.preventDefault()
    }
    wrapper.addEventListener('wheel', wheelHandler, { passive: false })

    // ----------------- INITIAL VIEW -----------------
    applyVisualTransform()
    syncJsPlumb()
}

// Save/Load canvas view
function saveCanvasView() {
    localStorage.setItem('rcm_canvas_view', JSON.stringify({
        zoomLevel: zoomLevel,
        panX: panX,
        panY: panY
    }))
    console.log('Canvas view saved')
}

// Save/Load device positions
function saveDevicePositions() {
    const positions = {}

    for (let switchId in switches) {
        const element = document.getElementById(`switch-${switchId}`)
        if (element) {
            positions[`switch-${switchId}`] = {
                x: parseInt(element.style.left),
                y: parseInt(element.style.top)
            }
        }
    }

    for (let relayId in relays) {
        const element = document.getElementById(`relay-${relayId}`)
        if (element) {
            positions[`relay-${relayId}`] = {
                x: parseInt(element.style.left),
                y: parseInt(element.style.top)
            }
        }
    }

    localStorage.setItem('rcm_device_positions', JSON.stringify(positions))
    console.log('Device positions saved')
}

function loadDevicePositions() {
    const saved = localStorage.getItem('rcm_device_positions')
    return saved ? JSON.parse(saved) : {}
}

function getSavedPosition(deviceId, defaultX, defaultY) {
    const positions = loadDevicePositions()
    return positions[deviceId] || { x: defaultX, y: defaultY }
}

function resetAllPositions() {
    if (!confirm('Reset all device positions to default? This will center all devices on the canvas.')) return

    localStorage.removeItem('rcm_device_positions')

    // Reload configuration to apply default positions
    loadConfiguration()
}

// Color management
function saveDeviceColors() {
    const colors = {}

    for (let switchId in switches) {
        if (switches[switchId].color) {
            colors[`switch-${switchId}`] = switches[switchId].color
        }
    }

    localStorage.setItem('rcm_device_colors', JSON.stringify(colors))
    console.log('Device colors saved')
}

function loadDeviceColors() {
    const saved = localStorage.getItem('rcm_device_colors')
    return saved ? JSON.parse(saved) : {}
}

function getSavedColor(deviceId) {
    const colors = loadDeviceColors()
    return colors[deviceId] || null
}

// Hide/Show devices
function saveHiddenDevices() {
    localStorage.setItem('rcm_hidden_devices', JSON.stringify([...hiddenDevices]))
    console.log('Hidden devices saved')
}

function loadHiddenDevices() {
    const saved = localStorage.getItem('rcm_hidden_devices')
    return saved ? new Set(JSON.parse(saved)) : new Set()
}

// Hidden buttons management
function saveHiddenButtons() {
    localStorage.setItem('rcm_hidden_buttons', JSON.stringify([...hiddenButtons]))
    console.log('Hidden buttons saved')
}

function loadHiddenButtons() {
    const saved = localStorage.getItem('rcm_hidden_buttons')
    return saved ? new Set(JSON.parse(saved)) : new Set()
}

function isButtonHidden(switchId, buttonId) {
    return hiddenButtons.has(`${switchId}-${buttonId}`)
}

function hideButton(switchId, buttonId) {
    const key = `${switchId}-${buttonId}`
    hiddenButtons.add(key)

    const buttonElement = document.getElementById(`switch-${switchId}-btn-${buttonId}`)
    if (buttonElement) {
        buttonElement.style.display = 'none'
    }

    // Hide connections
    const deviceConnections = connectionLookupMap[`switch-${switchId}`] || []
    deviceConnections.forEach(conn => {
        const sourceId = conn.sourceId.replace(`switch-${switchId}-btn-`, '')
        if (sourceId === buttonId) {
            conn.setVisible(false)
        }
    })

    updateShowHiddenButton(switchId)
    saveHiddenButtons()

    // Repaint jsPlumb
    if (jsPlumbInstance) {
        jsPlumbInstance.repaintEverything()
    }
}

function showButton(switchId, buttonId) {
    const key = `${switchId}-${buttonId}`
    hiddenButtons.delete(key)

    const buttonElement = document.getElementById(`switch-${switchId}-btn-${buttonId}`)
    if (buttonElement) {
        buttonElement.style.display = 'flex'
        buttonElement.style.pointerEvents = 'auto'
    }

    // Show connections
    const deviceConnections = connectionLookupMap[`switch-${switchId}`] || []
    deviceConnections.forEach(conn => {
        const sourceId = conn.sourceId.replace(`switch-${switchId}-btn-`, '')
        if (sourceId === buttonId) {
            conn.setVisible(true)
        }
    })

    updateShowHiddenButton(switchId)
    saveHiddenButtons()

    // Repaint jsPlumb
    if (jsPlumbInstance) {
        jsPlumbInstance.repaintEverything()
    }
}

function toggleShowHiddenButtons(switchId) {
    const container = document.getElementById(`switch-${switchId}-hidden-container`)
    const button = document.getElementById(`switch-${switchId}-show-hidden-btn`)

    if (!container || !button) return

    const isExpanded = container.style.display === 'block'

    if (isExpanded) {
        container.style.display = 'none'
        button.textContent = '▼ Show Hidden Buttons'
    } else {
        container.style.display = 'block'
        button.textContent = '▲ Hide Hidden Buttons'
    }
}

function updateShowHiddenButton(switchId) {
    const switchData = switches[switchId]
    if (!switchData) return

    const buttonCount = switchData.buttonCount
    let hasHiddenButtons = false
    let hiddenButtonsHTML = ''

    for (let i = 1; i <= buttonCount; i++) {
        const buttonId = String.fromCharCode(96 + i)
        if (isButtonHidden(switchId, buttonId)) {
            hasHiddenButtons = true
            hiddenButtonsHTML += `
                <div class="hidden-button-item" style="display: flex; align-items: center; padding: 0.5rem; margin: 0.25rem 0; background: rgba(100, 116, 139, 0.1); border-radius: 6px;">
                    <span style="flex: 1; font-size: 0.9rem; color: #94a3b8;">Button ${i}</span>
                    <button onclick="event.stopPropagation(); showButton(${switchId}, '${buttonId}')"
                            title="Show Button"
                            style="background: linear-gradient(135deg, #10b981 0%, #059669 100%); border: none; color: white; font-weight: bold; font-size: 11px; padding: 4px 12px; border-radius: 4px; cursor: pointer; transition: transform 0.2s;"
                            onmouseover="this.style.transform='scale(1.05)'"
                            onmouseout="this.style.transform='scale(1)'">Show</button>
                </div>
            `
        }
    }

    const showHiddenBtn = document.getElementById(`switch-${switchId}-show-hidden-btn`)
    if (showHiddenBtn) {
        showHiddenBtn.style.display = hasHiddenButtons ? 'block' : 'none'
    }

    // Update the hidden buttons container with new content
    const hiddenContainer = document.getElementById(`switch-${switchId}-hidden-container`)
    if (hiddenContainer) {
        hiddenContainer.innerHTML = hiddenButtonsHTML
    }
}

// Button types management
function saveButtonTypes() {
    console.log('Sending button types to backend')
    sendButtonTypesToWebSocket()
}

function getButtonType(switchId, buttonId) {
    return buttonTypes[switchId]?.[buttonId] ?? 0 // default to momentary
}

function setButtonType(switchId, buttonId, type) {
    if (!buttonTypes[switchId]) {
        buttonTypes[switchId] = {}
    }
    buttonTypes[switchId][buttonId] = type
    updateButtonTypeUI(switchId, buttonId, type)
    saveButtonTypes()
}

function toggleButtonType(switchId, buttonId) {
    const currentType = getButtonType(switchId, buttonId)
    const newType = currentType === 1 ? 0 : 1
    setButtonType(switchId, buttonId, newType)
}

function updateButtonTypeUI(switchId, buttonId, type) {
    const button = document.getElementById(`switch-${switchId}-btn-${buttonId}-type`)
    if (!button) return

    if (type === 0) {
        button.textContent = 'M'
        button.title = 'Momentary - Click to change to Toggle'
        button.style.background = 'linear-gradient(135deg, #f59e0b 0%, #d97706 100%)'
    } else {
        button.textContent = 'T'
        button.title = 'Toggle - Click to change to Momentary'
        button.style.background = 'linear-gradient(135deg, #6366f1 0%, #4f46e5 100%)'
    }
}

function sendButtonTypesToWebSocket() {
    if (!wsManager.isConnected()) {
        console.warn('WebSocket not connected, cannot send button types')
        return
    }

    // Build the data structure: { switchId: { buttonId: type, ... }, ... }
    const switchButtonTypes = {}

    for (let switchId in switches) {
        const buttonCount = switches[switchId].buttonCount
        switchButtonTypes[switchId] = {}

        for (let i = 1; i <= buttonCount; i++) {
            const buttonId = String.fromCharCode(96 + i)
            const type = getButtonType(switchId, buttonId)
            switchButtonTypes[switchId][buttonId] = type
        }
    }

    const msg = JSON.stringify({
        type: 'button_types',
        data: switchButtonTypes
    })

    console.log('Sending button types:', msg)
    wsManager.send(msg)
}

function hideDevice(deviceId, deviceType) {
    hiddenDevices.add(`${deviceType}-${deviceId}`)

    const element = document.getElementById(`${deviceType}-${deviceId}`)
    if (element) {
        element.style.display = 'none'
    }

    // Hide connections
    const deviceConnections = connectionLookupMap[`${deviceType}-${deviceId}`] || []
    deviceConnections.forEach(conn => {
        conn.setVisible(false)
    })

    // Repaint to remove endpoint dots immediately
    if (jsPlumbInstance) {
        jsPlumbInstance.repaintEverything()
    }
}

function showAllHiddenDevices() {
    hiddenDevices.forEach(deviceKey => {
        const element = document.getElementById(deviceKey)
        if (element) {
            element.style.display = 'block'
        }

        // Show connections
        const deviceConnections = connectionLookupMap[deviceKey] || []
        deviceConnections.forEach(conn => {
            conn.setVisible(true)
        })
    })

    hiddenDevices.clear()

    // Repaint to fix connection line positions immediately
    if (jsPlumbInstance) {
        jsPlumbInstance.repaintEverything()
    }
}

function showColorPicker(switchId) {
    const colors = [
        '#6366f1', // Default blue
        '#ef4444', // Red
        '#f59e0b', // Orange
        '#10b981', // Green
        '#8b5cf6', // Purple
        '#ec4899', // Pink
        '#06b6d4', // Cyan
        '#f97316', // Orange-red
        '#84cc16', // Lime
        '#a855f7', // Violet
        '#14b8a6', // Teal
        '#f43f5e', // Rose
        '#facc15', // Yellow
        '#fb923c', // Light orange
        '#4ade80', // Light green
        '#2dd4bf', // Light teal
        '#c084fc', // Light purple
        '#f472b6', // Light pink
        '#fb7185', // Watermelon
        '#fbbf24', // Amber
        '#22d3ee', // Sky blue
        '#a3e635', // Lime green
        '#fca5a5', // Coral
        '#d8b4fe'  // Lavender
    ]

    const colorButtons = colors.map(color =>
        `<button class="color-choice" style="background: ${color}; width: 48px; height: 48px; border: 2px solid #334155; border-radius: 8px; cursor: pointer; margin: 4px; transition: transform 0.2s;" onclick="setDeviceColor(${switchId}, '${color}')" onmouseover="this.style.transform='scale(1.15)'" onmouseout="this.style.transform='scale(1)'"></button>`
    ).join('')

    const modal = document.createElement('div')
    modal.id = 'colorPickerModal'
    modal.className = 'modal active'
    modal.innerHTML = `
        <div class="modal-content">
            <div class="modal-header">Choose Switch Color</div>
            <div style="display: flex; flex-wrap: wrap; justify-content: center; padding: 1rem;">
                ${colorButtons}
            </div>
            <div class="modal-buttons">
                <button class="modal-btn cancel" onclick="closeColorPicker()">Cancel</button>
            </div>
        </div>
    `
    document.body.appendChild(modal)
}

function closeColorPicker() {
    const modal = document.getElementById('colorPickerModal')
    if (modal) modal.remove()
}

function setDeviceColor(switchId, color) {
    switches[switchId].color = color

    // Update switch visual
    const switchElement = document.getElementById(`switch-${switchId}`)
    if (switchElement) {
        switchElement.style.borderLeftColor = color

        // Update all buttons in the switch
        const buttons = switchElement.querySelectorAll('.button-item')
        buttons.forEach(btn => {
            btn.style.background = `linear-gradient(135deg, ${color}33 0%, ${color}55 100%)`
            btn.style.borderColor = `${color}66`
        })
    }

    // Update connection colors
    const deviceId = `switch-${switchId}`
    const deviceConnections = connectionLookupMap[deviceId] || []

    deviceConnections.forEach(conn => {
        conn.setPaintStyle({ stroke: color, strokeWidth: 3 })
    })

    saveDeviceColors()
    closeColorPicker()
}

function applyDeviceColor(switchId) {
    if (!switches[switchId] || !switches[switchId].color) return

    const color = switches[switchId].color

    const switchElement = document.getElementById(`switch-${switchId}`)
    if (!switchElement) return

    switchElement.style.borderLeftColor = color

    const buttons = switchElement.querySelectorAll('.button-item')
    buttons.forEach(btn => {
        btn.style.background = `linear-gradient(135deg, ${color}33 0%, ${color}55 100%)`
        btn.style.borderColor = `${color}66`
    })

    // Update connection colors
    setTimeout(() => {
        const deviceId = `switch-${switchId}`
        const deviceConnections = connectionLookupMap[deviceId] || []

        deviceConnections.forEach(conn => {
            conn.setPaintStyle({ stroke: color, strokeWidth: 3 })
        })
    }, 150)
}

async function fetchAPI(endpoint) {
    try {
        const url = `${API_BASE_URL}${endpoint}`
        console.log('Fetching:', url)

        const response = await fetch(url)
        const text = await response.text()
        console.log('Response text:', text.substring(0, 200))

        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`)
        }

        try {
            return JSON.parse(text)
        } catch (e) {
            console.error('JSON parse error:', e)
            throw new Error(`Invalid JSON response`)
        }
    } catch (error) {
        console.error('Fetch error:', error)
        console.log('Using demo data instead')
        return null
    }
}

async function postForm(endpoint, data) {
    try {
        const formData = new FormData()
        Object.keys(data).forEach(key => {
            formData.append(key, data[key])
        })

        const url = `${API_BASE_URL}${endpoint}`
        console.log('Posting to:', url, data)

        const response = await fetch(url, {
            method: 'POST',
            body: formData
        })

        const text = await response.text()
        console.log('Post response:', text.substring(0, 200))

        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`)
        } else {
            wsManager.send(JSON.stringify({ "type": "update" }))
        }

        try {
            return JSON.parse(text)
        } catch (e) {
            console.log('Non-JSON response (might be OK):', text)
            return { success: true, message: text }
        }
    } catch (error) {
        console.error('Post error:', error)
        alert('Error posting data to server: ' + error.message)
        return null
    }
}

function bindJsPlumbEvents() {
    console.log('Binding jsPlumb events...')

    jsPlumbInstance.unbind("connection")
    jsPlumbInstance.unbind("connectionDetached")

    jsPlumbInstance.bind("connection", function (info, originalEvent) {
        console.log('Connection event fired, originalEvent:', originalEvent, 'isLoadingConnections:', isLoadingConnections)

        if (originalEvent && !isLoadingConnections) {
            const sourceId = info.sourceId
            const targetId = info.targetId

            const switchMatch = sourceId.match(/switch-(\d+)-btn-(\w+)/)
            const relayMatch = targetId.match(/relay-(\d+)-output-(\w+)/)

            if (switchMatch && relayMatch) {
                const switchId = parseInt(switchMatch[1])
                const buttonId = switchMatch[2]
                const relayId = parseInt(relayMatch[1])
                const outputId = relayMatch[2]

                console.log('User created connection:', { switchId, buttonId, relayId, outputId })

                postForm('/lights/add_connection', {
                    switch_id: switchId,
                    button_id: buttonId,
                    relay_id: relayId,
                    output_id: outputId
                }).then(result => {
                    if (result) {
                        if (!connections[switchId]) connections[switchId] = {}
                        if (!connections[switchId][buttonId]) connections[switchId][buttonId] = []
                        connections[switchId][buttonId].push({
                            relayId,
                            outputId,
                            connection: info.connection
                        })

                        // Update lookup map
                        const switchDeviceId = `switch-${switchId}`
                        const relayDeviceId = `relay-${relayId}`
                        if (!connectionLookupMap[switchDeviceId]) connectionLookupMap[switchDeviceId] = []
                        if (!connectionLookupMap[relayDeviceId]) connectionLookupMap[relayDeviceId] = []
                        connectionLookupMap[switchDeviceId].push(info.connection)
                        connectionLookupMap[relayDeviceId].push(info.connection)

                        console.log('Connection saved successfully')
                    } else {
                        console.error('Failed to save connection')
                        jsPlumbInstance.deleteConnection(info.connection)
                    }
                })
            }
        }
    })

    jsPlumbInstance.bind("connectionDetached", function (info, originalEvent) {
        if (originalEvent && !isLoadingConnections) {
            const sourceId = info.sourceId
            const targetId = info.targetId

            const switchMatch = sourceId.match(/switch-(\d+)-btn-(\w+)/)
            const relayMatch = targetId.match(/relay-(\d+)-output-(\w+)/)

            if (switchMatch && relayMatch) {
                const switchId = parseInt(switchMatch[1])
                const buttonId = switchMatch[2]
                const relayId = parseInt(relayMatch[1])
                const outputId = relayMatch[2]

                console.log('Connection removed:', { switchId, buttonId, relayId, outputId })

                postForm('/lights/remove_connection', {
                    switch_id: switchId,
                    button_id: buttonId,
                    relay_id: relayId,
                    output_id: outputId
                }).then(result => {
                    if (result) {
                        if (connections[switchId] && connections[switchId][buttonId]) {
                            connections[switchId][buttonId] = connections[switchId][buttonId].filter(
                                conn => !(conn.relayId === relayId && conn.outputId === outputId)
                            )
                        }

                        // Update lookup map
                        const switchDeviceId = `switch-${switchId}`
                        const relayDeviceId = `relay-${relayId}`
                        if (connectionLookupMap[switchDeviceId]) {
                            connectionLookupMap[switchDeviceId] = connectionLookupMap[switchDeviceId].filter(
                                c => !(c.sourceId === sourceId && c.targetId === targetId)
                            )
                        }
                        if (connectionLookupMap[relayDeviceId]) {
                            connectionLookupMap[relayDeviceId] = connectionLookupMap[relayDeviceId].filter(
                                c => !(c.sourceId === sourceId && c.targetId === targetId)
                            )
                        }

                        console.log('Connection removed successfully')
                    } else {
                        console.error('Failed to remove connection from API')
                    }
                })
            }
        }
    })

    jsPlumbInstance.bind("click", function (connection, e) {
        const { x, y } = { x: e.offsetX, y: e.offsetY }
        // Optional: implement a small distance check to ensure correct connection
        console.log("Clicked connection:", connection.sourceId, "->", connection.targetId)
    })

    jsPlumbInstance.bind("dblclick", function (connection, e) {
        e.preventDefault()

        const sourceId = connection.sourceId
        const targetId = connection.targetId

        const switchMatch = sourceId.match(/switch-(\d+)-btn-(\w+)/)
        const relayMatch = targetId.match(/relay-(\d+)-output-(\w+)/)

        if (switchMatch && relayMatch) {
            const switchId = parseInt(switchMatch[1])
            const buttonId = switchMatch[2]
            const relayId = parseInt(relayMatch[1])
            const outputId = relayMatch[2]

            console.log('Double-clicked connection, removing:', { switchId, buttonId, relayId, outputId })

            postForm('/lights/remove_connection', {
                switch_id: switchId,
                button_id: buttonId,
                relay_id: relayId,
                output_id: outputId
            }).then(result => {
                if (result) {
                    jsPlumbInstance.deleteConnection(connection)

                    if (connections[switchId] && connections[switchId][buttonId]) {
                        connections[switchId][buttonId] = connections[switchId][buttonId].filter(
                            conn => !(conn.relayId === relayId && conn.outputId === outputId)
                        )
                    }

                    // Update lookup map
                    const switchDeviceId = `switch-${switchId}`
                    const relayDeviceId = `relay-${relayId}`
                    if (connectionLookupMap[switchDeviceId]) {
                        connectionLookupMap[switchDeviceId] = connectionLookupMap[switchDeviceId].filter(c => c !== connection)
                    }
                    if (connectionLookupMap[relayDeviceId]) {
                        connectionLookupMap[relayDeviceId] = connectionLookupMap[relayDeviceId].filter(c => c !== connection)
                    }

                    console.log('Connection removed successfully')
                } else {
                    console.error('Failed to remove connection from API')
                }
            })
        }
    })
}

function highlightDevice(deviceId) {
    clearHighlights()

    const element = document.getElementById(deviceId)
    if (!element) return

    element.classList.add('highlighted')
    // Reinforce root highlight if this is the root device
    if (root_id && (deviceId === `switch-${root_id}` || deviceId === `relay-${root_id}`)) {
        element.classList.add('root')
    }
    highlightedDevice = deviceId

    // Dim all other devices
    document.querySelectorAll('.device-box').forEach(el => {
        if (el.id !== deviceId) {
            el.style.opacity = '0.3'
        } else {
            el.style.setProperty('opacity', '1', 'important')
        }
    })

    // Use lookup map for faster connection finding
    const deviceConnections = connectionLookupMap[deviceId] || []

    // Dim all connections first
    const allConnections = jsPlumbInstance.getAllConnections()
    allConnections.forEach(conn => {
        conn.canvas.style.opacity = '0.2'
        conn.canvas.classList.remove('highlighted')
        // Remove highlighted class from endpoints
        if (conn.endpoints) {
            conn.endpoints.forEach(ep => {
                if (ep.canvas) ep.canvas.classList.remove('highlighted')
            })
        }
    })

    deviceConnections.forEach(conn => {
        const sourceId = conn.sourceId
        const targetId = conn.targetId

        conn.setPaintStyle({ stroke: '#ef4444', strokeWidth: 5 })
        conn.canvas.style.opacity = '1'
        conn.canvas.classList.add('highlighted')

        // Add highlighted class to endpoints
        if (conn.endpoints) {
            conn.endpoints.forEach(ep => {
                if (ep.canvas) ep.canvas.classList.add('highlighted')
            })
        }

        if (sourceId.startsWith(deviceId)) {
            const targetMatch = targetId.match(/^(relay-\d+|switch-\d+)/)
            if (targetMatch) {
                const targetElement = document.getElementById(targetMatch[1])
                if (targetElement) {
                    targetElement.classList.add('highlighted')
                    targetElement.style.opacity = '1'
                }
            }
        } else {
            const sourceMatch = sourceId.match(/^(relay-\d+|switch-\d+)/)
            if (sourceMatch) {
                const sourceElement = document.getElementById(sourceMatch[1])
                if (sourceElement) {
                    sourceElement.classList.add('highlighted')
                    sourceElement.style.opacity = '1'
                }
            }
        }
    })

    console.log('Highlighted device:', deviceId)
}

function clearHighlights() {
    console.log('clearHighlights called')

    document.querySelectorAll('.device-box.highlighted').forEach(el => {
        el.classList.remove('highlighted')
    })

    // Restore full opacity to all devices by removing inline style
    document.querySelectorAll('.device-box').forEach(el => {
        el.style.removeProperty('opacity')
    })

    if (!jsPlumbInstance) return

    const allConnections = jsPlumbInstance.getAllConnections()
    allConnections.forEach(conn => {
        // Get the source switch ID to check for custom color
        const sourceId = conn.sourceId
        const switchMatch = sourceId.match(/switch-(\d+)-btn/)

        let color = '#6366f1' // default blue
        if (switchMatch) {
            const switchId = parseInt(switchMatch[1])
            if (switches[switchId] && switches[switchId].color) {
                color = switches[switchId].color
            }
        }

        conn.setPaintStyle({ stroke: color, strokeWidth: 3 })
        conn.canvas.style.opacity = '1'
        conn.canvas.classList.remove('highlighted')

        // Remove highlighted class from endpoints
        if (conn.endpoints) {
            conn.endpoints.forEach(ep => {
                if (ep.canvas) ep.canvas.classList.remove('highlighted')
            })
        }
    })

    highlightedDevice = null
}

function highlightButton(switchId, buttonId) {
    const buttonElement = document.getElementById(`switch-${switchId}-btn-${buttonId}`)
    if (!buttonElement) {
        console.warn(`Button not found: switch-${switchId}-btn-${buttonId}`)
        return
    }

    // Get the custom color if set, otherwise use default blue
    let targetColor = '#6366f1' // default blue
    if (switches[switchId] && switches[switchId].color) {
        targetColor = switches[switchId].color
    }

    // Set initial red background
    buttonElement.style.background = 'rgba(255, 0, 0, 1)'
    buttonElement.style.transition = 'none'

    // Force reflow to ensure the transition works
    buttonElement.offsetHeight

    // Convert hex color to rgba for gradient
    const r = parseInt(targetColor.slice(1, 3), 16)
    const g = parseInt(targetColor.slice(3, 5), 16)
    const b = parseInt(targetColor.slice(5, 7), 16)

    // Start fading to the switch's custom color over 5 seconds
    // Using ease-in: starts slow, ends fast
    requestAnimationFrame(() => {
        buttonElement.style.transition = 'background 5s ease-in'
        buttonElement.style.background = `linear-gradient(135deg, rgba(${r}, ${g}, ${b}, 0.2) 0%, rgba(${r}, ${g}, ${b}, 0.33) 100%)`
    })

    console.log('Highlighted button:', switchId, buttonId)
}

function highlightRoot() {
    if (!root_id) return

    const rootElement = document.getElementById(`relay-${root_id}`) || document.getElementById(`switch-${root_id}`)
    if (rootElement) {
        rootElement.classList.add('root')
        console.log('Highlighted root device:', root_id)
    }
}

function clearRootHighlight() {
    document.querySelectorAll('.device-box.root').forEach(el => {
        el.classList.remove('root')
    })
}

function updateDevice(deviceId, deviceType) {
    if (!wsManager.isConnected()) {
        alert('Not connected to server')
        return
    }

    const msg = JSON.stringify({
        type: 'update_device',
        device_id: deviceId,
        device_type: deviceType
    })

    console.log('Sending update request:', msg)
    wsManager.send(msg)
}

function updateAllRelays() {
    if (!wsManager.isConnected()) {
        alert('Not connected to server')
        return
    }

    const msg = JSON.stringify({ type: 'update_all_relays' })
    console.log('Updating all relays')
    wsManager.send(msg)
}

function updateAllSwitches() {
    if (!wsManager.isConnected()) {
        alert('Not connected to server')
        return
    }

    const msg = JSON.stringify({ type: 'update_all_switches' })
    console.log('Updating all switches')
    wsManager.send(msg)
}

function updateRoot() {
    if (!wsManager.isConnected()) {
        alert('Not connected to server')
        return
    }

    const msg = JSON.stringify({ type: 'update_root' })
    console.log('Updating root')
    wsManager.send(msg)
}

function clearButtonHighlight(switchId, buttonId) {
    const buttonElement = document.getElementById(`switch-${switchId}-btn-${buttonId}`)
    if (!buttonElement) {
        console.warn(`Button not found: switch-${switchId}-btn-${buttonId}`)
        return
    }

    // Get the custom color if set, otherwise use default blue
    let targetColor = '#6366f1' // default blue
    if (switches[switchId] && switches[switchId].color) {
        targetColor = switches[switchId].color
    }

    // Convert hex color to rgba for gradient
    const r = parseInt(targetColor.slice(1, 3), 16)
    const g = parseInt(targetColor.slice(3, 5), 16)
    const b = parseInt(targetColor.slice(5, 7), 16)

    // Restore to switch's color gradient
    buttonElement.style.background = `linear-gradient(135deg, ${targetColor}33 0%, ${targetColor}55 100%)`
    buttonElement.style.transition = ''
    console.log('Cleared button highlight:', switchId, buttonId)
}

// Helper function to add hover effect to a connection
function addConnectionHoverEffect(conn) {
    // Store original paint style
    if (!conn._originalPaintStyle) {
        conn._originalPaintStyle = conn.getPaintStyle()
    }

    // Get the connection canvas element
    const canvas = conn.canvas
    if (!canvas) return

    // Add hover effect using native DOM events
    canvas.addEventListener('mouseenter', function () {
        // When device is highlighted, only allow hover on highlighted connections
        if (highlightedDevice) {
            // Check if this connection is highlighted
            if (!canvas.classList.contains('highlighted')) {
                return
            }
            // If highlighted, make it even brighter
            conn.setPaintStyle({ stroke: '#ff6b6b', strokeWidth: 6 })
            canvas.style.cursor = 'pointer'
            return
        }

        const currentStyle = conn.getPaintStyle()
        const currentColor = currentStyle.stroke

        // Brighten the current color for hover
        let hoverColor = currentColor
        if (currentColor.startsWith('#')) {
            // Convert hex to rgb, brighten, and use
            const r = parseInt(currentColor.slice(1, 3), 16)
            const g = parseInt(currentColor.slice(3, 5), 16)
            const b = parseInt(currentColor.slice(5, 7), 16)

            // Brighten by adding to each channel (max 255)
            const brighten = 60
            const hr = Math.min(255, r + brighten)
            const hg = Math.min(255, g + brighten)
            const hb = Math.min(255, b + brighten)

            hoverColor = `rgb(${hr}, ${hg}, ${hb})`
        }

        conn.setPaintStyle({ stroke: hoverColor, strokeWidth: 5 })
        canvas.style.cursor = 'pointer'
    })

    canvas.addEventListener('mouseleave', function () {
        // When device is highlighted, restore highlighted style
        if (highlightedDevice && canvas.classList.contains('highlighted')) {
            conn.setPaintStyle({ stroke: '#ef4444', strokeWidth: 5 })
            canvas.style.cursor = 'default'
            return
        }

        // Don't change style if device is highlighted but this connection is not
        if (highlightedDevice) {
            return
        }

        // Restore original style
        if (conn._originalPaintStyle) {
            conn.setPaintStyle(conn._originalPaintStyle)
        }
        canvas.style.cursor = 'default'
    })
}

function initJsPlumb() {
    jsPlumb.ready(function () {
        jsPlumbInstance = jsPlumb.getInstance({
            Container: "canvas",
            Connector: ["Bezier", { curviness: 50 }],
            PaintStyle: {
                stroke: '#6366f1',
                strokeWidth: 3
            },
            Endpoint: ["Dot", { radius: 8 }],
            EndpointStyle: {
                fill: '#6366f1',
                stroke: '#4f46e5',
                strokeWidth: 2
            },
            EndpointHoverStyle: {
                fill: '#818cf8'
            },
            DragOptions: { cursor: 'move', zIndex: 2000 }
        })

        // Bind hover events to handle custom colors
        jsPlumbInstance.bind("connection", function (info) {
            addConnectionHoverEffect(info.connection)
        })

        bindJsPlumbEvents()
        initPanning()

        const canvas = document.getElementById('canvas')

        // Clear highlights on click (desktop)
        canvas.addEventListener('click', function (e) {
            // Clear highlights only if clicking on empty canvas (not on device box or connection line)
            const isDeviceBox = e.target.closest('.device-box')
            const isConnectionLine = e.target.tagName === 'svg' || e.target.tagName === 'path' || e.target.classList.contains('jtk-connector') || e.target.classList.contains('jtk-endpoint')

            console.log('Canvas click:', { target: e.target, isDeviceBox, isConnectionLine })

            if (!isDeviceBox && !isConnectionLine) {
                console.log('Clearing highlights from canvas click')
                clearHighlights()
            }
        })

        // Clear highlights on tap (touch screens)
        let canvasTouchStart = null
        canvas.addEventListener('touchstart', function (e) {
            if (e.touches.length === 1) {
                canvasTouchStart = {
                    x: e.touches[0].clientX,
                    y: e.touches[0].clientY,
                    time: Date.now()
                }
            }
        })

        canvas.addEventListener('touchend', function (e) {
            if (canvasTouchStart && e.changedTouches.length > 0) {
                const touch = e.changedTouches[0]
                const dx = Math.abs(touch.clientX - canvasTouchStart.x)
                const dy = Math.abs(touch.clientY - canvasTouchStart.y)
                const duration = Date.now() - canvasTouchStart.time

                // Only clear if it was a tap (not a pan/swipe) and not on a device or connection line
                const target = document.elementFromPoint(touch.clientX, touch.clientY)
                const isDeviceBox = target && target.closest('.device-box')
                const isConnectionLine = target && (target.tagName === 'svg' || target.tagName === 'path' || target.classList.contains('jtk-connector') || target.classList.contains('jtk-endpoint'))

                if (dx < 10 && dy < 10 && duration < 200 && !isDeviceBox && !isConnectionLine) {
                    clearHighlights()
                }
            }
            canvasTouchStart = null
        })

        loadConfiguration()
    })
}

async function loadConfiguration() {
    showLoading()
    isLoadingConnections = true

    // Suspend drawing for better performance
    if (jsPlumbInstance) {
        jsPlumbInstance.batch(() => {
            jsPlumbInstance.deleteEveryConnection()
            jsPlumbInstance.deleteEveryEndpoint()
        })
    }

    // Clear canvas
    const canvas = cachedElements.canvas || document.getElementById('canvas')
    canvas.innerHTML = ''
    if (!cachedElements.canvas) cachedElements.canvas = canvas

    switches = {}
    relays = {}
    connections = {}
    connectionLookupMap = {}
    cachedElements = { canvas, zoomLevel: cachedElements.zoomLevel }
    hiddenDevices = new Set()  // Don't persist hidden devices across reloads
    buttonTypes = {}
    hiddenButtons = loadHiddenButtons()

    try {
        const [relaysData, outputsData, switchesData, connectionsData, buttonTypesData] = await Promise.all([
            fetchAPI('/lights/get_relays'),
            fetchAPI('/lights/get_outputs'),
            fetchAPI('/lights/get_switches'),
            fetchAPI('/lights/get_connections'),
            fetchAPI('/lights/get_all_buttons')
        ])

        const relays_config = relaysData || getDemoRelays()
        const outputs_config = outputsData || getDemoOutputs()
        const switches_config = switchesData || getDemoSwitches()
        const connections_config = connectionsData || getDemoConnections()

        // Load button types
        if (buttonTypesData) {
            buttonTypes = buttonTypesData
            console.log('Loaded button types from API:', buttonTypes)
        }

        // create relays (centered on canvas by default)
        let relayX = 25000, relayY = 25000
        for (let [relayId, relayData] of Object.entries(relays_config)) {
            const relayName = Array.isArray(relayData) ? relayData[0] : relayData
            const outputsCount = Array.isArray(relayData) ? relayData[1] : 8
            createRelay(parseInt(relayId), relayName, outputs_config[relayId] || {}, relayX, relayY, outputsCount)
            relayY += 770
            if (relayY > 29000) { relayY = 25000; relayX += 350; }
        }

        // create switches (centered on canvas by default)
        let switchX = 23500, switchY = 25000
        for (let [switchId, switchData] of Object.entries(switches_config)) {
            if (!switchData || !Array.isArray(switchData) || switchData.length < 2) {
                console.warn(`Invalid switch data for switch ${switchId}:`, switchData)
                continue
            }
            const [switchName, buttonCount] = switchData
            createSwitch(parseInt(switchId), switchName, buttonCount, switchX, switchY)
            switchY += buttonCount * 72 + 185
            if (switchY > 29000) { switchY = 25000; switchX += 500; }
        }

        // Apply button types UI after switches are created
        for (let switchId in buttonTypes) {
            for (let buttonId in buttonTypes[switchId]) {
                const type = buttonTypes[switchId][buttonId]
                updateButtonTypeUI(switchId, buttonId, type)
            }
        }

        // Create connections after elements exist (batched for performance)
        setTimeout(() => {
            jsPlumbInstance.batch(() => {
                for (let [switchId, buttons] of Object.entries(connections_config)) {
                    for (let [buttonId, targets] of Object.entries(buttons)) {
                        // Skip connection if button is hidden
                        if (isButtonHidden(parseInt(switchId), buttonId)) {
                            continue
                        }
                        for (let [relayId, outputId] of targets) {
                            createConnection(parseInt(switchId), buttonId, parseInt(relayId), outputId)
                        }
                    }
                }
            })

            bindJsPlumbEvents()
            isLoadingConnections = false
            hideLoading()

            // Apply hidden state
            hiddenDevices.forEach(deviceKey => {
                const element = document.getElementById(deviceKey)
                if (element) {
                    element.style.display = 'none'
                }

                const deviceConnections = connectionLookupMap[deviceKey] || []
                deviceConnections.forEach(conn => {
                    conn.setVisible(false)
                })
            })
        }, 100)

        wsManager.send(JSON.stringify({ "type": "get_states" }))
    } catch (error) {
        console.error('Error loading configuration:', error)
        isLoadingConnections = false
        hideLoading()
    }
}

function getDemoRelays() {
    return {
        1: "Światła Przedpokój",
        2: "Rolety Przedpokój",
        3: "Światła Kuchnia"
    }
}

function getDemoOutputs() {
    return {
        1: { "1": "Warsztat 1", "2": "Warsztat 2", "3": "Kinkiet 1", "4": "Kinkiet 2", "5": "Przedpokój", "6": "Pokój f", "7": "Output 7", "8": "Output 8" },
        2: { "1": "Output 1", "2": "Output 2", "3": "Output 3", "4": "Output 4", "5": "Output 5", "6": "Output 6", "7": "Output 7", "8": "Output 8" },
        3: { "1": "Spiżarnia", "2": "Output 2", "3": "Output 3", "4": "Output 4", "5": "Downlight 1", "6": "Downlight 2", "7": "Downlight 3", "8": "Downlight 4" }
    }
}

function getDemoSwitches() {
    return {
        1: ["Wejście Do Domu", 3],
        2: ["Pokój a", 3],
        3: ["Pokój f", 3],
        4: ["Salon Wejście", 4],
        5: ["Schody Dół", 3],
        6: ["Spiżarnia Drzwi", 1]
    }
}

function getDemoConnections() {
    return {
        1: {
            "1": [[1, "5"], [1, "1"]],
            "2": [[1, "6"]],
            "3": [[1, "7"]]
        },
        2: {
            "1": [[1, "7"]],
            "2": [[1, "3"]],
            "3": [[2, "1"]]
        },
        3: {
            "1": [[1, "6"]],
            "2": [[2, "2"]]
        },
        4: {
            "1": [[3, "5"]],
            "2": [[3, "6"]],
            "3": [[3, "7"]],
            "4": [[3, "8"]]
        },
        6: {
            "1": [[3, "1"]]
        }
    }
}

// ========== DEVICE CREATION ==========

// Returns an inline SVG 4-bar signal-strength icon for the given RSSI (dBm)
// Thresholds: >= -55 → 4 bars green, >= -67 → 3 bars lime,
//             >= -78 → 2 bars orange, else → 1 bar red, null → 0 bars gray
function getRssiIcon(rssi) {
    let bars = 0
    let color = '#64748b'
    if (rssi !== null && rssi !== undefined) {
        if (rssi >= -55) { bars = 4; color = '#10b981' } // green
        else if (rssi >= -67) { bars = 3; color = '#84cc16' } // lime
        else if (rssi >= -78) { bars = 2; color = '#f59e0b' } // orange
        else { bars = 1; color = '#ef4444' } // red
    }
    const barHeights = [6, 10, 15, 20]
    const totalH = 22
    const barW = 5
    const gap = 2
    const totalW = 4 * barW + 3 * gap
    let svgBars = ''
    for (let i = 0; i < 4; i++) {
        const h = barHeights[i]
        const x = i * (barW + gap)
        const y = totalH - h
        const fill = i < bars ? color : '#334155'
        svgBars += `<rect x="${x}" y="${y}" width="${barW}" height="${h}" rx="1.5" fill="${fill}"/>`
    }
    return `<svg width="${totalW}" height="${totalH}" viewBox="0 0 ${totalW} ${totalH}" style="display:inline-block;vertical-align:middle;">${svgBars}</svg>`
}

function createSwitch(switchId, switchName, buttonCount, x, y) {
    const savedPos = getSavedPosition(`switch-${switchId}`, x, y)
    const savedColor = getSavedColor(`switch-${switchId}`)

    const switchDiv = document.createElement('div')
    switchDiv.id = `switch-${switchId}`
    switchDiv.className = 'device-box switch-box'
    switchDiv.style.left = `${savedPos.x}px`
    switchDiv.style.top = `${savedPos.y}px`
    switchDiv.style.zIndex = '100'

    if (savedColor) {
        switchDiv.style.borderLeftColor = savedColor
    }

    let buttonsHTML = ''
    let hiddenButtonsHTML = ''
    let hasHiddenButtons = false

    for (let i = 1; i <= buttonCount; i++) {
        const buttonId = String.fromCharCode(96 + i)
        const buttonType = getButtonType(switchId, buttonId)
        const typeLabel = buttonType === 0 ? 'M' : 'T'
        const typeTitle = buttonType === 0 ? 'Momentary - Click to change to Toggle' : 'Toggle - Click to change to Momentary'
        const typeColor = buttonType === 0 ? 'linear-gradient(135deg, #f59e0b 0%, #d97706 100%)' : 'linear-gradient(135deg, #6366f1 0%, #4f46e5 100%)'
        const isHidden = isButtonHidden(switchId, buttonId)
        const displayStyle = isHidden ? 'none' : 'flex'

        if (isHidden) hasHiddenButtons = true

        buttonsHTML += `
                    <div class="button-item" id="switch-${switchId}-btn-${buttonId}" style="display: ${displayStyle};">
                        <button class="button-type-toggle" 
                                id="switch-${switchId}-btn-${buttonId}-type"
                                onclick="event.stopPropagation(); toggleButtonType(${switchId}, '${buttonId}')"
                                title="${typeTitle}"
                                style="background: ${typeColor}; border: none; color: white; font-weight: bold; font-size: 12px; width: 24px; height: 24px; border-radius: 4px; cursor: pointer; margin-right: 8px; flex-shrink: 0; transition: transform 0.2s;"
                                onmouseover="this.style.transform='scale(1.1)'"
                                onmouseout="this.style.transform='scale(1)'">${typeLabel}</button>
                        <span class="button-name" style="flex: 1;">Button ${i}</span>
                        <button class="button-hide-toggle" 
                                onclick="event.stopPropagation(); hideButton(${switchId}, '${buttonId}')"
                                title="Hide Button"
                                style="background: linear-gradient(135deg, #64748b 0%, #475569 100%); border: none; color: white; font-weight: bold; font-size: 10px; width: 20px; height: 20px; border-radius: 4px; cursor: pointer; margin-left: 8px; flex-shrink: 0; transition: transform 0.2s;"
                                onmouseover="this.style.transform='scale(1.1)'"
                                onmouseout="this.style.transform='scale(1)'">👁️</button>
                        <span class="item-icon">🔘</span>
                    </div>
                `

        // Only add to hidden buttons section if actually hidden
        if (isHidden) {
            hiddenButtonsHTML += `
                        <div class="hidden-button-item" style="display: flex; align-items: center; padding: 0.5rem; margin: 0.25rem 0; background: rgba(100, 116, 139, 0.1); border-radius: 6px;">
                            <span style="flex: 1; font-size: 0.9rem; color: #94a3b8;">Button ${i}</span>
                            <button onclick="event.stopPropagation(); showButton(${switchId}, '${buttonId}')"
                                    title="Show Button"
                                    style="background: linear-gradient(135deg, #10b981 0%, #059669 100%); border: none; color: white; font-weight: bold; font-size: 11px; padding: 4px 12px; border-radius: 4px; cursor: pointer; transition: transform 0.2s;"
                                    onmouseover="this.style.transform='scale(1.05)'"
                                    onmouseout="this.style.transform='scale(1)'">Show</button>
                        </div>
                    `
        }
    }

    const isOnline = online_switches.has(switchId)
    const isUpToDate = up_to_date_devices[switchId]
    let statusClass
    if (!isOnline) {
        statusClass = 'status-offline' // red
    } else if (isUpToDate) {
        statusClass = 'status-online' // green
    } else {
        statusClass = 'status-outdated' // orange/yellow
    }
    const statusDot = `<span class="status-indicator ${statusClass}"></span>`

    const rssiSwitch = devices_rssi[switchId]
    const rssiTitleSwitch = rssiSwitch !== undefined ? `RSSI: ${rssiSwitch} dBm` : 'RSSI: N/A'

    switchDiv.innerHTML = `
                <div class="device-header">
                    <span>
                    ${statusDot}
                    <span class="device-id" onclick="event.stopPropagation(); copyIdToClipboard(${switchId}, this)">ID: ${switchId}</span>
                    </span>
                    <div style="display: flex; gap: 0.5rem;">
                        <button class="hide-btn" onclick="event.stopPropagation(); hideDevice(${switchId}, 'switch')" title="Hide Device">👁️</button>
                        <button class="update-btn update-btn-switch" onclick="event.stopPropagation(); updateDevice(${switchId}, 'switch')" title="Update Device">⟳</button>
                        <button class="delete-btn" onclick="event.stopPropagation(); deleteSwitch(${switchId})">✕</button>
                    </div>
                </div>
                <div style="display: flex; align-items: center; gap: 0.5rem; margin-bottom: 1rem;">
                    <div class="device-name device-name-switch-${switchId}" style="flex: 1; margin-bottom: 0; cursor: pointer;">${switchName}</div>
                    <span class="signal-icon signal-icon-switch-${switchId}" title="${rssiTitleSwitch}">${getRssiIcon(rssiSwitch)}</span>
                    <button class="color-btn" onclick="event.stopPropagation(); showColorPicker(${switchId})" title="Change Color">🎨</button>
                </div>
                ${buttonsHTML}
                <button id="switch-${switchId}-show-hidden-btn" 
                        onclick="event.stopPropagation(); toggleShowHiddenButtons(${switchId})"
                        style="display: ${hasHiddenButtons ? 'block' : 'none'}; width: 100%; padding: 8px; margin-top: 8px; background: linear-gradient(135deg, #475569 0%, #334155 100%); border: none; color: white; font-size: 12px; font-weight: 600; border-radius: 6px; cursor: pointer; transition: all 0.2s;"
                        onmouseover="this.style.background='linear-gradient(135deg, #64748b 0%, #475569 100%)'"
                        onmouseout="this.style.background='linear-gradient(135deg, #475569 0%, #334155 100%)'">▼ Show Hidden Buttons</button>
                <div id="switch-${switchId}-hidden-container" style="display: none; margin-top: 8px; padding: 8px; background: rgba(30, 41, 59, 0.5); border-radius: 6px; position: relative; z-index: 10000;">
                    ${hiddenButtonsHTML}
                </div>
        `

    document.getElementById('canvas').appendChild(switchDiv)

    // Add click and touch handler for device name edit
    const deviceNameElement = switchDiv.querySelector(`.device-name-switch-${switchId}`)
    if (deviceNameElement) {
        addClickAndTouchHandler(deviceNameElement, (e) => {
            e.stopPropagation()
            editDeviceName('switch', switchId)
        })
    }

    let isDragging = false
    let dragStartTime = 0
    let touchStartPos = null

    // Bring device to front on interaction
    function bringToFront() {
        const allDevices = document.querySelectorAll('.device-box')
        let maxZ = 100
        allDevices.forEach(dev => {
            const z = parseInt(dev.style.zIndex || 100)
            if (z > maxZ) maxZ = z
        })
        switchDiv.style.zIndex = maxZ + 1
    }

    switchDiv.addEventListener('mousedown', function () {
        isDragging = false
        dragStartTime = Date.now()
        bringToFront()
    })

    switchDiv.addEventListener('touchstart', function (e) {
        isDragging = false
        dragStartTime = Date.now()
        bringToFront()
        if (e.touches.length === 1) {
            touchStartPos = {
                x: e.touches[0].clientX,
                y: e.touches[0].clientY
            }
        }
    })

    switchDiv.addEventListener('touchend', function (e) {
        // Check if it was a tap (not a drag) and quick
        if (!isDragging && (Date.now() - dragStartTime) < 200) {
            // Verify minimal movement (tap, not swipe)
            if (touchStartPos && e.changedTouches.length > 0) {
                const touch = e.changedTouches[0]
                const dx = Math.abs(touch.clientX - touchStartPos.x)
                const dy = Math.abs(touch.clientY - touchStartPos.y)
                if (dx < 10 && dy < 10) {
                    highlightDevice(`switch-${switchId
                        } `)
                    e.preventDefault()
                }
            }
        }
        touchStartPos = null
    })

    switchDiv.addEventListener('click', function (e) {
        // Only highlight if not dragging and click was quick
        if (!isDragging && (Date.now() - dragStartTime) < 200) {
            highlightDevice(`switch-${switchId}`)
        }
    })

    jsPlumbInstance.draggable(switchDiv, {
        containment: false,
        start: function () {
            isDragging = true
            isCardDragging = true
        },
        stop: function () {
            saveDevicePositions()
            // Reset drag state after a short delay
            setTimeout(() => {
                isDragging = false
                isCardDragging = false
            }, 50)
        }
    })

    // Prevent triggering any child clicks while dragging (capture phase)
    switchDiv.addEventListener('click', function (e) {
        if ((isDragging || isCardDragging) && e.target === switchDiv) {
            e.stopPropagation()
            e.preventDefault()
        }
    }, true)
    switchDiv.addEventListener('touchend', function (e) {
        if ((isDragging || isCardDragging) && e.target === switchDiv) {
            e.stopPropagation()
            e.preventDefault()
        }
    }, true)

    for (let i = 1; i <= buttonCount; i++) {
        const btnElement = document.getElementById(`switch-${switchId}-btn-${String.fromCharCode(96 + i)}`)

        if (!btnElement) {
            console.warn(`Button element not found: switch-${switchId}-btn-${String.fromCharCode(96 + i)}`)
            continue
        }

        jsPlumbInstance.makeSource(btnElement, {
            anchor: "Right",
            endpoint: ["Dot", { radius: 8 }],
            paintStyle: { fill: "#6366f1", stroke: "#4f46e5", strokeWidth: 2 },
            connectorStyle: { stroke: "#6366f1", strokeWidth: 3 },
            maxConnections: -1
        })
    }

    switches[switchId] = { name: switchName, buttonCount, element: switchDiv, color: savedColor }

    // Apply saved color after DOM is ready
    if (savedColor) {
        applyDeviceColor(switchId)
    }
}

function copyIdToClipboard(id, element) {
    navigator.clipboard.writeText(id).then(() => {
        // Get element position
        const rect = element.getBoundingClientRect()

        // Create message element
        const message = document.createElement('div')
        message.textContent = `ID ${id} copied!`
        message.style.cssText = `
position: fixed;
top: ${rect.top - 40}px;
left: ${rect.left + rect.width / 2}px;
transform: translateX(-50%);
background: #10b981;
color: white;
padding: 8px 16px;
border-radius: 6px;
font-weight: 500;
font-size: 12px;
z-index: 10000;
box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
pointer-events: none;
`
        document.body.appendChild(message)

        // Remove after 1.5 seconds
        setTimeout(() => {
            message.remove()
        }, 1500)
    }).catch(err => {
        console.error('Failed to copy ID:', err)
    })
}

function createRelay(relayId, relayName, outputs, x, y, outputsCount = 8) {
    const savedPos = getSavedPosition(`relay-${relayId}`, x, y)

    const relayDiv = document.createElement('div')
    relayDiv.id = `relay-${relayId}`
    relayDiv.className = 'device-box relay-box'
    relayDiv.style.left = `${savedPos.x}px`
    relayDiv.style.top = `${savedPos.y}px`
    relayDiv.style.zIndex = '100'

    let outputsHTML = ''
    for (let i = 1; i <= outputsCount; i++) {
        let outputName = outputs[String.fromCharCode(96 + i)] || `Output ${i}`
        if (Array.isArray(outputName)) {
            outputName = outputName[0]
        }
        outputsHTML += `
                    <div class="output-item" id="relay-${relayId}-output-${String.fromCharCode(96 + i)}">
                        <span><img class="item-icon light-bulb-${relayId}-${String.fromCharCode(96 + i)}" src="/static/data/img/off.png" alt="switch" style="cursor: pointer;"></span>
                        <span class="output-name output-name-${relayId}-${String.fromCharCode(96 + i)}" style="cursor: pointer;">${outputName}</span>
                    </div>
                `
    }

    const isOnline = online_relays.has(relayId)
    const isUpToDate = up_to_date_devices[relayId]
    let statusClass
    if (!isOnline) {
        statusClass = 'status-offline' // red
    } else if (isUpToDate) {
        statusClass = 'status-online' // green
    } else {
        statusClass = 'status-outdated' // orange/yellow
    }
    const statusDot = `<span class="status-indicator ${statusClass}"></span>`
    const rssiRelay = devices_rssi[relayId]
    const rssiTitleRelay = rssiRelay !== undefined ? `RSSI: ${rssiRelay} dBm` : 'RSSI: N/A'

    relayDiv.innerHTML = `
                <div class="device-header">
                    <span>
                    ${statusDot}
                    <span class="device-id" onclick="event.stopPropagation(); copyIdToClipboard(${relayId}, this)">ID: ${relayId}</span>
                    </span>
                    <div style="display: flex; gap: 0.5rem;">
                        <button class="hide-btn" onclick="event.stopPropagation(); hideDevice(${relayId}, 'relay')" title="Hide Device">👁️</button>
                        <button class="update-btn update-btn-relay" onclick="event.stopPropagation(); updateDevice(${relayId}, 'relay')" title="Update Device">⟳</button>
                        <button class="delete-btn" onclick="event.stopPropagation(); deleteRelay(${relayId})">✕</button>
                    </div>
                </div>
                <div style="display: flex; align-items: center; gap: 0.5rem; margin-bottom: 1rem;">
                    <div class="device-name device-name-relay-${relayId}" style="flex: 1; margin-bottom: 0; cursor: pointer;">${relayName}</div>
                    <span class="signal-icon signal-icon-relay-${relayId}" title="${rssiTitleRelay}">${getRssiIcon(rssiRelay)}</span>
                </div>
                ${outputsHTML}
            `

    document.getElementById('canvas').appendChild(relayDiv)

    // Add click and touch handlers for light bulbs and output names
    for (let i = 1; i <= outputsCount; i++) {
        const outputId = String.fromCharCode(96 + i)

        // Light bulb toggle
        const bulbElement = relayDiv.querySelector(`.light-bulb-${relayId}-${outputId}`)
        if (bulbElement) {
            addClickAndTouchHandler(bulbElement, (e) => {
                e.stopPropagation()
                changeSwitchState(relayId, outputId)
            })
        }

        // Output name edit
        const nameElement = relayDiv.querySelector(`.output-name-${relayId}-${outputId}`)
        if (nameElement) {
            addClickAndTouchHandler(nameElement, (e) => {
                e.stopPropagation()
                editOutputName(relayId, outputId)
            })
        }
    }

    // Device name edit
    const deviceNameElement = relayDiv.querySelector(`.device-name-relay-${relayId}`)
    if (deviceNameElement) {
        addClickAndTouchHandler(deviceNameElement, (e) => {
            e.stopPropagation()
            editDeviceName('relay', relayId)
        })
    }

    let isDragging = false
    let dragStartTime = 0
    let touchStartPos = null

    // Bring device to front on interaction
    function bringToFront() {
        const allDevices = document.querySelectorAll('.device-box')
        let maxZ = 100
        allDevices.forEach(dev => {
            const z = parseInt(dev.style.zIndex || 100)
            if (z > maxZ) maxZ = z
        })
        relayDiv.style.zIndex = maxZ + 1
    }

    relayDiv.addEventListener('mousedown', function () {
        isDragging = false
        dragStartTime = Date.now()
        bringToFront()
    })

    relayDiv.addEventListener('touchstart', function (e) {
        isDragging = false
        dragStartTime = Date.now()
        bringToFront()
        if (e.touches.length === 1) {
            touchStartPos = {
                x: e.touches[0].clientX,
                y: e.touches[0].clientY
            }
        }
    })

    relayDiv.addEventListener('touchend', function (e) {
        // Check if it was a tap (not a drag) and quick
        if (!isDragging && (Date.now() - dragStartTime) < 200) {
            // Verify minimal movement (tap, not swipe)
            if (touchStartPos && e.changedTouches.length > 0) {
                const touch = e.changedTouches[0]
                const dx = Math.abs(touch.clientX - touchStartPos.x)
                const dy = Math.abs(touch.clientY - touchStartPos.y)
                if (dx < 10 && dy < 10) {
                    highlightDevice(`relay-${relayId}`)
                    e.preventDefault()
                }
            }
        }
        touchStartPos = null
    })

    relayDiv.addEventListener('click', function (e) {
        // Only highlight if not dragging and click was quick
        if (!isDragging && (Date.now() - dragStartTime) < 200) {
            highlightDevice(`relay-${relayId}`)
        }
    })

    jsPlumbInstance.draggable(relayDiv, {
        containment: false,
        start: function () {
            isDragging = true
            isCardDragging = true
        },
        stop: function () {
            saveDevicePositions()
            // Reset drag state after a short delay
            setTimeout(() => {
                isDragging = false
                isCardDragging = false
            }, 50)
        }
    })

    // Prevent triggering any child clicks while dragging (capture phase)
    relayDiv.addEventListener('click', function (e) {
        if ((isDragging || isCardDragging) && e.target === relayDiv) {
            e.stopPropagation()
            e.preventDefault()
        }
    }, true)
    relayDiv.addEventListener('touchend', function (e) {
        if ((isDragging || isCardDragging) && e.target === relayDiv) {
            e.stopPropagation()
            e.preventDefault()
        }
    }, true)

    for (let i = 1; i <= outputsCount; i++) {
        const outputElement = document.getElementById(`relay-${relayId}-output-${String.fromCharCode(96 + i)}`)

        if (!outputElement) {
            console.warn(`Output element not found: relay-${relayId}-output-${String.fromCharCode(96 + i)}`)
            continue
        }

        jsPlumbInstance.makeTarget(outputElement, {
            anchor: "Left",
            endpoint: ["Dot", { radius: 8 }],
            paintStyle: { fill: "#f59e0b", stroke: "#d97706", strokeWidth: 2 },
        })
    }

    relays[relayId] = { name: relayName, outputs, outputsCount, element: relayDiv }
}

// ========== UI UPDATES ==========

function updateOnlineStatus() {
    // Update all switches
    for (let switchId in switches) {
        const element = document.getElementById(`switch-${switchId}`)
        if (element) {
            const indicator = element.querySelector('.status-indicator')
            if (indicator) {
                const isOnline = online_switches.has(parseInt(switchId))
                const isUpToDate = up_to_date_devices[parseInt(switchId)]
                let statusClass
                if (!isOnline) {
                    statusClass = 'status-indicator status-offline' // red
                } else if (isUpToDate) {
                    statusClass = 'status-indicator status-online' // green
                } else {
                    statusClass = 'status-indicator status-outdated' // orange/yellow
                }
                indicator.className = statusClass
            }
            const rssiIcon = element.querySelector(`.signal-icon-switch-${switchId}`)
            if (rssiIcon) {
                const rssi = devices_rssi[parseInt(switchId)]
                rssiIcon.innerHTML = getRssiIcon(rssi)
                rssiIcon.title = rssi !== undefined ? `RSSI: ${rssi} dBm` : 'RSSI: N/A'
            }
        }
    }

    // Update all relays
    for (let relayId in relays) {
        const element = document.getElementById(`relay-${relayId}`)
        if (element) {
            const indicator = element.querySelector('.status-indicator')
            if (indicator) {
                const isOnline = online_relays.has(parseInt(relayId))
                const isUpToDate = up_to_date_devices[parseInt(relayId)]
                let statusClass
                if (!isOnline) {
                    statusClass = 'status-indicator status-offline' // red
                } else if (isUpToDate) {
                    statusClass = 'status-indicator status-online' // green
                } else {
                    statusClass = 'status-indicator status-outdated' // orange/yellow
                }
                indicator.className = statusClass
            }
            const rssiIcon = element.querySelector(`.signal-icon-relay-${relayId}`)
            if (rssiIcon) {
                const rssi = devices_rssi[parseInt(relayId)]
                rssiIcon.innerHTML = getRssiIcon(rssi)
                rssiIcon.title = rssi !== undefined ? `RSSI: ${rssi} dBm` : 'RSSI: N/A'
            }
        }
    }
}

function createConnection(switchId, buttonId, relayId, outputId) {
    const sourceElement = document.getElementById(`switch-${switchId}-btn-${buttonId}`)
    const targetElement = document.getElementById(`relay-${relayId}-output-${outputId}`)

    if (!sourceElement || !targetElement) {
        console.error('Source or target element not found', { switchId, buttonId, relayId, outputId })
        return
    }

    console.log('Creating connection from loaded data:', { switchId, buttonId, relayId, outputId })

    // Check if switch has custom color
    let connectionColor = '#6366f1' // default blue
    if (switches[switchId] && switches[switchId].color) {
        connectionColor = switches[switchId].color
    }

    const conn = jsPlumbInstance.connect({
        source: sourceElement,
        target: targetElement,
        paintStyle: { stroke: connectionColor, strokeWidth: 3 },
        endpoint: ["Dot", { radius: 8 }]
    })

    if (conn) {
        // Store original paint style for hover
        conn._originalPaintStyle = { stroke: connectionColor, strokeWidth: 3 }

        // Add hover effect
        addConnectionHoverEffect(conn)

        if (!connections[switchId]) connections[switchId] = {}
        if (!connections[switchId][buttonId]) connections[switchId][buttonId] = []
        connections[switchId][buttonId].push({ relayId, outputId, connection: conn })

        // Update lookup map for fast highlighting
        const switchDeviceId = `switch-${switchId}`
        const relayDeviceId = `relay - ${relayId} `

        if (!connectionLookupMap[switchDeviceId]) connectionLookupMap[switchDeviceId] = []
        if (!connectionLookupMap[relayDeviceId]) connectionLookupMap[relayDeviceId] = []

        connectionLookupMap[switchDeviceId].push(conn)
        connectionLookupMap[relayDeviceId].push(conn)
    }
}

// Helper function to add both click and touch support
function addClickAndTouchHandler(element, handler) {
    let touchHandled = false
    let touchStartTime = 0
    let touchStartPos = null

    element.addEventListener('touchstart', (e) => {
        touchStartTime = Date.now()
        if (e.touches.length === 1) {
            touchStartPos = {
                x: e.touches[0].clientX,
                y: e.touches[0].clientY
            }
        }
    }, { passive: true })

    element.addEventListener('touchend', (e) => {
        const touchDuration = Date.now() - touchStartTime
        let wasTap = touchDuration < 300

        // Check if touch moved significantly
        if (touchStartPos && e.changedTouches.length > 0) {
            const touch = e.changedTouches[0]
            const dx = Math.abs(touch.clientX - touchStartPos.x)
            const dy = Math.abs(touch.clientY - touchStartPos.y)
            if (dx > 10 || dy > 10) {
                wasTap = false
            }
        }

        touchStartPos = null

        if (wasTap && !isCardDragging) {
            e.preventDefault()
            e.stopPropagation()
            touchHandled = true
            handler(e)
            setTimeout(() => { touchHandled = false }, 300)
        }
    }, { passive: false })

    element.addEventListener('click', (e) => {
        if (!touchHandled && !isCardDragging) {
            handler(e)
        }
    })
}

function changeSwitchState(relay_id, output_id) {
    if (!wsManager.isConnected()) {
        return
    }

    if (pendingClicks.has(`${relay_id}-${output_id}`)) {
        return
    }

    pendingClicks.add(`${relay_id}-${output_id}`)
    var newState = lights[`${relay_id}-${output_id}`] === 1 ? 0 : 1

    console.log('Changing state of', `${relay_id}-${output_id}`, 'to', newState)
    var msg = JSON.stringify({ "relay_id": relay_id, "output_id": output_id, "state": newState })
    console.log(msg)

    if (!wsManager.send(msg)) {
        pendingClicks.delete(`${relay_id}-${output_id}`)

    }

    setTimeout(function () {
        pendingClicks.delete(`${relay_id}-${output_id}`)
    }, 2000)
}

function updateLightUI(relay_id, output_id, state) {
    console.log('Updating UI for', `relay-${relay_id}-output-${output_id}`, 'to', state)
    var outputElement = document.getElementById(`relay-${relay_id}-output-${output_id}`)
    if (!outputElement) {
        console.warn('Output element not found:', `relay-${relay_id}-output-${output_id}`)
        return
    }

    var element = outputElement.querySelector('img')
    if (!element) return

    if (state === 1) {
        element.src = "/static/data/img/on.png"
        element.parentElement.classList.add('active')
    } else {
        element.src = "/static/data/img/off.png"
        element.parentElement.classList.remove('active')
    }
}

// ========== MODAL DIALOGS ==========

function showAddSwitchModal() {
    document.getElementById('addSwitchModal').classList.add('active')
    document.getElementById('switchId').value = ''
    document.getElementById('switchName').value = ''
    document.getElementById('switchButtons').value = '3'
    document.addEventListener('keydown', function handler(e) {
        if (e.key === 'Enter') {
            addSwitch()
            document.removeEventListener('keydown', handler)
        }
        if (e.key === 'Escape') {
            closeModal('addSwitchModal')
            document.removeEventListener('keydown', handler)
        }
    })
}

function showAddRelayModal() {
    document.getElementById('addRelayModal').classList.add('active')
    document.getElementById('relayId').value = ''
    document.getElementById('relayName').value = ''
    document.addEventListener('keydown', function handler(e) {
        if (e.key === 'Enter') {
            addRelay()
            document.removeEventListener('keydown', handler)
        }
        if (e.key === 'Escape') {
            closeModal('addRelayModal')
            document.removeEventListener('keydown', handler)
        }
    })
}

function closeModal(modalId) {
    document.getElementById(modalId).classList.remove('active')
}

// ========== DEVICE CRUD OPERATIONS ==========

async function addSwitch() {
    const switchId = parseInt(document.getElementById('switchId').value)
    const switchName = document.getElementById('switchName').value
    const buttonCount = parseInt(document.getElementById('switchButtons').value)

    if (!switchId || !switchName) {
        alert('Please fill all fields')
        return
    }

    if (switches[switchId]) {
        alert('Switch ID already exists')
        return
    }

    const result = await postForm('/lights/add_switch', {
        switch_id: switchId,
        switch_name: switchName,
        buttons: buttonCount
    })

    if (result !== null) {
        const numSwitches = Object.keys(switches).length
        const x = 100 + (numSwitches % 3) * 350
        const y = 100 + Math.floor(numSwitches / 3) * 280
        createSwitch(switchId, switchName, buttonCount, x, y)
        closeModal('addSwitchModal')
    }
}

async function addRelay() {
    const relayId = parseInt(document.getElementById('relayId').value)
    const relayName = document.getElementById('relayName').value
    const outputsCount = parseInt(document.getElementById('relayOutputs').value) || 8

    if (!relayId || !relayName) {
        alert('Please fill all fields')
        return
    }

    if (relays[relayId]) {
        alert('Relay ID already exists')
        return
    }

    if (outputsCount !== 8 && outputsCount !== 16) {
        alert('Outputs must be either 8 or 16')
        return
    }

    const result = await postForm('/lights/add_relay', {
        relay_id: relayId,
        relay_name: relayName,
        outputs: outputsCount
    })

    if (result !== null) {
        const outputs = {}
        for (let i = 1; i <= outputsCount; i++) {
            outputs[String.fromCharCode(96 + i)] = `Output ${i} `
        }

        const numRelays = Object.keys(relays).length
        const x = 1200 + (numRelays % 3) * 350
        const y = 100 + Math.floor(numRelays / 3) * 450
        createRelay(relayId, relayName, outputs, x, y, outputsCount)
        closeModal('addRelayModal')
    }
}

async function deleteSwitch(switchId) {
    if (!confirm('Are you sure you want to delete this switch?')) return

    const result = await postForm('/lights/remove_switch', {
        switch_id: switchId
    })

    if (result !== null) {
        const element = document.getElementById(`switch-${switchId}`)
        if (element) {
            jsPlumbInstance.removeAllEndpoints(element)
            element.remove()
        }

        delete switches[switchId]
        delete connections[switchId]
        delete connectionLookupMap[`switch-${switchId}`]

        if (highlightedDevice === `switch-${switchId}`) {
            clearHighlights()
        }
    }
}

async function deleteRelay(relayId) {
    if (!confirm('Are you sure you want to delete this relay?')) return

    const result = await postForm('/lights/remove_relay', {
        relay_id: relayId
    })

    if (result !== null) {
        const element = document.getElementById(`relay - ${relayId} `)
        if (element) {
            jsPlumbInstance.removeAllEndpoints(element)
            element.remove()
        }

        delete relays[relayId]
        delete connectionLookupMap[`relay - ${relayId} `]

        if (highlightedDevice === `relay - ${relayId} `) {
            clearHighlights()
        }
    }
}

function editDeviceName(type, id) {
    currentEditTarget = { type, id }
    const currentName = type === 'switch' ? switches[id].name : relays[id].name
    document.getElementById('editNameInput').value = currentName
    document.getElementById('editNameModal').classList.add('active')

    if (type === 'switch') {
        document.getElementById('editButtonNumber').style.display = 'block'
        document.getElementById('editButtonNumber').value = switches[id].buttonCount
    } else if (type === 'relay') {
        document.getElementById('editButtonNumber').style.display = 'block'
        document.getElementById('editButtonNumber').value = relays[id].outputsCount || 8
    }

    document.addEventListener('keydown', function handler(e) {
        if (e.key === 'Enter') {
            saveNameEdit()
            document.removeEventListener('keydown', handler)
        }
        if (e.key === 'Escape') {
            closeModal('editNameModal')
            document.removeEventListener('keydown', handler)
        }
    })
}

function editOutputName(relayId, outputId) {
    currentEditTarget = { type: 'output', relayId, outputId }
    const outputs = relays[relayId].outputs
    let currentName = outputs[outputId] || outputs[outputId] || `Output ${outputId.charCodeAt(0) - 96} `
    if (Array.isArray(currentName)) {
        currentName = currentName[0]
    }
    document.getElementById('editNameInput').value = currentName
    document.getElementById('editNameModal').classList.add('active')

    document.addEventListener('keydown', function handler(e) {
        if (e.key === 'Enter') {
            saveNameEdit()
            document.removeEventListener('keydown', handler)
        }
        if (e.key === 'Escape') {
            closeModal('editNameModal')
            document.removeEventListener('keydown', handler)
        }
    })
}

async function saveNameEdit() {
    const newName = document.getElementById('editNameInput').value
    if (!newName) {
        alert('Name cannot be empty')
        return
    }

    if (currentEditTarget.type === 'output') {
        const result = await postForm('/lights/name_output', {
            relay_id: currentEditTarget.relayId,
            output_id: currentEditTarget.outputId,
            output_name: newName
        })

        if (result !== null) {
            // Keep the tuple structure (name, section_id)
            const currentOutput = relays[currentEditTarget.relayId].outputs[currentEditTarget.outputId]
            const sectionId = Array.isArray(currentOutput) ? currentOutput[1] : null
            relays[currentEditTarget.relayId].outputs[currentEditTarget.outputId] = [newName, sectionId]
            const outputElement = document.querySelector(`#relay-${currentEditTarget.relayId}-output-${currentEditTarget.outputId} .output-name`)
            if (outputElement) outputElement.textContent = newName
        }
    } else if (currentEditTarget.type === 'switch') {
        // Rename switch
        const buttonCount = parseInt(document.getElementById('editButtonNumber').value)
        const result = await postForm('/lights/rename_switch', {
            switch_id: currentEditTarget.id,
            switch_name: newName,
            buttons: buttonCount
        })

        if (result !== null) {
            switches[currentEditTarget.id].name = newName
            const nameElement = document.querySelector(`#switch - ${currentEditTarget.id} .device - name`)
            if (nameElement) nameElement.textContent = newName
        }

        document.getElementById('editButtonNumber').style.display = 'none'
    } else if (currentEditTarget.type === 'relay') {
        // Rename relay
        const outputsCount = parseInt(document.getElementById('editButtonNumber').value) || relays[currentEditTarget.id].outputsCount || 8

        if (outputsCount !== 8 && outputsCount !== 16) {
            alert('Outputs must be either 8 or 16')
            return
        }

        const result = await postForm('/lights/rename_relay', {
            relay_id: currentEditTarget.id,
            relay_name: newName,
            outputs: outputsCount
        })

        if (result !== null) {
            relays[currentEditTarget.id].name = newName
            const nameElement = document.querySelector(`#relay - ${currentEditTarget.id} .device - name`)
            if (nameElement) nameElement.textContent = newName
        }

        document.getElementById('editButtonNumber').style.display = 'none'
    }

    closeModal('editNameModal')
}

async function clearAllConnections() {
    if (!confirm('Are you sure you want to clear all connections?')) return

    showLoading()

    const removePromises = []

    for (let [switchId, buttons] of Object.entries(connections)) {
        for (let [buttonId, connList] of Object.entries(buttons)) {
            for (let conn of connList) {
                removePromises.push(
                    postForm('/lights/remove_connection', {
                        switch_id: switchId,
                        button_id: buttonId,
                        relay_id: conn.relayId,
                        output_id: conn.outputId
                    })
                )
            }
        }
    }

    await Promise.all(removePromises)

    jsPlumbInstance.batch(() => {
        jsPlumbInstance.deleteEveryConnection()
    })
    connections = {}
    connectionLookupMap = {}

    hideLoading()
    console.log('All connections cleared')
}

window.addEventListener('load', initJsPlumb)