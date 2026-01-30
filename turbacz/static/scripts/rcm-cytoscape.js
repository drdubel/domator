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
        elements.push({
            group: 'nodes',
            data: {
                id: `switch-${switchId}`,
                label: switchData.name,
                type: 'switch',
                deviceId: switchId,
                color: switchData.color || '#6366f1',
                statusColor: statusColor
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
        elements.push({
            group: 'nodes',
            data: {
                id: `relay-${relayId}`,
                label: relayData.name,
                type: 'relay',
                deviceId: relayId,
                statusColor: statusColor
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
        const connectionColor = switchData?.color || '#6366f1';

        for (const [buttonId, conns] of Object.entries(buttons)) {
            if (Array.isArray(conns)) {
                for (const conn of conns) {
                    elements.push({
                        group: 'edges',
                        data: {
                            id: `edge-${switchId}-${buttonId}-${conn.relayId}-${conn.outputId}`,
                            source: `switch-${switchId}-btn-${buttonId}`,
                            target: `relay-${conn.relayId}-output-${conn.outputId}`,
                            color: connectionColor,
                            switchId: switchId,
                            buttonId: buttonId,
                            relayId: conn.relayId,
                            outputId: conn.outputId
                        }
                    });
                }
            }
        }
    }

    return elements;
}

// Load configuration from API
async function loadConfiguration() {
    showLoading();

    try {
        const switchesData = await fetchAPI('/lights/switches');
        const relaysData = await fetchAPI('/lights/relays');
        const connectionsData = await fetchAPI('/lights/connections');
        const lightsData = await fetchAPI('/lights/get_lights');

        if (switchesData) switches = switchesData;
        if (relaysData) relays = relaysData;
        if (connectionsData) connections = connectionsData;
        if (lightsData) lights = lightsData;

        console.log('Loaded:', { switches, relays, connections, lights });

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

    // Calculate and set proper heights for container nodes
    cy.nodes('[type="switch"]').forEach(node => {
        const switchId = node.data('deviceId');
        const buttonCount = mockSwitches[switchId]?.buttonCount || 0;
        const height = 80 + (buttonCount * 30) + 20;
        node.data('height', height);
    });

    cy.nodes('[type="relay"]').forEach(node => {
        const relayId = node.data('deviceId');
        const outputCount = Object.keys(mockRelays[relayId]?.outputs || {}).length;
        const height = 80 + (outputCount * 30) + 20;
        node.data('height', height);
    });

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
