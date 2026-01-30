// RCM Cytoscape.js Implementation - Stage 1
// Parallel implementation for performance testing

let cy = null;

// Mock data mirroring jsPlumb structure
const mockSwitches = {
    12345: { name: "Living Room", buttonCount: 4, x: 24800, y: 24800, color: '#6366f1' },
    12346: { name: "Kitchen", buttonCount: 3, x: 25200, y: 24800, color: '#10b981' },
    12347: { name: "Bedroom", buttonCount: 4, x: 24800, y: 25200, color: '#f59e0b' }
};

const mockRelays = {
    54321: {
        name: "Main Relay", x: 25600, y: 24800, outputs: {
            'a': 'Light 1', 'b': 'Light 2', 'c': 'Light 3', 'd': 'Light 4',
            'e': 'Light 5', 'f': 'Light 6', 'g': 'Light 7', 'h': 'Light 8'
        }
    },
    54322: {
        name: "Secondary", x: 25600, y: 25200, outputs: {
            'a': 'Outlet 1', 'b': 'Outlet 2', 'c': 'Outlet 3', 'd': 'Outlet 4'
        }
    }
};

const mockConnections = {
    12345: {
        'a': [{ relayId: 54321, outputId: 'a' }],
        'b': [{ relayId: 54321, outputId: 'b' }],
        'c': [{ relayId: 54321, outputId: 'c' }]
    },
    12346: {
        'a': [{ relayId: 54321, outputId: 'd' }],
        'b': [{ relayId: 54322, outputId: 'a' }]
    },
    12347: {
        'a': [{ relayId: 54322, outputId: 'b' }],
        'b': [{ relayId: 54322, outputId: 'c' }]
    }
};

// Convert data to Cytoscape format
function generateCytoscapeElements() {
    const elements = [];

    // Add switch nodes
    for (const [switchId, switchData] of Object.entries(mockSwitches)) {
        // Main switch container node
        elements.push({
            group: 'nodes',
            data: {
                id: `switch-${switchId}`,
                label: switchData.name,
                type: 'switch',
                deviceId: switchId,
                color: switchData.color
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
                    buttonId: buttonId
                },
                position: { x: switchData.x, y: switchData.y + 40 + (i * 30) }
            });
        }
    }

    // Add relay nodes
    for (const [relayId, relayData] of Object.entries(mockRelays)) {
        // Main relay container node
        elements.push({
            group: 'nodes',
            data: {
                id: `relay-${relayId}`,
                label: relayData.name,
                type: 'relay',
                deviceId: relayId
            },
            position: { x: relayData.x, y: relayData.y },
            classes: 'relay-container'
        });

        // Output nodes (for connection endpoints)
        let outputIndex = 0;
        for (const [outputId, outputName] of Object.entries(relayData.outputs)) {
            elements.push({
                group: 'nodes',
                data: {
                    id: `relay-${relayId}-output-${outputId}`,
                    label: outputName,
                    type: 'output',
                    parent: `relay-${relayId}`,
                    outputId: outputId
                },
                position: { x: relayData.x, y: relayData.y + 40 + (outputIndex * 30) }
            });
            outputIndex++;
        }
    }

    // Add connection edges
    for (const [switchId, buttons] of Object.entries(mockConnections)) {
        const switchData = mockSwitches[switchId];
        const connectionColor = switchData?.color || '#6366f1';

        for (const [buttonId, connections] of Object.entries(buttons)) {
            for (const conn of connections) {
                elements.push({
                    group: 'edges',
                    data: {
                        id: `edge-${switchId}-${buttonId}-${conn.relayId}-${conn.outputId}`,
                        source: `switch-${switchId}-btn-${buttonId}`,
                        target: `relay-${conn.relayId}-output-${conn.outputId}`,
                        color: connectionColor
                    }
                });
            }
        }
    }

    return elements;
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
    zoomIn: zoomIn,
    zoomOut: zoomOut,
    resetZoom: resetZoom,
    getInstance: () => cy
};
