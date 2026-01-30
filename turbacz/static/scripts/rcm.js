// Global variables
let jsPlumbInstance
let switches = {}
let relays = {}
let online_relays = new Set()
let online_switches = new Set()
let up_to_date_devices = {} // Maps device_id to boolean indicating if firmware is up to date
let connections = {}
var lights = {}
var pendingClicks = new Set()
let currentEditTarget = null
let highlightedDevice = null
let isLoadingConnections = false
let connectionLookupMap = {} // Maps device IDs to their connections for fast lookup
let zoomUpdateScheduled = false
let cachedElements = {}
let hiddenDevices = new Set() // Track hidden devices

// Zoom and Pan System - GPU Accelerated
function loadCanvasView() {
    const saved = localStorage.getItem('rcm_canvas_view')
    if (saved) {
        const view = JSON.parse(saved)
        return {
            zoomLevel: view.zoomLevel || 0.4,
            panX: view.panX !== undefined ? view.panX : getDefaultPanX(),
            panY: view.panY !== undefined ? view.panY : getDefaultPanY()
        }
    }
    return { zoomLevel: 0.4, panX: getDefaultPanX(), panY: getDefaultPanY() }
}

function getDefaultPanX() {
    const deviceCenterX = 25000
    const viewportCenterX = window.innerWidth / 2
    const defaultZoom = 0.4
    return viewportCenterX - (deviceCenterX * defaultZoom)
}

function getDefaultPanY() {
    const deviceCenterY = 25000
    const viewportCenterY = (window.innerHeight - 90) / 2
    const defaultZoom = 0.4
    return viewportCenterY - (deviceCenterY * defaultZoom)
}

const initialView = loadCanvasView()
let zoomLevel = initialView.zoomLevel
let panX = initialView.panX
let panY = initialView.panY
let isPanning = false
let isPinching = false
let startX = 0
let startY = 0
let lastPinchDistance = 0

// Cached DOM elements
let canvasElement = null
let zoomLevelElement = null
let transformPending = false
let lastDisplayedZoom = -1

const API_BASE_URL = `https://${window.location.host}`

// Performance utilities
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
var wsManager = new WebSocketManager('/rcm/ws/', function (event) {
    var msg = JSON.parse(event.data)
    console.log(msg)


    if (msg.type == "update") {
        loadConfiguration()
    }

    if (msg.type == "light_state") {
        pendingClicks.delete(`${msg.relay_id}-${msg.output_id}`)
        lights[`${msg.relay_id}-${msg.output_id}`] = msg.state
        updateLightUI(msg.relay_id, msg.output_id, msg.state)
    }

    if (msg.type == "online_status") {
        online_relays = new Set(msg.online_relays)
        online_switches = new Set(msg.online_switches)
        up_to_date_devices = msg.up_to_date_devices || {}

        console.log('Online relays:', online_relays)
        console.log('Online switches:', online_switches)
        console.log('Up to date devices:', up_to_date_devices)

        updateOnlineStatus()
    }

    if (msg.type == "switch_state" && msg.switch_id && msg.button_id) {
        highlightButton(msg.switch_id, msg.button_id)

        // Auto-clear highlight after 5 seconds
        setTimeout(() => {
            clearButtonHighlight(msg.switch_id, msg.button_id)
        }, 5000)
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
function applyCanvasTransform() {
    canvasElement.style.transform = `translate(${panX}px, ${panY}px) scale(${zoomLevel})`
}

function updateZoomDisplay() {
    const displayZoom = Math.round(zoomLevel * 100)
    if (displayZoom !== lastDisplayedZoom) {
        zoomLevelElement.innerText = `${displayZoom}%`
        lastDisplayedZoom = displayZoom
    }
}

function applyVisualTransform() {
    applyCanvasTransform()
    updateZoomDisplay()
}

function suspendJsPlumb() {
    if (jsPlumbInstance) {
        jsPlumbInstance.setSuspendDrawing(true)
    }
}

function resumeJsPlumb() {
    if (jsPlumbInstance) {
        jsPlumbInstance.setSuspendDrawing(false, true) // second param = repaint immediately
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
    if (commit) syncJsPlumb()
    else debouncedSyncJsPlumb()
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
    canvasElement.style.willChange = 'transform'
    canvasElement.style.backfaceVisibility = 'hidden'

    // ----------------- MOUSE PANNING -----------------
    wrapper.addEventListener('mousedown', e => {
        if (e.target !== wrapper && e.target !== canvasElement) return
        isPanning = true
        startX = e.clientX - panX
        startY = e.clientY - panY
        wrapper.classList.add('grabbing')
        suspendJsPlumb()
    })
    document.addEventListener('mousemove', e => {
        if (!isPanning) return
        panX = e.clientX - startX
        panY = e.clientY - startY
        applyCanvasTransform()
    })
    document.addEventListener('mouseup', () => {
        if (!isPanning) return
        isPanning = false
        wrapper.classList.remove('grabbing')
        resumeJsPlumb()
        saveCanvasView()
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
            e.preventDefault()
        }
    }, { passive: false })

    document.addEventListener('touchmove', e => {
        if (isPanning && e.touches.length === 1) {
            const touch = e.touches[0]
            panX = touch.clientX - startX
            panY = touch.clientY - startY
            applyCanvasTransform()
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
    }, { passive: false })

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
            debouncedSyncJsPlumb()
            setTimeout(saveCanvasView, 150)
        }
    })

    // ----------------- MOUSE WHEEL ZOOM -----------------
    let wheelTimeout = null
    const wheelHandler = e => {
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
        }, 200)

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
    highlightedDevice = deviceId

    // Dim all other devices
    document.querySelectorAll('.device-box').forEach(el => {
        if (el.id !== deviceId) {
            el.style.opacity = '0.3'
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
    document.querySelectorAll('.device-box.highlighted').forEach(el => {
        el.classList.remove('highlighted')
    })

    // Restore full opacity to all devices
    document.querySelectorAll('.device-box').forEach(el => {
        el.style.opacity = '1'
    })

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

    buttonElement.style.background = ''
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
            if (e.target.id === 'canvas') {
                clearHighlights()
            }
        })

        // Clear highlights on tap (touch screens)
        let canvasTouchStart = null
        canvas.addEventListener('touchstart', function (e) {
            if (e.target.id === 'canvas' && e.touches.length === 1) {
                canvasTouchStart = {
                    x: e.touches[0].clientX,
                    y: e.touches[0].clientY,
                    time: Date.now()
                }
            }
        })

        canvas.addEventListener('touchend', function (e) {
            if (e.target.id === 'canvas' && canvasTouchStart && e.changedTouches.length > 0) {
                const touch = e.changedTouches[0]
                const dx = Math.abs(touch.clientX - canvasTouchStart.x)
                const dy = Math.abs(touch.clientY - canvasTouchStart.y)
                const duration = Date.now() - canvasTouchStart.time

                // Only clear if it was a tap (not a pan/swipe)
                if (dx < 10 && dy < 10 && duration < 200) {
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

    try {
        const [relaysData, outputsData, switchesData, connectionsData] = await Promise.all([
            fetchAPI('/lights/get_relays'),
            fetchAPI('/lights/get_outputs'),
            fetchAPI('/lights/get_switches'),
            fetchAPI('/lights/get_connections')
        ])

        const relays_config = relaysData || getDemoRelays()
        const outputs_config = outputsData || getDemoOutputs()
        const switches_config = switchesData || getDemoSwitches()
        const connections_config = connectionsData || getDemoConnections()

        // create relays (centered on canvas by default)
        let relayX = 25000, relayY = 25000
        for (let [relayId, relayName] of Object.entries(relays_config)) {
            createRelay(parseInt(relayId), relayName, outputs_config[relayId] || {}, relayX, relayY)
            relayY += 770
            if (relayY > 29000) { relayY = 25000; relayX += 350; }
        }

        // create switches (centered on canvas by default)
        let switchX = 23500, switchY = 25000
        for (let [switchId, switchData] of Object.entries(switches_config)) {
            const [switchName, buttonCount] = switchData
            createSwitch(parseInt(switchId), switchName, buttonCount, switchX, switchY)
            switchY += buttonCount * 72 + 185
            if (switchY > 29000) { switchY = 25000; switchX += 500; }
        }

        // Create connections after elements exist (batched for performance)
        setTimeout(() => {
            jsPlumbInstance.batch(() => {
                for (let [switchId, buttons] of Object.entries(connections_config)) {
                    for (let [buttonId, targets] of Object.entries(buttons)) {
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
        1: { "1": "Warsztat 1", "2": "Warsztat 2", "3": "Kinkiet 1", "4": "Kinkiet 2", "5": "Przedpokój", "6": "Pokój Zo", "7": "Pokój Bobusi", "8": "Output 8" },
        2: { "1": "Output 1", "2": "Output 2", "3": "Output 3", "4": "Output 4", "5": "Output 5", "6": "Output 6", "7": "Output 7", "8": "Output 8" },
        3: { "1": "Spiżarnia", "2": "Output 2", "3": "Output 3", "4": "Output 4", "5": "Downlight 1", "6": "Downlight 2", "7": "Downlight 3", "8": "Downlight 4" }
    }
}

function getDemoSwitches() {
    return {
        1: ["Wejście Do Domu", 3],
        2: ["Pokój Bobusi", 3],
        3: ["Pokój Zo", 3],
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

function createSwitch(switchId, switchName, buttonCount, x, y) {
    const savedPos = getSavedPosition(`switch-${switchId}`, x, y)
    const savedColor = getSavedColor(`switch-${switchId}`)

    const switchDiv = document.createElement('div')
    switchDiv.id = `switch-${switchId}`
    switchDiv.className = 'device-box switch-box'
    switchDiv.style.left = `${savedPos.x}px`
    switchDiv.style.top = `${savedPos.y}px`

    if (savedColor) {
        switchDiv.style.borderLeftColor = savedColor
    }

    let buttonsHTML = ''
    for (let i = 1; i <= buttonCount; i++) {
        buttonsHTML += `
                    <div class="button-item" id="switch-${switchId}-btn-${String.fromCharCode(96 + i)}">
                        <span class="button-name">Button ${i}</span>
                        <span class="item-icon">🔘</span>
                    </div>
                `
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
                    <button class="color-btn" onclick="event.stopPropagation(); showColorPicker(${switchId})" title="Change Color">🎨</button>
                </div>
                ${buttonsHTML}
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

    switchDiv.addEventListener('mousedown', function () {
        isDragging = false
        dragStartTime = Date.now()
    })

    switchDiv.addEventListener('touchstart', function (e) {
        isDragging = false
        dragStartTime = Date.now()
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
                    highlightDevice(`switch-${switchId}`)
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
        },
        stop: function () {
            saveDevicePositions()
            // Reset drag state after a short delay
            setTimeout(() => {
                isDragging = false
            }, 100)
        }
    })

    for (let i = 1; i <= buttonCount; i++) {
        const btnElement = document.getElementById(`switch-${switchId}-btn-${String.fromCharCode(96 + i)}`)

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

function createRelay(relayId, relayName, outputs, x, y) {
    const savedPos = getSavedPosition(`relay-${relayId}`, x, y)

    const relayDiv = document.createElement('div')
    relayDiv.id = `relay-${relayId}`
    relayDiv.className = 'device-box relay-box'
    relayDiv.style.left = `${savedPos.x}px`
    relayDiv.style.top = `${savedPos.y}px`

    let outputsHTML = ''
    for (let i = 1; i <= 8; i++) {
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
                <div class="device-name device-name-relay-${relayId}" style="cursor: pointer;">${relayName}</div>
                ${outputsHTML}
            `

    document.getElementById('canvas').appendChild(relayDiv)

    // Add click and touch handlers for light bulbs and output names
    for (let i = 1; i <= 8; i++) {
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

    relayDiv.addEventListener('mousedown', function () {
        isDragging = false
        dragStartTime = Date.now()
    })

    relayDiv.addEventListener('touchstart', function (e) {
        isDragging = false
        dragStartTime = Date.now()
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
        },
        stop: function () {
            saveDevicePositions()
            // Reset drag state after a short delay
            setTimeout(() => {
                isDragging = false
            }, 100)
        }
    })

    for (let i = 1; i <= 8; i++) {
        const outputElement = document.getElementById(`relay-${relayId}-output-${String.fromCharCode(96 + i)}`)
        jsPlumbInstance.makeTarget(outputElement, {
            anchor: "Left",
            endpoint: ["Dot", { radius: 8 }],
            paintStyle: { fill: "#f59e0b", stroke: "#d97706", strokeWidth: 2 },
        })
    }

    relays[relayId] = { name: relayName, outputs, element: relayDiv }
}

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
        const relayDeviceId = `relay-${relayId}`

        if (!connectionLookupMap[switchDeviceId]) connectionLookupMap[switchDeviceId] = []
        if (!connectionLookupMap[relayDeviceId]) connectionLookupMap[relayDeviceId] = []

        connectionLookupMap[switchDeviceId].push(conn)
        connectionLookupMap[relayDeviceId].push(conn)
    }
}

// Helper function to add both click and touch support
function addClickAndTouchHandler(element, handler) {
    let touchHandled = false

    element.addEventListener('touchend', (e) => {
        e.preventDefault()
        e.stopPropagation()
        touchHandled = true
        handler(e)
        setTimeout(() => { touchHandled = false }, 300)
    }, { passive: false })

    element.addEventListener('click', (e) => {
        if (!touchHandled) {
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

    if (!relayId || !relayName) {
        alert('Please fill all fields')
        return
    }

    if (relays[relayId]) {
        alert('Relay ID already exists')
        return
    }

    const result = await postForm('/lights/add_relay', {
        relay_id: relayId,
        relay_name: relayName
    })

    if (result !== null) {
        const outputs = {}
        for (let i = 1; i <= 8; i++) {
            outputs[i] = `Output ${i}`
        }

        const numRelays = Object.keys(relays).length
        const x = 1200 + (numRelays % 3) * 350
        const y = 100 + Math.floor(numRelays / 3) * 450
        createRelay(relayId, relayName, outputs, x, y)
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
        const element = document.getElementById(`relay-${relayId}`)
        if (element) {
            jsPlumbInstance.removeAllEndpoints(element)
            element.remove()
        }

        delete relays[relayId]
        delete connectionLookupMap[`relay-${relayId}`]

        if (highlightedDevice === `relay-${relayId}`) {
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
    let currentName = outputs[outputId] || outputs[outputId] || `Output ${outputId.charCodeAt(0) - 96}`
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
            relays[currentEditTarget.relayId].outputs[currentEditTarget.outputId] = newName
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
            const nameElement = document.querySelector(`#switch-${currentEditTarget.id} .device-name`)
            if (nameElement) nameElement.textContent = newName
        }

        document.getElementById('editButtonNumber').style.display = 'none'
    } else if (currentEditTarget.type === 'relay') {
        // Rename relay
        const result = await postForm('/lights/rename_relay', {
            relay_id: currentEditTarget.id,
            relay_name: newName
        })

        if (result !== null) {
            relays[currentEditTarget.id].name = newName
            const nameElement = document.querySelector(`#relay-${currentEditTarget.id} .device-name`)
            if (nameElement) nameElement.textContent = newName
        }
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