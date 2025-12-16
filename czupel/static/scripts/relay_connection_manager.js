// Global variables
let instance = null;
let switches = [];
let relays = [];
let switchCounter = 0;
let relayCounter = 0;
let deviceNames = {};
let outputNames = {}; // Store names for individual outputs

// Canvas control
let currentZoom = 1;
let panX = 0;
let panY = 0;
let isPanning = false;
let startX = 0;
let startY = 0;

// Auto-save
let saveTimeout = null;
const AUTOSAVE_DELAY = 1000; // Save 1 second after last change
const STORAGE_KEY = 'relay_switch_config';

// WebSocket
let ws = null;
let wsId = Math.floor(Math.random() * 2000000000);
let wsConnected = false;
let reconnectTimeout = null;


Sentry.init({
    dsn: "https://5f6bf7564d8d0462fb16de94af6096d7@o4506468887494656.ingest.us.sentry.io/4510545906499584",
    // Setting this option to true will send default PII data to Sentry.
    // For example, automatic IP address collection on events
    integrations: [Sentry.browserTracingIntegration()],
    tracesSampleRate: 1.0,
    sendDefaultPii: true,
    tracePropagationTargets: ["localhost", /^https:\/\/czupel\.dry\.pl\//],
});

// Wait for jsPlumb to load with better error handling
function waitForJsPlumb() {
    return new Promise((resolve, reject) => {
        let attempts = 0;
        const maxAttempts = 100; // Increased from 50

        const checkJsPlumb = setInterval(() => {
            attempts++;

            // Update loading message with dots
            const loadingMsg = document.getElementById('loadingMessage');
            if (loadingMsg && attempts % 5 === 0) {
                const dots = '.'.repeat((attempts / 5) % 4);
                loadingMsg.textContent = `Loading jsPlumb library${dots}`;
            }

            if (typeof jsPlumb !== 'undefined') {
                clearInterval(checkJsPlumb);
                console.log('✅ jsPlumb loaded successfully');
                resolve();
            } else if (attempts >= maxAttempts) {
                clearInterval(checkJsPlumb);
                reject(new Error('jsPlumb failed to load after ' + (maxAttempts / 10) + ' seconds'));
            }
        }, 100);
    });
}

// Initialize jsPlumb instance
function initializeJsPlumb() {
    if (!jsPlumb) {
        throw new Error('jsPlumb library not loaded');
    }

    if (instance) {
        instance.reset();
    }

    instance = jsPlumb.getInstance({
        Container: 'canvas',
        Connector: ['Bezier', { curviness: 70 }],
        DragOptions: { cursor: 'pointer', zIndex: 2000 },
        PaintStyle: { strokeWidth: 3, stroke: '#3b82f6' },
        EndpointStyle: { fill: '#3b82f6', radius: 8 },
        HoverPaintStyle: { strokeWidth: 4 },
        EndpointHoverStyle: { fill: '#2563eb', radius: 10 },
        ConnectionOverlays: [
            ['Arrow', {
                location: 1,
                width: 10,
                length: 10,
                foldback: 0.8
            }]
        ]
    });

    bindJsPlumbEvents();
    console.log('✅ jsPlumb initialized');
}

// Separate function to bind events (can be called multiple times safely)
function bindJsPlumbEvents() {
    if (!instance) return;

    // Unbind all first to prevent duplicates
    instance.unbind('beforeDrop');
    instance.unbind('connection');
    instance.unbind('connectionDetached');
    instance.unbind('drag');
    instance.unbind('dragstop');

    // Prevent duplicate connections
    instance.bind('beforeDrop', function (info) {
        const sourceId = info.sourceId;
        const targetId = info.targetId;

        const existing = instance.getAllConnections().find(conn =>
            conn.sourceId === sourceId && conn.targetId === targetId
        );

        if (existing) {
            console.log('❌ Duplicate prevented:', sourceId, '->', targetId);
            return false;
        }

        console.log('✅ Allowing connection:', sourceId, '->', targetId);
        return true;
    });

    // Connection created
    instance.bind('connection', function (info) {
        console.log('🔗 Connection event fired:', info.sourceId, '->', info.targetId);
        const color = document.getElementById('connectionColor').value;
        info.connection.setPaintStyle({ stroke: color, strokeWidth: 3 });

        updateConnectionMap();
        updateConnectionCount();
    });

    // Connection removed
    instance.bind('connectionDetached', function (info) {
        console.log('🗑️ Detached event fired:', info.sourceId, '->', info.targetId);

        updateConnectionMap();
        updateConnectionCount();
    });

    // Drag events
    instance.bind('drag', function (params) {
        // Repaint while dragging
    });

    instance.bind('dragstop', function (params) {
        console.log('✅ Drag stopped:', params.el.id);
    });

    console.log('✅ Events bound');
}

// Generate random ID
function generateId() {
    return Math.floor(Math.random() * 9000000000) + 1000000000;
}

// Zoom functions
function zoomIn() {
    currentZoom = Math.min(currentZoom * 1.2, 3);
    updateCanvasTransform();
}

function zoomOut() {
    currentZoom = Math.max(currentZoom / 1.2, 0.3);
    updateCanvasTransform();
}

function resetZoom() {
    currentZoom = 1;
    panX = 0;
    panY = 0;
    updateCanvasTransform();
}

function updateCanvasTransform() {
    const canvas = document.getElementById('canvas');
    canvas.style.transform = `translate(${panX}px, ${panY}px) scale(${currentZoom})`;
    document.getElementById('zoomDisplay').textContent = Math.round(currentZoom * 100) + '%';

    if (instance) {
        instance.setZoom(currentZoom);
    }
}

// Pan/zoom with mouse
document.addEventListener('DOMContentLoaded', () => {
    const container = document.getElementById('canvasContainer');

    container.addEventListener('mousedown', (e) => {
        if (e.target === container || e.target.id === 'canvas') {
            isPanning = true;
            startX = e.clientX - panX;
            startY = e.clientY - panY;
            container.classList.add('panning');
        }
    });

    document.addEventListener('mousemove', (e) => {
        if (isPanning) {
            panX = e.clientX - startX;
            panY = e.clientY - startY;
            updateCanvasTransform();
        }
    });

    document.addEventListener('mouseup', () => {
        isPanning = false;
        container.classList.remove('panning');
    });

    // Zoom with mouse wheel
    container.addEventListener('wheel', (e) => {
        e.preventDefault();
        const delta = e.deltaY > 0 ? 0.9 : 1.1;
        currentZoom = Math.max(0.3, Math.min(3, currentZoom * delta));
        updateCanvasTransform();
    });
});

// Editable functions
function makeEditable(element, getValue, setValue, placeholder = '') {
    element.addEventListener('click', function (e) {
        e.stopPropagation();
        const currentValue = getValue();
        const input = document.createElement('input');
        input.type = 'text';
        input.className = element.className.replace('device-', 'device-') + '-input';
        input.value = currentValue;
        input.placeholder = placeholder;

        element.replaceWith(input);
        input.focus();
        input.select();

        function save() {
            const newValue = input.value.trim() || currentValue;
            setValue(newValue);

            const newElement = element.cloneNode();
            newElement.textContent = newValue;
            input.replaceWith(newElement);

            makeEditable(newElement, getValue, setValue, placeholder);
        }

        input.addEventListener('blur', save);
        input.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') save();
        });
        input.addEventListener('click', (e) => e.stopPropagation());
    });
}

// Show add switch modal
function showAddSwitchModal() {
    switchCounter++;
    document.getElementById('switchName').value = `Switch ${switchCounter}`;
    document.getElementById('addSwitchModal').classList.add('active');
}

function closeAddSwitchModal() {
    document.getElementById('addSwitchModal').classList.remove('active');
}

function confirmAddSwitch() {
    const name = document.getElementById('switchName').value.trim() || `Switch ${switchCounter}`;
    const buttonCount = parseInt(document.getElementById('buttonCount').value);

    addSwitch(buttonCount, name);
    closeAddSwitchModal();
}

// Add switch
function addSwitch(buttonCount = 4, customName = null, customId = null, position = null) {
    if (!instance) return;
    console.log('🔵 addSwitch called:', { buttonCount, customName, position });
    console.trace();

    const switchId = customId || generateId();
    const switchName = customName || `Switch ${switchCounter}`;

    switches.push({ id: switchId, buttonCount, name: switchName });
    deviceNames[switchId] = switchName;

    const canvas = document.getElementById('canvas');
    const box = document.createElement('div');
    box.className = 'device-box switch-box';
    box.id = `switch-${switchId}`;

    if (position) {
        box.style.left = position.x + 'px';
        box.style.top = position.y + 'px';
    } else {
        box.style.left = (100 + switches.length * 50) + 'px';
        box.style.top = (100 + switches.length * 30) + 'px';
    }

    // Header
    const header = document.createElement('div');
    header.className = 'device-header';

    const idDiv = document.createElement('div');
    idDiv.className = 'device-id';
    idDiv.textContent = `ID: ${switchId}`;
    header.appendChild(idDiv);

    makeEditable(idDiv,
        () => switchId.toString(),
        (newId) => {
            if (!/^\d+$/.test(newId)) {
                alert('ID must contain only numbers!');
                return;
            }
            updateSwitchId(switchId, parseInt(newId), buttonCount);
        },
        'Device ID'
    );

    const deleteBtn = document.createElement('button');
    deleteBtn.className = 'btn btn-danger btn-sm';
    deleteBtn.innerHTML = '🗑️';
    deleteBtn.onclick = () => deleteSwitch(switchId);
    header.appendChild(deleteBtn);

    box.appendChild(header);

    // Name
    const nameDiv = document.createElement('div');
    nameDiv.className = 'device-name';
    nameDiv.textContent = switchName;
    box.appendChild(nameDiv);

    makeEditable(nameDiv,
        () => deviceNames[switchId] || switchName,
        (newName) => { deviceNames[switchId] = newName; },
        'Device Name'
    );

    // Buttons
    const letters = ['a', 'b', 'c', 'd', 'e', 'f', 'g'];
    for (let i = 0; i < buttonCount; i++) {
        const letter = letters[i];
        const fullId = `${switchId}${letter}`;

        const outputNode = document.createElement('div');
        outputNode.id = fullId;
        outputNode.className = 'output-node switch-button';
        outputNode.style.position = 'relative'; // Important for endpoints

        const nameSpan = document.createElement('span');
        nameSpan.className = 'output-name';
        nameSpan.textContent = outputNames[fullId] || `Button ${i + 1}`;
        outputNode.appendChild(nameSpan);

        makeEditable(nameSpan,
            () => outputNames[fullId] || `Button ${i + 1}`,
            (newName) => {
                outputNames[fullId] = newName;
                updateConnectionMap();
            },
            'Output name'
        );

        const letterSpan = document.createElement('span');
        letterSpan.className = 'output-letter';
        letterSpan.textContent = letter.toUpperCase();
        outputNode.appendChild(letterSpan);

        box.appendChild(outputNode);
    }

    canvas.appendChild(box);
    // Make draggable using jsPlumb
    instance.draggable(box, { grid: [1, 1] });

    console.log('✅ Switch created:', switchId);

    // Add endpoints AFTER box is in DOM
    setTimeout(() => {
        for (let i = 0; i < buttonCount; i++) {
            const letter = letters[i];
            const fullId = `${switchId}${letter}`;
            const el = document.getElementById(fullId);

            if (el) {
                instance.makeSource(el, {
                    filter: '.output-letter',
                    anchor: 'Right',
                    connectorStyle: { stroke: '#3b82f6', strokeWidth: 3 },
                    endpoint: ['Dot', { radius: 8 }],
                    paintStyle: { fill: '#2563eb', stroke: '#1e40af', strokeWidth: 2 },
                    hoverPaintStyle: { fill: '#1e40af' },
                    maxConnections: -1
                });
                console.log('Added source endpoint:', fullId);
            } else {
                console.error('Element not found:', fullId);
            }
        }
    }, 50); // Increased timeout

    updateConnectionCount();
}

// Add relay
function addRelay(customName = null, customId = null, position = null) {
    if (!instance) return;

    relayCounter++;
    const relayId = customId || generateId();
    const relayName = customName || `Relay ${relayCounter}`;

    relays.push({ id: relayId, name: relayName });
    deviceNames[relayId] = relayName;

    const canvas = document.getElementById('canvas');
    const box = document.createElement('div');
    box.className = 'device-box relay-box';
    box.id = `relay-${relayId}`;

    if (position) {
        box.style.left = position.x + 'px';
        box.style.top = position.y + 'px';
    } else {
        box.style.left = (600 + relays.length * 50) + 'px';
        box.style.top = (100 + relays.length * 30) + 'px';
    }

    // Header
    const header = document.createElement('div');
    header.className = 'device-header';

    const idDiv = document.createElement('div');
    idDiv.className = 'device-id';
    idDiv.textContent = `ID: ${relayId}`;
    header.appendChild(idDiv);

    makeEditable(idDiv,
        () => relayId.toString(),
        (newId) => {
            if (!/^\d+$/.test(newId)) {
                alert('ID must contain only numbers!');
                return;
            }
            updateRelayId(relayId, parseInt(newId));
        },
        'Device ID'
    );

    const deleteBtn = document.createElement('button');
    deleteBtn.className = 'btn btn-danger btn-sm';
    deleteBtn.innerHTML = '🗑️';
    deleteBtn.onclick = () => deleteRelay(relayId);
    header.appendChild(deleteBtn);

    box.appendChild(header);

    // Name
    const nameDiv = document.createElement('div');
    nameDiv.className = 'device-name';
    nameDiv.textContent = relayName;
    box.appendChild(nameDiv);

    makeEditable(nameDiv,
        () => deviceNames[relayId] || relayName,
        (newName) => { deviceNames[relayId] = newName; },
        'Device Name'
    );

    // Outputs
    const letters = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'];
    letters.forEach((letter, index) => {
        const fullId = `${relayId}${letter}`;

        const outputNode = document.createElement('div');
        outputNode.id = fullId;
        outputNode.className = 'output-node relay-output';
        outputNode.style.position = 'relative'; // Important for endpoints

        const nameSpan = document.createElement('span');
        nameSpan.className = 'output-name';
        nameSpan.textContent = outputNames[fullId] || `Output ${index + 1}`;
        outputNode.appendChild(nameSpan);

        makeEditable(nameSpan,
            () => outputNames[fullId] || `Output ${index + 1}`,
            (newName) => {
                outputNames[fullId] = newName;
                updateConnectionMap();
            },
            'Output name'
        );

        const letterSpan = document.createElement('span');
        letterSpan.className = 'output-letter';
        letterSpan.textContent = letter.toUpperCase();
        outputNode.appendChild(letterSpan);

        box.appendChild(outputNode);
    });

    canvas.appendChild(box);
    // Make draggable using jsPlumb
    instance.draggable(box, { grid: [1, 1] });

    console.log('✅ Relay created:', relayId);

    // Add endpoints AFTER box is in DOM
    setTimeout(() => {
        const letters = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'];
        letters.forEach((letter) => {
            const fullId = `${relayId}${letter}`;
            const el = document.getElementById(fullId);

            if (el) {
                instance.makeTarget(el, {
                    filter: '.output-letter',
                    anchor: 'Left',
                    endpoint: ['Dot', { radius: 8 }],
                    paintStyle: { fill: '#d97706', stroke: '#b45309', strokeWidth: 2 },
                    hoverPaintStyle: { fill: '#b45309' },
                    maxConnections: -1
                });
                console.log('Added target endpoint:', fullId);
            } else {
                console.error('Element not found:', fullId);
            }
        });
    }, 50); // Increased timeout

    updateConnectionCount();
}

// Update switch ID
function updateSwitchId(oldId, newId, buttonCount) {
    const allIds = [...switches.map(s => s.id), ...relays.map(r => r.id)];
    if (newId !== oldId && allIds.includes(newId)) {
        alert('This ID already exists!');
        return;
    }

    const switchIndex = switches.findIndex(s => s.id === oldId);
    if (switchIndex === -1) return;

    switches[switchIndex].id = newId;

    if (deviceNames[oldId]) {
        deviceNames[newId] = deviceNames[oldId];
        delete deviceNames[oldId];
    }

    // Update output names
    const letters = ['a', 'b', 'c', 'd', 'e', 'f', 'g'];
    for (let i = 0; i < buttonCount; i++) {
        const oldFullId = `${oldId}${letters[i]}`;
        const newFullId = `${newId}${letters[i]}`;
        if (outputNames[oldFullId]) {
            outputNames[newFullId] = outputNames[oldFullId];
            delete outputNames[oldFullId];
        }
    }

    // Remove old box and recreate
    const oldBox = document.getElementById(`switch-${oldId}`);
    const position = {
        x: parseInt(oldBox.style.left),
        y: parseInt(oldBox.style.top)
    };

    deleteSwitch(oldId, true);
    addSwitch(buttonCount, deviceNames[newId], newId, position);
}

// Update relay ID
function updateRelayId(oldId, newId) {
    const allIds = [...switches.map(s => s.id), ...relays.map(r => r.id)];
    if (newId !== oldId && allIds.includes(newId)) {
        alert('This ID already exists!');
        return;
    }

    const relayIndex = relays.findIndex(r => r.id === oldId);
    if (relayIndex === -1) return;

    relays[relayIndex].id = newId;

    if (deviceNames[oldId]) {
        deviceNames[newId] = deviceNames[oldId];
        delete deviceNames[oldId];
    }

    // Update output names
    const letters = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'];
    letters.forEach(letter => {
        const oldFullId = `${oldId}${letter}`;
        const newFullId = `${newId}${letter}`;
        if (outputNames[oldFullId]) {
            outputNames[newFullId] = outputNames[oldFullId];
            delete outputNames[oldFullId];
        }
    });

    // Remove old box and recreate
    const oldBox = document.getElementById(`relay-${oldId}`);
    const position = {
        x: parseInt(oldBox.style.left),
        y: parseInt(oldBox.style.top)
    };

    deleteRelay(oldId, true);
    addRelay(deviceNames[newId], newId, position);
}

// Delete switch
function deleteSwitch(switchId, skipConfirm = false) {
    if (!skipConfirm && !confirm('Delete this switch?')) return;

    const switchData = switches.find(s => s.id === switchId);
    if (!switchData) return;

    const letters = ['a', 'b', 'c', 'd', 'e', 'f', 'g'];
    for (let i = 0; i < switchData.buttonCount; i++) {
        const fullId = `${switchId}${letters[i]}`;
        instance.remove(fullId);
        delete outputNames[fullId];
    }

    switches = switches.filter(s => s.id !== switchId);
    delete deviceNames[switchId];

    const box = document.getElementById(`switch-${switchId}`);
    if (box) box.remove();

    updateConnectionMap();
    updateConnectionCount();
}

// Delete relay
function deleteRelay(relayId, skipConfirm = false) {
    if (!skipConfirm && !confirm('Delete this relay?')) return;

    const letters = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'];
    letters.forEach(letter => {
        const fullId = `${relayId}${letter}`;
        instance.remove(fullId);
        delete outputNames[fullId];
    });

    relays = relays.filter(r => r.id !== relayId);
    delete deviceNames[relayId];

    const box = document.getElementById(`relay-${relayId}`);
    if (box) box.remove();

    updateConnectionMap();
    updateConnectionCount();
}

// Update connection count
function updateConnectionCount() {
    if (!instance) return;

    const count = instance.getAllConnections().length;
    const countElement = document.getElementById('connectionCount');
    if (countElement) {
        countElement.textContent = count;
    }
    console.log(`📊 Connection count: ${count}`);
}

// Update connection map
function updateConnectionMap() {
    if (!instance) {
        console.warn('⚠️ Instance not ready');
        return;
    }

    const connections = {};
    const allConnections = instance.getAllConnections();

    console.log(`📋 Updating connection map with ${allConnections.length} connections`);

    allConnections.forEach(conn => {
        const sourceId = conn.sourceId;
        const targetId = conn.targetId;

        const sourceMatch = sourceId.match(/^(\d+)([a-z])$/);
        if (!sourceMatch) {
            console.warn('⚠️ Invalid sourceId:', sourceId);
            return;
        }

        const switchId = sourceMatch[1];
        const buttonLetter = sourceMatch[2];

        const targetMatch = targetId.match(/^(\d+)([a-z])$/);
        if (!targetMatch) {
            console.warn('⚠️ Invalid targetId:', targetId);
            return;
        }

        const relayId = targetMatch[1];
        const outputLetter = targetMatch[2];

        if (!connections[switchId]) {
            connections[switchId] = {};
        }

        if (!connections[switchId][buttonLetter]) {
            connections[switchId][buttonLetter] = [];
        }

        connections[switchId][buttonLetter].push([relayId, outputLetter]);
    });

    const formatted = JSON.stringify(connections, null, 4)
        .replace(/"/g, "'")
        .replace(/\[/g, '(')
        .replace(/\]/g, ')');

    document.getElementById('output').textContent = formatted;
    console.log('✅ Connection map updated');
}

// In updateCanvasTransform, add at the end:
function updateCanvasTransform() {
    const canvas = document.getElementById('canvas');
    canvas.style.transform = `translate(${panX}px, ${panY}px) scale(${currentZoom})`;
    document.getElementById('zoomDisplay').textContent = Math.round(currentZoom * 100) + '%';

    if (instance) {
        instance.setZoom(currentZoom);
    }
}

// In makeEditable, add autoSave in the save function:
function makeEditable(element, getValue, setValue, placeholder = '') {
    element.addEventListener('click', function (e) {
        e.stopPropagation();
        const currentValue = getValue();
        const input = document.createElement('input');
        input.type = 'text';
        input.className = element.className.replace('device-', 'device-') + '-input';
        input.value = currentValue;
        input.placeholder = placeholder;

        element.replaceWith(input);
        input.focus();
        input.select();

        function save() {
            const newValue = input.value.trim() || currentValue;
            setValue(newValue);

            const newElement = element.cloneNode();
            newElement.textContent = newValue;
            input.replaceWith(newElement);

            makeEditable(newElement, getValue, setValue, placeholder);
        }

        input.addEventListener('blur', save);
        input.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') save();
        });
        input.addEventListener('click', (e) => e.stopPropagation());
    });
}

// Export full configuration
function exportConnections() {
    if (!instance) return;

    const data = getCurrentState();

    const dataStr = JSON.stringify(data, null, 2);
    const dataBlob = new Blob([dataStr], { type: 'application/json' });
    const url = URL.createObjectURL(dataBlob);

    const link = document.createElement('a');
    link.href = url;
    link.download = `relay-config-full-${Date.now()}.json`;
    link.click();

    URL.revokeObjectURL(url);
    alert('✅ Full configuration exported!');
}

// Export connection map only (Python format)
function exportConnectionMap() {
    if (!instance) return;

    const connections = {};

    instance.getAllConnections().forEach(conn => {
        const sourceId = conn.source.id;
        const targetId = conn.target.id;

        const sourceMatch = sourceId.match(/^(\d+)([a-z])$/);
        const targetMatch = targetId.match(/^(\d+)([a-z])$/);

        if (sourceMatch && targetMatch) {
            const switchId = sourceMatch[1];
            const buttonLetter = sourceMatch[2];
            const relayId = targetMatch[1];
            const outputLetter = targetMatch[2];

            if (!connections[switchId]) {
                connections[switchId] = {};
            }

            if (!connections[switchId][buttonLetter]) {
                connections[switchId][buttonLetter] = [];
            }

            connections[switchId][buttonLetter].push([relayId, outputLetter]);
        }
    });

    // Format as Python-style dict with comments
    let output = "# Connection Map\n";
    output += "# Structure: dict<switchId, dict<buttonLetter, list<tuple(relayId, outputLetter)>>>\n\n";

    const formatted = JSON.stringify(connections, null, 4)
        .replace(/"/g, "'")
        .replace(/\[/g, '(')
        .replace(/\]/g, ')');

    output += formatted;

    const dataBlob = new Blob([output], { type: 'text/plain' });
    const url = URL.createObjectURL(dataBlob);

    const link = document.createElement('a');
    link.href = url;
    link.download = `relay-connections-map-${Date.now()}.py`;
    link.click();

    URL.revokeObjectURL(url);
    alert('✅ Connection map exported!');
}

// Import
function importConnections() {
    if (!instance) return;

    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';

    input.onchange = (e) => {
        const file = e.target.files[0];
        if (!file) return;

        const reader = new FileReader();

        reader.onload = (event) => {
            try {
                const data = JSON.parse(event.target.result);
                restoreState(data);
                saveToLocalStorage(); // Save imported state
                alert('✅ Imported successfully!');
            } catch (error) {
                console.error('Import error:', error);
                alert('❌ Error: ' + error.message);
            }
        };

        reader.readAsText(file);
    };

    input.click();
}

// Clear all connections
function clearAllConnections() {
    if (instance) {
        instance.deleteEveryConnection();
        updateConnectionMap();
        updateConnectionCount();
    }
}

// Get current state
function getCurrentState() {
    const connections = {};

    if (instance) {
        instance.getAllConnections().forEach(conn => {
            const sourceId = conn.sourceId;
            const targetId = conn.targetId;

            const sourceMatch = sourceId.match(/^(\d+)([a-z])$/);
            const targetMatch = targetId.match(/^(\d+)([a-z])$/);

            if (sourceMatch && targetMatch) {
                const switchId = sourceMatch[1];
                const buttonLetter = sourceMatch[2];
                const relayId = targetMatch[1];
                const outputLetter = targetMatch[2];

                if (!connections[switchId]) {
                    connections[switchId] = {};
                }

                if (!connections[switchId][buttonLetter]) {
                    connections[switchId][buttonLetter] = [];
                }

                connections[switchId][buttonLetter].push([relayId, outputLetter]);
            }
        });
    }

    const positions = {};
    [...switches, ...relays].forEach(device => {
        const type = switches.find(s => s.id === device.id) ? 'switch' : 'relay';
        const box = document.getElementById(`${type}-${device.id}`);
        if (box) {
            positions[device.id] = {
                x: parseInt(box.style.left) || 0,
                y: parseInt(box.style.top) || 0
            };
        }
    });

    return {
        switches: switches,
        relays: relays,
        connections: connections,
        deviceNames: deviceNames,
        outputNames: outputNames,
        positions: positions,
        canvasState: {
            zoom: currentZoom,
            panX: panX,
            panY: panY
        },
        settings: {
            connectionColor: document.getElementById('connectionColor')?.value || '#3b82f6'
        },
        timestamp: new Date().toISOString()
    };
}


// Restore state from data
function restoreState(data) {
    console.log('🔄 Restoring state...');

    // Assume everything is already cleared (either by init or handleConfigFromWS)

    // Set data
    switches = [];
    relays = [];
    switchCounter = 0;
    relayCounter = 0;
    deviceNames = data.deviceNames || {};
    outputNames = data.outputNames || {};

    if (data.settings && data.settings.connectionColor) {
        const colorPicker = document.getElementById('connectionColor');
        if (colorPicker) {
            colorPicker.value = data.settings.connectionColor;
        }
    }

    if (data.canvasState) {
        currentZoom = data.canvasState.zoom || 1;
        panX = data.canvasState.panX || 0;
        panY = data.canvasState.panY || 0;
        updateCanvasTransform();
    }

    // Recreate switches
    if (data.switches) {
        console.log(`➕ Creating ${data.switches.length} switches`);
        data.switches.forEach(sw => {
            switchCounter++;
            const position = data.positions ? data.positions[sw.id] : null;
            addSwitch(sw.buttonCount, deviceNames[sw.id] || sw.name, sw.id, position);
            // NO instance.draggable() here!
        });
    }

    // Recreate relays
    if (data.relays) {
        console.log(`➕ Creating ${data.relays.length} relays`);
        data.relays.forEach(relay => {
            const position = data.positions ? data.positions[relay.id] : null;
            addRelay(deviceNames[relay.id] || relay.name, relay.id, position);
            // NO instance.draggable() here!
        });
    }

    // Wait for DOM, then recreate connections
    setTimeout(() => {
        if (data.connections) {
            console.log('🔗 Recreating connections...');

            // Suspend events during batch creation
            instance.setSuspendEvents(true);

            let restoreCount = 0;

            Object.keys(data.connections).forEach(switchId => {
                Object.keys(data.connections[switchId]).forEach(buttonLetter => {
                    data.connections[switchId][buttonLetter].forEach(([relayId, outputLetter]) => {
                        try {
                            const sourceId = `${switchId}${buttonLetter}`;
                            const targetId = `${relayId}${outputLetter}`;

                            instance.connect({
                                source: sourceId,
                                target: targetId
                            });

                            restoreCount++;
                        } catch (err) {
                            console.error('❌ Connection error:', err);
                        }
                    });
                });
            });

            // Resume events
            instance.setSuspendEvents(false);

            console.log(`✅ Restored ${restoreCount} connections`);
        }

        // Update display
        setTimeout(() => {
            const finalCount = instance.getAllConnections().length;
            console.log(`📊 Final connection count: ${finalCount}`);

            updateConnectionMap();
            updateConnectionCount();
            instance.repaintEverything();

            console.log('✅ State fully restored');
        }, 100);

    }, 500);
}


// Get cookie value
function getCookie(cname) {
    let name = cname + "=";
    let decodedCookie = decodeURIComponent(document.cookie);
    let ca = decodedCookie.split(';');
    for (let i = 0; i < ca.length; i++) {
        let c = ca[i];
        while (c.charAt(0) == ' ') {
            c = c.substring(1);
        }
        if (c.indexOf(name) == 0) {
            return c.substring(name.length, c.length);
        }
    }
    return "";
}


// WebSocket functions
function initWebSocket() {
    try {
        auth_token = getCookie("access_token")
        ws = new WebSocket(`wss://${window.location.host}/rcm/ws/` + wsId + `?token=` + auth_token)

        ws.onopen = function () {
            console.log('✅ WebSocket connected');
            wsConnected = true;
            updateWSStatus(true);
            clearTimeout(reconnectTimeout);
        };

        ws.onmessage = function (event) {
            try {
                const config = JSON.parse(event.data);
                console.log('📨 WebSocket received configuration:', config);

                if (config.switches && config.relays && config.connections) {
                    handleConfigFromWS(config);
                } else {
                    console.error('Invalid configuration format received');
                }
            } catch (error) {
                console.error('Failed to parse WebSocket message:', error);
            }
        };

        ws.onerror = function (error) {
            console.error('❌ WebSocket error:', error);
            wsConnected = false;
            updateWSStatus(false);
        };

        ws.onclose = function () {
            console.log('⚠️ WebSocket disconnected');
            wsConnected = false;
            updateWSStatus(false);

            reconnectTimeout = setTimeout(() => {
                console.log('🔄 Attempting to reconnect...');
                initWebSocket();
            }, 3000);
        };

    } catch (error) {
        console.error('Failed to initialize WebSocket:', error);
        wsConnected = false;
        updateWSStatus(false);
    }
}

function updateWSStatus(connected) {
    const statusElement = document.getElementById('wsStatus');
    if (statusElement) {
        if (connected) {
            statusElement.textContent = '🟢 Connected';
            statusElement.style.color = '#10b981';
        } else {
            statusElement.textContent = '🔴 Disconnected';
            statusElement.style.color = '#ef4444';
        }
    }
}

function sendConfigToWS() {
    if (!ws || !wsConnected) {
        alert('❌ WebSocket is not connected!');
        return;
    }

    if (ws.readyState !== WebSocket.OPEN) {
        alert('❌ WebSocket is not ready. Please wait...');
        return;
    }

    try {
        const config = getCurrentState();

        ws.send(JSON.stringify(config));
        console.log('📤 Sent configuration to server:', config);

        const statusElement = document.getElementById('wsStatus');
        const originalText = statusElement.textContent;
        statusElement.textContent = '✅ Sent!';
        setTimeout(() => {
            statusElement.textContent = originalText;
        }, 2000);

    } catch (error) {
        console.error('Failed to send configuration:', error);
        alert('❌ Failed to send: ' + error.message);
    }
}

// Handle configuration received from WebSocket
function handleConfigFromWS(config) {
    if (!config) {
        console.error('No configuration received');
        return;
    }
    try {
        console.log('🔄 Auto-loading configuration from server...');

        // Complete reset - clear everything
        document.getElementById('canvas').innerHTML = '';
        switches = [];
        relays = [];
        switchCounter = 0;
        relayCounter = 0;
        deviceNames = {};
        outputNames = {};

        // Reset jsPlumb completely
        if (instance) {
            instance.reset();
        }

        // Re-initialize jsPlumb with fresh event bindings
        initializeJsPlumb();

        // Small delay to ensure everything is cleared
        setTimeout(() => {
            restoreState(config);

            const statusElement = document.getElementById('wsStatus');
            if (statusElement) {
                const originalText = statusElement.textContent;
                statusElement.textContent = '✅ Loaded';
                setTimeout(() => { statusElement.textContent = originalText; }, 2000);
            }
        }, 100);

    } catch (error) {
        console.error('Failed to auto-load configuration:', error);
    }
}

function disconnectWebSocket() {
    if (ws) {
        ws.close();
        ws = null;
        wsConnected = false;
        updateWSStatus(false);
        clearTimeout(reconnectTimeout);
        console.log('🔌 WebSocket manually disconnected');
    }
}

function reconnectWebSocket() {
    disconnectWebSocket();
    setTimeout(() => {
        initWebSocket();
    }, 500);
}


// Main init
async function init() {
    try {
        console.log('Starting initialization...');

        await waitForJsPlumb();
        console.log('jsPlumb loaded');

        document.getElementById('loadingMessage').style.display = 'none';
        document.getElementById('controls').style.display = 'flex';
        document.getElementById('canvasContainer').style.display = 'block';
        document.getElementById('infoPanel').style.display = 'block';

        initializeJsPlumb();
        console.log('jsPlumb initialized');

        // Initialize WebSocket connection
        initWebSocket();
        console.log('WebSocket initialized');

    } catch (error) {
        console.error('Init error:', error);
        document.getElementById('loadingMessage').innerHTML = `
            <div style="color: #ef4444; text-align: center; padding: 60px 20px;">
                <h3 style="margin-bottom: 15px;">❌ Initialization Failed</h3>
                <p style="margin-bottom: 20px;">${error.message}</p>
                <button class="btn btn-primary" onclick="location.reload()">
                    🔄 Reload Page
                </button>
            </div>
        `;
    }
}

// Start initialization when page loads
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
} else {
    init();
}