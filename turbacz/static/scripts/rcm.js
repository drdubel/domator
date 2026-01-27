// Global variables
let jsPlumbInstance
let switches = {}
let relays = {}
let online_relays = new Set()
let online_switches = new Set()
let connections = {}
var lights = {}
var pendingClicks = new Set()
let currentEditTarget = null
let highlightedDevice = null
let isLoadingConnections = false

// Zoom and Pan
let zoomLevel = 0.7
let panX = -300
let panY = -900
let isPanning = false
let startX = 0
let startY = 0

const API_BASE_URL = `https://${window.location.host}`

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

        console.log('Online relays:', online_relays)
        console.log('Online switches:', online_switches)

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

function resetZoom() {
    zoomLevel = 0.7
    panX = -300
    panY = -900
    updateZoom()
}

function updateZoom() {
    const canvas = document.getElementById('canvas')
    document.getElementById('zoomLevel').innerText = `${Math.round(zoomLevel * 100)}%`

    canvas.style.transform = `translate(${panX}px, ${panY}px) scale(${zoomLevel})`

    if (jsPlumbInstance) {
        jsPlumbInstance.setZoom(zoomLevel); // ← critical
        jsPlumbInstance.repaintEverything()
    }
}

function zoomIn() {
    zoomAtPoint(1.1, window.innerWidth / 2, window.innerHeight / 2)
}

function zoomOut() {
    zoomAtPoint(0.9, window.innerWidth / 2, window.innerHeight / 2)
}

function zoomAtPoint(factor, centerX, centerY) {
    const prevScale = zoomLevel
    zoomLevel *= factor

    // clamp zoom
    zoomLevel = Math.min(Math.max(zoomLevel, 0.2), 3)

    // adjust pan so zoom is centered
    panX = centerX - (centerX - panX) * (zoomLevel / prevScale)
    panY = centerY - (centerY - panY) * (zoomLevel / prevScale)
    updateZoom()
}


// Pan functions
function initPanning() {
    const wrapper = document.getElementById('canvas-wrapper')
    const canvas = document.getElementById('canvas')

    wrapper.addEventListener('mousedown', (e) => {
        // Only pan if clicking on wrapper/canvas background (not on devices)
        if (e.target === wrapper || e.target === canvas) {
            isPanning = true
            startX = e.clientX - panX * zoomLevel
            startY = e.clientY - panY * zoomLevel
            wrapper.classList.add('grabbing')
        }
    })

    document.addEventListener('mousemove', (e) => {
        if (isPanning) {
            panX = (e.clientX - startX) / zoomLevel
            panY = (e.clientY - startY) / zoomLevel
            updateZoom()
        }
    })

    document.addEventListener('mouseup', () => {
        isPanning = false
        wrapper.classList.remove('grabbing')
    })

    // Zoom with mouse wheel
    wrapper.addEventListener('wheel', (e) => {
        e.preventDefault()

        const rect = wrapper.getBoundingClientRect()
        const centerX = rect.width / 2
        const centerY = rect.height / 2

        const zoomFactor = e.deltaY < 0 ? 1.1 : 0.9
        zoomAtPoint(zoomFactor, centerX, centerY)
    }, { passive: false })
    resetZoom()
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

    const allConnections = jsPlumbInstance.getAllConnections()

    allConnections.forEach(conn => {
        const sourceId = conn.sourceId
        const targetId = conn.targetId

        if (sourceId.startsWith(deviceId) || targetId.startsWith(deviceId)) {
            conn.setPaintStyle({ stroke: '#ef4444', strokeWidth: 5 })

            if (sourceId.startsWith(deviceId)) {
                const targetMatch = targetId.match(/^(relay-\d+|switch-\d+)/)
                if (targetMatch) {
                    const targetElement = document.getElementById(targetMatch[1])
                    if (targetElement) targetElement.classList.add('highlighted')
                }
            } else {
                const sourceMatch = sourceId.match(/^(relay-\d+|switch-\d+)/)
                if (sourceMatch) {
                    const sourceElement = document.getElementById(sourceMatch[1])
                    if (sourceElement) sourceElement.classList.add('highlighted')
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

    const allConnections = jsPlumbInstance.getAllConnections()
    allConnections.forEach(conn => {
        conn.setPaintStyle({ stroke: '#6366f1', strokeWidth: 3 })
    })

    highlightedDevice = null
}

function highlightButton(switchId, buttonId) {
    const buttonElement = document.getElementById(`switch-${switchId}-btn-${buttonId}`)
    if (!buttonElement) {
        console.warn(`Button not found: switch-${switchId}-btn-${buttonId}`)
        return
    }

    // Set initial red background
    buttonElement.style.background = 'rgba(255, 0, 0, 1)'
    buttonElement.style.transition = 'none'

    // Force reflow to ensure the transition works
    buttonElement.offsetHeight

    // Start fading to normal blue gradient color over 5 seconds
    // Using ease-in: starts slow, ends fast
    requestAnimationFrame(() => {
        buttonElement.style.transition = 'background 5s ease-in'
        buttonElement.style.background = 'linear-gradient(135deg, rgba(99, 102, 241, 0.2) 0%, rgba(79, 70, 229, 0.2) 100%)'
    })

    console.log('Highlighted button:', switchId, buttonId)
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

function initJsPlumb() {
    jsPlumb.ready(function () {
        jsPlumbInstance = jsPlumb.getInstance({
            Container: "canvas",
            Connector: ["Bezier", { curviness: 50 }],
            PaintStyle: {
                stroke: '#6366f1',
                strokeWidth: 3
            },
            HoverPaintStyle: {
                stroke: '#818cf8',
                strokeWidth: 4
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

        bindJsPlumbEvents()
        initPanning()

        document.getElementById('canvas').addEventListener('click', function (e) {
            if (e.target.id === 'canvas') {
                clearHighlights()
            }
        })

        loadConfiguration()
    })
}

async function loadConfiguration() {
    showLoading()
    isLoadingConnections = true

    // Remove all connections & endpoints
    jsPlumbInstance.deleteEveryConnection()
    jsPlumbInstance.deleteEveryEndpoint()

    // Clear canvas
    const canvas = document.getElementById('canvas')
    canvas.innerHTML = ''

    switches = {}
    relays = {}
    connections = {}

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

        // create relays
        let relayX = 2500, relayY = 1500
        for (let [relayId, relayName] of Object.entries(relays_config)) {
            createRelay(parseInt(relayId), relayName, outputs_config[relayId] || {}, relayX, relayY)
            relayY += 770
            if (relayY > 4000) { relayY = 1500; relayX += 350; }
        }

        // create switches
        let switchX = 1000, switchY = 1500
        for (let [switchId, switchData] of Object.entries(switches_config)) {
            const [switchName, buttonCount] = switchData
            createSwitch(parseInt(switchId), switchName, buttonCount, switchX, switchY)
            switchY += buttonCount * 72 + 185
            if (switchY > 4000) { switchY = 1500; switchX += 500; }
        }

        // Create connections after elements exist
        setTimeout(() => {
            for (let [switchId, buttons] of Object.entries(connections_config)) {
                for (let [buttonId, targets] of Object.entries(buttons)) {
                    for (let [relayId, outputId] of targets) {
                        createConnection(parseInt(switchId), buttonId, parseInt(relayId), outputId)
                    }
                }
            }

            bindJsPlumbEvents()
            isLoadingConnections = false
            hideLoading()
        }, 100); // slightly smaller delay may work

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
    const switchDiv = document.createElement('div')
    switchDiv.id = `switch-${switchId}`
    switchDiv.className = 'device-box switch-box'
    switchDiv.style.left = `${x}px`
    switchDiv.style.top = `${y}px`

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
    const statusClass = isOnline ? 'status-online' : 'status-offline'
    const statusDot = `<span class="status-indicator ${statusClass}"></span>`

    switchDiv.innerHTML = `
                <div class="device-header">
                    <span>
                    ${statusDot}
                    <span class="device-id" onclick="event.stopPropagation(); copyIdToClipboard(${switchId}, this)">ID: ${switchId}</span>
                    </span>
                    <button class="delete-btn" onclick="event.stopPropagation(); deleteSwitch(${switchId})">✕</button>
                </div>
                <div class="device-name" onclick="event.stopPropagation(); editDeviceName('switch', ${switchId})">${switchName}</div>
                ${buttonsHTML}
            `

    document.getElementById('canvas').appendChild(switchDiv)

    switchDiv.addEventListener('click', function () {
        highlightDevice(`switch-${switchId}`)
    })

    jsPlumbInstance.draggable(switchDiv, {
        containment: false
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

    switches[switchId] = { name: switchName, buttonCount, element: switchDiv }
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
    const relayDiv = document.createElement('div')
    relayDiv.id = `relay-${relayId}`
    relayDiv.className = 'device-box relay-box'
    relayDiv.style.left = `${x}px`
    relayDiv.style.top = `${y}px`

    let outputsHTML = ''
    for (let i = 1; i <= 8; i++) {
        let outputName = outputs[String.fromCharCode(96 + i)] || `Output ${i}`
        if (Array.isArray(outputName)) {
            outputName = outputName[0]
        }
        outputsHTML += `
                    <div class="output-item" id="relay-${relayId}-output-${String.fromCharCode(96 + i)}">
                        <span><img class="item-icon" src="/static/data/img/off.png" alt="switch" onclick="changeSwitchState(${relayId}, '${String.fromCharCode(96 + i)}')"></span>
                        <span class="output-name" onclick="event.stopPropagation(); editOutputName(${relayId}, '${String.fromCharCode(96 + i)}')">${outputName}</span>
                    </div>
                `
    }

    const isOnline = online_relays.has(relayId)
    const statusClass = isOnline ? 'status-online' : 'status-offline'
    const statusDot = `<span class="status-indicator ${statusClass}"></span>`

    relayDiv.innerHTML = `
                <div class="device-header">
                    <span>
                    ${statusDot}
                    <span class="device-id" onclick="event.stopPropagation(); copyIdToClipboard(${relayId}, this)">ID: ${relayId}</span>
                    </span>
                    <button class="delete-btn" onclick="event.stopPropagation(); deleteRelay(${relayId})">✕</button>
                </div>
                <div class="device-name" onclick="event.stopPropagation(); editDeviceName('relay', ${relayId})">${relayName}</div>
                ${outputsHTML}
            `

    document.getElementById('canvas').appendChild(relayDiv)

    relayDiv.addEventListener('click', function () {
        highlightDevice(`relay-${relayId}`)
    })

    jsPlumbInstance.draggable(relayDiv, {
        containment: false
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
                indicator.className = isOnline ? 'status-indicator status-online' : 'status-indicator status-offline'
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
                indicator.className = isOnline ? 'status-indicator status-online' : 'status-indicator status-offline'
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

    const conn = jsPlumbInstance.connect({
        source: sourceElement,
        target: targetElement,
        paintStyle: { stroke: '#6366f1', strokeWidth: 3 },
        endpoint: ["Dot", { radius: 8 }]
    })

    if (conn) {
        if (!connections[switchId]) connections[switchId] = {}
        if (!connections[switchId][buttonId]) connections[switchId][buttonId] = []
        connections[switchId][buttonId].push({ relayId, outputId, connection: conn })
    }
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
    var element = document.getElementById(`relay-${relay_id}-output-${output_id}`).querySelector('img')
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

    jsPlumbInstance.deleteEveryConnection()
    connections = {}

    hideLoading()
    console.log('All connections cleared')
}

window.addEventListener('load', initJsPlumb)
