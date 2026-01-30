// RCM Cytoscape.js Implementation - Full Integration
// Canvas-based rendering with real data and WebSocket support

let cy = null;
let switches = {};
let relays = {};
let connections = {};
let online_relays = new Set();
let online_switches = new Set();
let up_to_date_devices = {};
let lights = {};
let pendingClicks = new Set();
let highlightedDevice = null;

const API_BASE_URL = `https://${window.location.host}`;

// Initialize WebSocket manager
var wsManager = new WebSocketManager('/rcm/ws/', function (event) {
    var msg = JSON.parse(event.data);
    console.log(msg);

    if (msg.type == "update") {
        loadConfiguration();
    }

    if (msg.type == "light_state") {
        pendingClicks.delete(`${msg.relay_id}-${msg.output_id}`);
        lights[`${msg.relay_id}-${msg.output_id}`] = msg.state;
        updateLightUI(msg.relay_id, msg.output_id, msg.state);
    }

    if (msg.type == "online_status") {
        online_relays = new Set(msg.online_relays);
        online_switches = new Set(msg.online_switches);
        up_to_date_devices = msg.up_to_date_devices || {};

        console.log('Online relays:', online_relays);
        console.log('Online switches:', online_switches);
        console.log('Up to date devices:', up_to_date_devices);

        updateOnlineStatus();
    }

    if (msg.type == "switch_state" && msg.switch_id && msg.button_id) {
        highlightButton(msg.switch_id, msg.button_id);

        // Auto-clear highlight after 5 seconds
        setTimeout(() => {
            clearButtonHighlight(msg.switch_id, msg.button_id);
        }, 5000);
    }
});

// Connect WebSocket
wsManager.connect();
wsManager.startConnectionCheck();

// API Functions
async function fetchAPI(endpoint) {
    try {
        const url = `${API_BASE_URL}${endpoint}`;
        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

        const text = await response.text();
        if (!text) return null;

        try {
            return JSON.parse(text);
        } catch (jsonError) {
            console.error('JSON parse error for', endpoint, ':', text.substring(0, 200));
            return null;
        }
    } catch (error) {
        console.error('Fetch error:', error);
        return null;
    }
}

async function postForm(endpoint, data) {
    try {
        const formData = new FormData();
        Object.keys(data).forEach(key => formData.append(key, data[key]));

        const url = `${API_BASE_URL}${endpoint}`;
        const response = await fetch(url, {
            method: 'POST',
            body: formData
        });

        if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
        wsManager.send(JSON.stringify({ "type": "update" }));
        return { success: true };
    } catch (error) {
        console.error('Post error:', error);
        alert('Error: ' + error.message);
        return null;
    }
}

// Convert data to Cytoscape format
function generateCytoscapeElements() {
    const elements = [];

    // Add switch nodes
    for (const [switchId, switchData] of Object.entries(switches)) {
        const isOnline = online_switches.has(parseInt(switchId));
        const isUpToDate = up_to_date_devices[parseInt(switchId)];
        let statusColor = '#ef4444'; // red offline
        if (isOnline) {
            statusColor = isUpToDate ? '#10b981' : '#f59e0b'; // green or orange
        }

        // Main switch container node
        const switchHeight = 80 + (switchData.buttonCount * 45) + 20;
        elements.push({
            group: 'nodes',
            data: {
                id: `switch-${switchId}`,
                label: switchData.name,
                type: 'switch',
                deviceId: switchId,
                color: switchData.color || '#6366f1',
                statusColor: statusColor,
                height: switchHeight
            },
            position: { x: switchData.x, y: switchData.y },
            classes: 'switch-container'
        });

        // Button nodes (for connection endpoints)
        for (let i = 0; i < switchData.buttonCount; i++) {
            const buttonId = String.fromCharCode(97 + i); // a, b, c, d
            elements.push({
                group: 'nodes',
                data: {
                    id: `switch-${switchId}-btn-${buttonId}`,
                    label: `Btn ${i + 1}`,
                    type: 'button',
                    parent: `switch-${switchId}`,
                    buttonId: buttonId,
                    switchId: switchId
                },
                position: { x: switchData.x, y: switchData.y + 60 + (i * 45) }
            });
        }
    }

    // Add relay nodes
    for (const [relayId, relayData] of Object.entries(relays)) {
        const isOnline = online_relays.has(parseInt(relayId));
        const isUpToDate = up_to_date_devices[parseInt(relayId)];
        let statusColor = '#ef4444'; // red offline
        if (isOnline) {
            statusColor = isUpToDate ? '#10b981' : '#f59e0b'; // green or orange
        }

        // Main relay container node
        const outputCount = Object.keys(relayData.outputs || {}).length;
        const relayHeight = 80 + (outputCount * 45) + 20;
        elements.push({
            group: 'nodes',
            data: {
                id: `relay-${relayId}`,
                label: relayData.name,
                type: 'relay',
                deviceId: relayId,
                statusColor: statusColor,
                height: relayHeight
            },
            position: { x: relayData.x, y: relayData.y },
            classes: 'relay-container'
        });

        // Output nodes (for connection endpoints)
        let outputIndex = 0;
        for (const [outputId, outputName] of Object.entries(relayData.outputs || {})) {
            const lightState = lights[`${relayId}-${outputId}`] || 0;
            elements.push({
                group: 'nodes',
                data: {
                    id: `relay-${relayId}-output-${outputId}`,
                    label: outputName,
                    type: 'output',
                    parent: `relay-${relayId}`,
                    outputId: outputId,
                    relayId: relayId,
                    lightState: lightState
                },
                position: { x: relayData.x, y: relayData.y + 60 + (outputIndex * 45) }
            });
            outputIndex++;
        }
    }

    // Add connection edges
    for (const [switchId, buttons] of Object.entries(connections)) {
        const switchData = switches[switchId];
        if (!switchData) {
            console.warn(`Connection references nonexistent switch ${switchId}`);
            continue;
        }

        const connectionColor = switchData.color || '#6366f1';

        for (const [buttonId, targets] of Object.entries(buttons)) {
            if (Array.isArray(targets)) {
                for (const [relayId, outputId] of targets) {
                    // Validate connection
                    if (!relayId || !outputId) {
                        console.warn(`Invalid connection: switch ${switchId} button ${buttonId} -> relay ${relayId} output ${outputId}`);
                        continue;
                    }

                    // Check if target relay and output exist
                    const targetRelay = relays[relayId];
                    if (!targetRelay) {
                        console.warn(`Connection target relay does not exist: ${relayId}. Available relays:`, Object.keys(relays));
                        continue;
                    }
                    if (!targetRelay.outputs || !targetRelay.outputs[outputId]) {
                        console.warn(`Connection output does not exist: relay ${relayId} output ${outputId}. Available outputs:`, Object.keys(targetRelay.outputs || {}));
                        continue;
                    }

                    elements.push({
                        group: 'edges',
                        data: {
                            id: `edge-${switchId}-${buttonId}-${relayId}-${outputId}`,
                            source: `switch-${switchId}-btn-${buttonId}`,
                            target: `relay-${relayId}-output-${outputId}`,
                            color: connectionColor,
                            switchId: switchId,
                            buttonId: buttonId,
                            relayId: relayId,
                            outputId: outputId
                        }
                    });
                }
            }
        }
    }

    return elements;
}

// Position management
function loadDevicePositions() {
    const saved = localStorage.getItem('rcm_device_positions');
    return saved ? JSON.parse(saved) : {};
}

function getSavedPosition(deviceId, defaultX, defaultY) {
    const positions = loadDevicePositions();
    return positions[deviceId] || { x: defaultX, y: defaultY };
}

function saveDevicePositions() {
    const positions = {};

    // Save switch positions
    for (const switchId of Object.keys(switches)) {
        const node = cy.$(`#switch-${switchId}`);
        if (node.length) {
            positions[`switch-${switchId}`] = {
                x: Math.round(node.position('x')),
                y: Math.round(node.position('y'))
            };
        }
    }

    // Save relay positions
    for (const relayId of Object.keys(relays)) {
        const node = cy.$(`#relay-${relayId}`);
        if (node.length) {
            positions[`relay-${relayId}`] = {
                x: Math.round(node.position('x')),
                y: Math.round(node.position('y'))
            };
        }
    }

    localStorage.setItem('rcm_device_positions', JSON.stringify(positions));
}

function loadDeviceColors() {
    const saved = localStorage.getItem('rcm_device_colors');
    return saved ? JSON.parse(saved) : {};
}

function getSavedColor(deviceId) {
    const colors = loadDeviceColors();
    return colors[deviceId];
}

// Load configuration from API
async function loadConfiguration() {
    showLoading();

    try {
        const [relaysData, outputsData, switchesData, connectionsData] = await Promise.all([
            fetchAPI('/lights/get_relays'),
            fetchAPI('/lights/get_outputs'),
            fetchAPI('/lights/get_switches'),
            fetchAPI('/lights/get_connections')
        ]);

        // Convert API format to internal format
        // Relays: API returns { relayId: relayName } -> convert to { relayId: { name, outputs, x, y } }
        if (relaysData) {
            relays = {};
            let relayY = 25000;
            for (const [relayId, relayName] of Object.entries(relaysData)) {
                const savedPos = getSavedPosition(`relay-${relayId}`, 25000, relayY);
                relays[relayId] = {
                    name: relayName,
                    outputs: outputsData?.[relayId] || {},
                    x: savedPos.x,
                    y: savedPos.y
                };
                relayY += 400;
            }
        }

        // Switches: API returns { switchId: [switchName, buttonCount] } -> convert to { switchId: { name, buttonCount, color, x, y } }
        if (switchesData) {
            switches = {};
            let switchY = 25000;
            for (const [switchId, switchData] of Object.entries(switchesData)) {
                const [switchName, buttonCount] = switchData;
                const savedPos = getSavedPosition(`switch-${switchId}`, 23500, switchY);
                const savedColor = getSavedColor(`switch-${switchId}`);
                switches[switchId] = {
                    name: switchName,
                    buttonCount: buttonCount,
                    color: savedColor || '#6366f1',
                    x: savedPos.x,
                    y: savedPos.y
                };
                switchY += (buttonCount * 72 + 185);
            }
        }

        if (connectionsData) connections = connectionsData;

        console.log('Loaded:', { switches, relays, connections });

        // Rebuild Cytoscape graph
        if (cy) {
            cy.destroy();
        }
        initCytoscape('cy-container');

    } catch (error) {
        console.error('Error loading configuration:', error);
    }

    hideLoading();
}

function showLoading() {
    const loading = document.getElementById('loading');
    if (loading) loading.classList.add('active');
}

function hideLoading() {
    const loading = document.getElementById('loading');
    if (loading) loading.classList.remove('active');
}

function updateOnlineStatus() {
    if (!cy) return;

    // Update switch status colors
    cy.nodes('[type="switch"]').forEach(node => {
        const switchId = parseInt(node.data('deviceId'));
        const isOnline = online_switches.has(switchId);
        const isUpToDate = up_to_date_devices[switchId];

        let statusColor = '#ef4444'; // red offline
        if (isOnline) {
            statusColor = isUpToDate ? '#10b981' : '#f59e0b'; // green or orange
        }

        node.data('statusColor', statusColor);
    });

    // Update relay status colors
    cy.nodes('[type="relay"]').forEach(node => {
        const relayId = parseInt(node.data('deviceId'));
        const isOnline = online_relays.has(relayId);
        const isUpToDate = up_to_date_devices[relayId];

        let statusColor = '#ef4444'; // red offline
        if (isOnline) {
            statusColor = isUpToDate ? '#10b981' : '#f59e0b'; // green or orange
        }

        node.data('statusColor', statusColor);
    });
}

function updateLightUI(relay_id, output_id, state) {
    if (!cy) return;

    const outputNode = cy.getElementById(`relay-${relay_id}-output-${output_id}`);
    if (outputNode.length) {
        outputNode.data('lightState', state);
    }
}

function highlightButton(switchId, buttonId) {
    // TODO: Implement button highlighting
    console.log('Highlight button:', switchId, buttonId);
}

function clearButtonHighlight(switchId, buttonId) {
    // TODO: Implement clear button highlight
    console.log('Clear highlight:', switchId, buttonId);
}

// Toolbar functions
async function addSwitch() {
    const switchId = parseInt(document.getElementById('new-switch-id').value);
    const switchName = document.getElementById('new-switch-name').value;
    const buttonCount = parseInt(document.getElementById('new-switch-buttons').value);

    if (!switchId || !switchName || !buttonCount) {
        alert('Please fill all fields');
        return;
    }

    const result = await postForm('/lights/add_switch', {
        switch_id: switchId,
        switch_name: switchName,
        button_count: buttonCount
    });

    if (result) {
        closeModal('addSwitchModal');
    }
}

async function addRelay() {
    const relayId = parseInt(document.getElementById('new-relay-id').value);
    const relayName = document.getElementById('new-relay-name').value;

    if (!relayId || !relayName) {
        alert('Please fill all fields');
        return;
    }

    const result = await postForm('/lights/add_relay', {
        relay_id: relayId,
        relay_name: relayName
    });

    if (result) {
        closeModal('addRelayModal');
    }
}

function showAddSwitchModal() {
    document.getElementById('addSwitchModal').classList.add('active');
}

function showAddRelayModal() {
    document.getElementById('addRelayModal').classList.add('active');
}

function closeModal(modalId) {
    document.getElementById(modalId).classList.remove('active');
}

function updateAllSwitches() {
    if (!wsManager.isConnected()) {
        alert('Not connected to server');
        return;
    }
    wsManager.send(JSON.stringify({ type: 'update_all_switches' }));
}

function updateAllRelays() {
    if (!wsManager.isConnected()) {
        alert('Not connected to server');
        return;
    }
    wsManager.send(JSON.stringify({ type: 'update_all_relays' }));
}

function updateRoot() {
    if (!wsManager.isConnected()) {
        alert('Not connected to server');
        return;
    }
    wsManager.send(JSON.stringify({ type: 'update_root' }));
}

// Make overlay draggable
function makeDraggable(overlay, node) {
    let isDragging = false;
    let startX = 0;
    let startY = 0;
    let startNodeX = 0;
    let startNodeY = 0;

    overlay.addEventListener('mousedown', (e) => {
        // Only drag if clicking on the card itself, not buttons
        if (e.target.tagName === 'BUTTON' || e.target.closest('button')) return;
        if (e.target.classList.contains('button-item') || e.target.classList.contains('output-item')) return;

        isDragging = true;
        startX = e.clientX;
        startY = e.clientY;
        const nodePos = node.position();
        startNodeX = nodePos.x;
        startNodeY = nodePos.y;

        overlay.style.cursor = 'grabbing';
        e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
        if (!isDragging) return;

        const zoom = cy.zoom();
        const pan = cy.pan();

        // Calculate movement in canvas coordinates
        const dx = (e.clientX - startX) / zoom;
        const dy = (e.clientY - startY) / zoom;

        // Update node position
        node.position({
            x: startNodeX + dx,
            y: startNodeY + dy
        });

        // Update overlay position
        updateOverlayPosition(node, overlay);
    });

    document.addEventListener('mouseup', () => {
        if (isDragging) {
            isDragging = false;
            overlay.style.cursor = 'move';
            saveDevicePositions();
        }
    });
}

// Create HTML device card overlays
function createDeviceOverlays() {
    const container = document.getElementById('cy-container');

    // Remove existing overlays
    const existingOverlays = container.querySelectorAll('.device-overlay');
    existingOverlays.forEach(el => el.remove());

    // Create switch overlays
    cy.nodes('[type="switch"]').forEach(node => {
        const switchId = node.data('deviceId');
        const switchData = switches[switchId];
        if (!switchData) return;

        const isOnline = online_switches.has(parseInt(switchId));
        const isUpToDate = up_to_date_devices[parseInt(switchId)];
        let statusClass = !isOnline ? 'status-offline' : (isUpToDate ? 'status-online' : 'status-outdated');

        let buttonsHTML = '';
        for (let i = 0; i < switchData.buttonCount; i++) {
            const buttonId = String.fromCharCode(97 + i);
            buttonsHTML += `
                <div class="button-item" id="switch-${switchId}-btn-${buttonId}-label">
                    <span class="button-name">Button ${i + 1}</span>
                    <span class="item-icon">🔘</span>
                </div>
            `;
        }

        const overlay = document.createElement('div');
        overlay.className = 'device-overlay switch-overlay';
        overlay.id = `overlay-switch-${switchId}`;
        overlay.innerHTML = `
            <div class="device-card switch-card" style="border-left-color: ${switchData.color || '#6366f1'}">
                <div class="device-header">
                    <span>
                        <span class="status-indicator ${statusClass}"></span>
                        <span class="device-id">ID: ${switchId}</span>
                    </span>
                    <div class="device-buttons">
                        <button class="hide-btn" title="Hide Device">👁️</button>
                        <button class="update-btn update-btn-switch" title="Update Device">⟳</button>
                        <button class="color-btn" title="Change Color">🎨</button>
                        <button class="delete-btn">✕</button>
                    </div>
                </div>
                <div class="device-name">${switchData.name}</div>
                ${buttonsHTML}
            </div>
        `;

        container.appendChild(overlay);
        updateOverlayPosition(node, overlay);

        // Make overlay draggable
        makeDraggable(overlay, node);
    });

    // Create relay overlays
    cy.nodes('[type="relay"]').forEach(node => {
        const relayId = node.data('deviceId');
        const relayData = relays[relayId];
        if (!relayData) return;

        const isOnline = online_relays.has(parseInt(relayId));
        const isUpToDate = up_to_date_devices[parseInt(relayId)];
        let statusClass = !isOnline ? 'status-offline' : (isUpToDate ? 'status-online' : 'status-outdated');

        let outputsHTML = '';
        for (const [outputId, outputName] of Object.entries(relayData.outputs || {})) {
            // Handle array outputName (take first element)
            let displayName = outputName;
            if (Array.isArray(outputName)) {
                displayName = outputName[0];
            }
            const lightState = lights[`${relayId}-${outputId}`] || 0;
            const imgSrc = lightState ? '/static/data/img/on.png' : '/static/data/img/off.png';
            outputsHTML += `
                <div class="output-item" id="relay-${relayId}-output-${outputId}-label">
                    <span><img class="item-icon" src="${imgSrc}" alt="light"></span>
                    <span class="output-name">${displayName}</span>
                </div>
            `;
        }

        const overlay = document.createElement('div');
        overlay.className = 'device-overlay relay-overlay';
        overlay.id = `overlay-relay-${relayId}`;
        overlay.innerHTML = `
            <div class="device-card relay-card">
                <div class="device-header">
                    <span>
                        <span class="status-indicator ${statusClass}"></span>
                        <span class="device-id">ID: ${relayId}</span>
                    </span>
                    <div class="device-buttons">
                        <button class="hide-btn" title="Hide Device">👁️</button>
                        <button class="update-btn update-btn-relay" title="Update Device">⟳</button>
                        <button class="delete-btn">✕</button>
                    </div>
                </div>
                <div class="device-name">${relayData.name}</div>
                ${outputsHTML}
            </div>
        `;

        container.appendChild(overlay);
        updateOverlayPosition(node, overlay);

        // Make overlay draggable
        makeDraggable(overlay, node);
    });

    // Enable dragging on container nodes
    cy.nodes('[type="switch"], [type="relay"]').forEach(node => {
        node.grabify();
    });

    // Update overlay positions when nodes are dragged
    cy.on('drag', 'node[type="switch"], node[type="relay"]', function (event) {
        const node = event.target;
        const deviceId = node.data('deviceId');
        const type = node.data('type');
        const overlay = document.getElementById(`overlay-${type}-${deviceId}`);
        if (overlay) {
            updateOverlayPosition(node, overlay);
        }
    });

    // Save positions after drag ends
    cy.on('dragfree', 'node[type="switch"], node[type="relay"]', debounce(saveDevicePositions, 300));

    // Update positions on pan/zoom - no debounce for smooth movement
    cy.on('pan zoom', () => {
        cy.nodes('[type="switch"], [type="relay"]').forEach(node => {
            const deviceId = node.data('deviceId');
            const type = node.data('type');
            const overlay = document.getElementById(`overlay-${type}-${deviceId}`);
            if (overlay) {
                updateOverlayPosition(node, overlay);
            }
        });
    });
}

function updateOverlayPosition(node, overlay) {
    const pos = node.renderedPosition();
    const zoom = cy.zoom();
    const width = 280;
    const height = node.data('height');

    // Scale inversely with zoom - cards get smaller as you zoom out
    const scale = 1 / zoom;
    const scaledWidth = width * scale;
    const scaledHeight = height * scale;

    overlay.style.left = `${pos.x - scaledWidth / 2}px`;
    overlay.style.top = `${pos.y - scaledHeight / 2}px`;
    overlay.style.width = `${width}px`;
    overlay.style.transform = `scale(${scale})`;
    overlay.style.transformOrigin = 'center center';
}

// Initialize Cytoscape
function initCytoscape(containerId) {
    cy = cytoscape({
        container: document.getElementById(containerId),

        elements: generateCytoscapeElements(),

        style: [
            // Switch container nodes
            {
                selector: '.switch-container',
                style: {
                    'width': 280,
                    'height': 'data(height)',
                    'shape': 'roundrectangle',
                    'background-color': '#1e293b',
                    'border-width': 4,
                    'border-color': 'data(color)',
                    'label': 'data(label)',
                    'text-valign': 'top',
                    'text-halign': 'center',
                    'text-margin-y': -130,
                    'font-size': 22,
                    'font-weight': 800,
                    'color': '#f1f5f9',
                    'text-wrap': 'wrap',
                    'text-max-width': 250
                }
            },

            // Relay container nodes
            {
                selector: '.relay-container',
                style: {
                    'width': 280,
                    'height': 'data(height)',
                    'shape': 'roundrectangle',
                    'background-color': '#1e293b',
                    'border-width': 4,
                    'border-color': '#f59e0b',
                    'label': 'data(label)',
                    'text-valign': 'top',
                    'text-halign': 'center',
                    'text-margin-y': -130,
                    'font-size': 22,
                    'font-weight': 800,
                    'color': '#f1f5f9',
                    'text-wrap': 'wrap',
                    'text-max-width': 250
                }
            },

            // Button nodes (invisible, only for connections)
            {
                selector: 'node[type="button"]',
                style: {
                    'width': 10,
                    'height': 10,
                    'background-color': '#6366f1',
                    'border-width': 2,
                    'border-color': '#4f46e5',
                    'label': ''
                }
            },

            // Output nodes (invisible, only for connections)
            {
                selector: 'node[type="output"]',
                style: {
                    'width': 10,
                    'height': 10,
                    'background-color': '#f59e0b',
                    'border-width': 2,
                    'border-color': '#d97706',
                    'label': ''
                }
            },

            // Connection edges
            {
                selector: 'edge',
                style: {
                    'width': 3,
                    'line-color': 'data(color)',
                    'target-arrow-color': 'data(color)',
                    'curve-style': 'bezier',
                    'control-point-step-size': 40
                }
            },

            // Edge hover effect
            {
                selector: 'edge:active',
                style: {
                    'width': 5,
                    'line-color': '#818cf8'
                }
            }
        ],

        layout: {
            name: 'preset' // Use predefined positions
        },

        // Performance optimizations
        renderer: {
            name: 'canvas'
        },

        wheelSensitivity: 0.3,

        minZoom: 0.2,
        maxZoom: 3,

        // Better rendering performance
        pixelRatio: 'auto',
        motionBlur: false,
        textureOnViewport: true,
        hideEdgesOnViewport: false,
        hideLabelsOnViewport: false
    });

    // Create HTML overlays for device cards
    createDeviceOverlays();

    // Enable smooth pan and zoom
    setupPanZoom();

    // Setup hover effects
    setupHoverEffects();

    console.log('Cytoscape initialized with', cy.nodes().length, 'nodes and', cy.edges().length, 'edges');
}

// Setup pan and zoom with smooth transitions
function setupPanZoom() {
    // Mouse wheel zoom
    cy.on('wheel', function (event) {
        event.preventDefault();
    });

    // Double-click to fit
    cy.on('dblclick', function (event) {
        if (event.target === cy) {
            cy.animate({
                fit: {
                    eles: cy.elements(),
                    padding: 50
                },
                duration: 300,
                easing: 'ease-out'
            });
        }
    });

    // Save viewport state on pan/zoom
    cy.on('viewport', debounce(function () {
        const zoom = cy.zoom();
        const pan = cy.pan();
        localStorage.setItem('rcm_cytoscape_viewport', JSON.stringify({
            zoom: zoom,
            pan: pan
        }));
    }, 300));

    // Restore viewport state
    const savedViewport = localStorage.getItem('rcm_cytoscape_viewport');
    if (savedViewport) {
        try {
            const viewport = JSON.parse(savedViewport);
            cy.viewport({
                zoom: viewport.zoom,
                pan: viewport.pan
            });
        } catch (e) {
            // Default view
            cy.fit(cy.elements(), 50);
        }
    } else {
        // Initial fit
        cy.fit(cy.elements(), 50);
    }
}

// Setup hover effects for edges
function setupHoverEffects() {
    let hoveredEdge = null;

    cy.on('mouseover', 'edge', function (event) {
        const edge = event.target;
        hoveredEdge = edge;

        const currentColor = edge.data('color');
        // Brighten color
        const brightColor = brightenColor(currentColor, 60);

        edge.style({
            'width': 5,
            'line-color': brightColor
        });
    });

    cy.on('mouseout', 'edge', function (event) {
        const edge = event.target;
        if (edge === hoveredEdge) {
            hoveredEdge = null;
        }

        edge.style({
            'width': 3,
            'line-color': edge.data('color')
        });
    });

    // Container node hover
    cy.on('mouseover', 'node[type="switch"], node[type="relay"]', function (event) {
        event.target.style({
            'border-width': 6
        });
    });

    cy.on('mouseout', 'node[type="switch"], node[type="relay"]', function (event) {
        event.target.style({
            'border-width': 4
        });
    });
}

// Utility: Brighten hex color
function brightenColor(hexColor, amount) {
    if (!hexColor.startsWith('#')) return hexColor;

    const r = parseInt(hexColor.slice(1, 3), 16);
    const g = parseInt(hexColor.slice(3, 5), 16);
    const b = parseInt(hexColor.slice(5, 7), 16);

    const hr = Math.min(255, r + amount);
    const hg = Math.min(255, g + amount);
    const hb = Math.min(255, b + amount);

    return `rgb(${hr}, ${hg}, ${hb})`;
}

// Utility: Debounce function
function debounce(func, delay) {
    let timeoutId;
    return function (...args) {
        clearTimeout(timeoutId);
        timeoutId = setTimeout(() => func.apply(this, args), delay);
    };
}

// Zoom controls
function zoomIn() {
    cy.animate({
        zoom: cy.zoom() * 1.2,
        duration: 200
    });
}

function zoomOut() {
    cy.animate({
        zoom: cy.zoom() * 0.8,
        duration: 200
    });
}

function resetZoom() {
    cy.animate({
        fit: {
            eles: cy.elements(),
            padding: 50
        },
        duration: 300
    });
}

// Export for global access
window.CytoscapeRCM = {
    init: initCytoscape,
    loadConfiguration: loadConfiguration,
    zoomIn: zoomIn,
    zoomOut: zoomOut,
    resetZoom: resetZoom,
    addSwitch: addSwitch,
    addRelay: addRelay,
    showAddSwitchModal: showAddSwitchModal,
    showAddRelayModal: showAddRelayModal,
    closeModal: closeModal,
    updateAllSwitches: updateAllSwitches,
    updateAllRelays: updateAllRelays,
    updateRoot: updateRoot,
    getInstance: () => cy
};

// Auto-initialize when DOM is ready
document.addEventListener('DOMContentLoaded', function () {
    console.log('Initializing Cytoscape RCM...');
    wsManager.connect();
    loadConfiguration();
});
