const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')
const vm = require('node:vm')
const { test } = require('node:test')
const { JSDOM } = require('jsdom')
const { HeatingSeries } = require('../../static/scripts/heating-series.js')

const root = path.join(__dirname, '../..')
const script = name => fs.readFileSync(path.join(root, 'static/scripts', name), 'utf8')
function page(html = '') {
    const dom = new JSDOM(html, { url: 'https://domator.example:8443/', runScripts: 'outside-only' })
    dom.window.setTimeout = () => 0
    dom.window.setInterval = () => 0
    dom.window.Headers = Headers
    dom.window.console = { log() {}, warn() {}, info() {}, error() {} }
    return dom
}
function run(dom, source) { return vm.runInContext(source, dom.getInternalVMContext()) }
const payloads = ['<img src=x onerror="alert(1)">', '" autofocus onfocus="alert(1)', "' onmouseover='alert(1)", '&quot;<script>alert(1)</script>']
// Waveshare tool defaults, as an HMMD presence sensor reports them.
const SENSOR_CONFIG = {
    min: 0, max: 12, delay: 30, led: false, fw: 'v1.5.9',
    trigger: [60000, 30000, 3000, 2000, 500, 400, 400, 300, 300, 300, 300, 250, 250, 200, 200, 200],
    hold: [40000, 20000, 400, 300, 300, 200, 200, 150, 150, 100, 100, 100, 100, 100, 100, 100]
}

test('blind names are DOM text/value, including stored quotes, entities and markup', () => {
    const dom = page('<section id="relay-blinds-section"><div id="relay-blinds-cards"></div></section>')
    try {
        run(dom, `class WebSocketManager { connect() {} startConnectionCheck() {} setupVisibilityHandler() {} }
            function $(callback) {}\n${script('blinds.js')}`)
        for (const value of payloads) {
            dom.window.sample = [{ relay_id: 111, power_id: 'a', direction_id: 'b', name: value, relay_name: value }]
            run(dom, 'renderRelayBlinds(sample)')
            const document = dom.window.document
            assert.equal(document.querySelector('.rblind-name-text').textContent, value)
            assert.equal(document.querySelector('.rblind-name-input').value, value)
            assert.equal(document.querySelector('.rblind-subname').textContent, value)
            assert.equal(document.querySelectorAll('script, [onerror], [onfocus], [onmouseover], [autofocus]').length, 0)
        }
        dom.window.sample[0].direction_id = "b' onclick='alert(1)"
        run(dom, 'renderRelayBlinds(sample)')
        assert.equal(dom.window.document.querySelectorAll('.relay-blind-card').length, 0)
    } finally { dom.window.close() }
})

test('RCM renders stored names literally and ignores malformed blind-pair IDs', () => {
    const dom = page(fs.readFileSync(path.join(root, 'static/rcm.html'), 'utf8'))
    try {
        dom.window.jsPlumb = { ready() {} }
        run(dom, `${script('common.js')}\n${script('rcm.js')}\n
            jsPlumbInstance = { draggable() {}, makeSource() {}, makeTarget() {}, repaintEverything() {} }`)
        for (const value of payloads) {
            dom.window.hostileName = value
            run(dom, `document.getElementById('canvas').innerHTML = '';
                State.blindPairs = { '111': [['a', "b' onclick='alert(1)"]] };
                createSwitch(222, hostileName, 3, 0, 0);
                createRelay(111, hostileName, { a: [hostileName, 1, 0, 0] }, 0, 0, 8);`)
            const doc = dom.window.document
            assert.equal(doc.querySelector('.device-name-switch-222').textContent, value)
            assert.equal(doc.querySelector('.device-name-relay-111').textContent, value)
            assert.equal(doc.querySelector('.output-name-111-a').textContent, value)
            assert.equal(doc.querySelectorAll('[onerror], [onfocus], [autofocus]').length, 0)
            assert.equal(doc.querySelectorAll('.blind-pair-group').length, 0)
        }
        run(dom, `document.getElementById('canvas').innerHTML = '';
            State.blindPairs = { '111': [['a', 'b']] };
            createRelay(111, 'Relay', {}, 0, 0, 8);`)
        const swap = dom.window.document.querySelector('.blind-swap-btn')
        assert.equal(swap.getAttribute('onclick'), null)
        assert.equal(swap.dataset.output, 'a')
        assert.equal(swap.dataset.direction, 'b')
    } finally { dom.window.close() }
})

test('history and live points use identical buckets, preserve live races, gaps and one-hour retention', () => {
    const series = new HeatingSeries()
    // Live arrives before the history HTTP request completes.
    series.merge([{ timestamp: 3601, cold: 20 }], true, 3605)
    series.merge([{ timestamp: 3600, cold: '10' }, { timestamp: 3592, cold: '8' }], false, 3605)
    let cold = series.datasets()[0]
    assert.deepEqual(cold, [{ x: 3592000, y: 8 }, { x: 3596000, y: null }, { x: 3600000, y: 20 }])
    series.merge([{ timestamp: 3603, cold: 21 }, { timestamp: 3602, cold: 99 }], true, 3605)
    assert.equal(series.datasets()[0].at(-1).y, 21)
    series.merge(Array.from({ length: 4000 }, (_, i) => ({ timestamp: i + 3604, cold: i })), true, 7605)
    cold = series.datasets()[0]
    assert.ok(cold.length <= 901)
    assert.ok(cold[0].x >= Math.floor((7605 - 3600) / 4) * 4000)
    assert.ok(series.points.size <= 901)
    series.merge([{ timestamp: Infinity, cold: 1 }, { timestamp: 7604, cold: 'NaN' }], true, 7605)
    assert.equal(series.datasets()[0].at(-1).y, null)
})

test('browser posts use a session CSRF token and retain the current host/port', async () => {
    const dom = page()
    const calls = []
    try {
        dom.window.fetch = async (url, options) => {
            calls.push([String(url), options])
            return { ok: true, status: 200, json: async () => ({ token: 'csrf-test' }) }
        }
        run(dom, script('common.js'))
        await run(dom, `apiFetch('/lights/name_output', { method: 'POST', body: 'test' })`)
        assert.equal(calls[0][0], '/csrf-token')
        assert.equal(calls[1][0], 'https://domator.example:8443/lights/name_output')
        assert.equal(calls[1][1].headers.get('X-CSRF-Token'), 'csrf-test')
        await assert.rejects(run(dom, `apiFetch('https://evil.example/', { method: 'POST' })`), /Cross-origin/)
        assert.equal(calls.length, 2)
    } finally { dom.window.close() }
})

test('Sentry is optional, uses integrity, and disables default PII', () => {
    const dom = page()
    try {
        run(dom, script('sentry.js'))
        assert.equal(dom.window.document.querySelector('script'), null)
        dom.window.SENTRY_DSN = 'https://public@example.com/1'
        dom.window.SENTRY_TRACES_SAMPLE_RATE = 0.05
        let options
        dom.window.Sentry = { init(value) { options = value } }
        run(dom, script('sentry.js'))
        const sdk = dom.window.document.querySelector('script')
        assert.match(sdk.integrity, /^sha384-[A-Za-z0-9+/]{64}$/)
        assert.equal(sdk.crossOrigin, 'anonymous')
        sdk.onload()
        assert.equal(options.sendDefaultPii, false)
        assert.equal(options.tracesSampleRate, 0.05)
    } finally { dom.window.close() }
})


test('RCM gateway controls follow reported roles and retain remote button cards', () => {
    const dom = page(fs.readFileSync(path.join(root, 'static/rcm.html'), 'utf8'))
    try {
        dom.window.jsPlumb = { ready() {} }
        run(dom, `${script('common.js')}\n${script('rcm.js')}\n
            jsPlumbInstance = { draggable() {}, makeSource() {}, makeTarget() {}, repaintEverything() {} };
            online_switches = new Set([222]);
            State.deviceRoles = {222: {type: 'switch', gateway: true,
                gateway_mac: '02:00:00:00:00:01', channel: 11}};
            createSwitch(222, 'Switch', 7, 0, 0);`)
        const button = dom.window.document.querySelector('[data-gateway-switch="222"]')
        assert.equal(button.hidden, false)
        assert.equal(button.textContent, '📡 Gateway: on')
        assert.equal(button.getAttribute('aria-pressed'), 'true')
        assert.equal(button.disabled, false)
        assert.match(button.title, /02:00:00:00:00:01/)
        run(dom, `online_switches = new Set(); updateGatewayControls()`)
        assert.equal(button.hidden, false)
        assert.equal(button.disabled, true)
        run(dom, `online_switches = new Set([222]); updateGatewayControls()`)
        run(dom, `State.deviceRoles[222] = {type: 'remote'}; updateGatewayControls()`)
        assert.equal(button.hidden, true)
        assert.equal(dom.window.document.querySelector('[data-remote-switch="222"]').hidden, false)
        assert.ok(dom.window.document.getElementById('switch-222-btn-g'))
    } finally { dom.window.close() }
})

test('RCM remote badge aligns with identity and stays hidden for powered switches', () => {
    const dom = rcmPage()
    try {
        const style = dom.window.document.createElement('style')
        style.textContent = fs.readFileSync(path.join(root, 'static/styles/rcm.css'), 'utf8')
        dom.window.document.head.appendChild(style)
        run(dom, `State.deviceRoles[222] = {type: 'remote'};
            createSwitch(222, 'Battery switch', 4, 0, 0);
            createSwitch(333, 'Powered switch', 3, 0, 0);`)
        const doc = dom.window.document
        const remote = doc.querySelector('[data-remote-switch="222"]')
        assert.equal(remote.hidden, false)
        assert.equal(remote.closest('.device-header-identity').querySelector('.device-id').textContent, 'ID: 222')
        assert.equal(dom.window.getComputedStyle(remote).display, 'inline-flex')
        assert.equal(dom.window.getComputedStyle(remote.parentElement).alignItems, 'center')
        assert.equal(dom.window.getComputedStyle(doc.querySelector('.device-header-actions')).alignItems, 'center')
        assert.equal(dom.window.getComputedStyle(doc.querySelector('[data-remote-switch="333"]')).display, 'none')
        assert.equal(dom.window.getComputedStyle(doc.querySelector('[data-gateway-switch="222"]')).display, 'none')
    } finally { dom.window.close() }
})

test('RCM remote clicks flash all inputs and repeated clicks restart only their own timer', () => {
    const dom = rcmPage()
    try {
        run(dom, `State.deviceRoles[222] = {type: 'remote'};
            createSwitch(222, 'Battery switch', 4, 0, 0);`)
        const pending = new Map()
        let timerId = 0
        dom.window.setTimeout = (callback, delay) => {
            assert.equal(delay, 5000)
            pending.set(++timerId, callback)
            return timerId
        }
        dom.window.clearTimeout = id => pending.delete(id)
        const click = button => {
            dom.window.buttonMessage = {type: 'switch_state', switch_id: 222, button_id: button}
            run(dom, 'wsManager.onMessage({data: JSON.stringify(buttonMessage)})')
        }
        for (const button of ['a', 'b', 'c']) {
            click(button)
            assert.ok(dom.window.document.getElementById(`switch-222-btn-${button}`).classList.contains('button-activity'))
        }
        assert.equal(pending.size, 3)
        const oldBTimer = run(dom, 'State.buttonHighlightTimers.get("222-b")')
        click('b')
        assert.equal(pending.size, 3)
        assert.equal(pending.has(oldBTimer), false)
        const newBTimer = run(dom, 'State.buttonHighlightTimers.get("222-b")')
        pending.get(newBTimer)()
        assert.equal(dom.window.document.getElementById('switch-222-btn-b').classList.contains('button-activity'), false)
        assert.ok(dom.window.document.getElementById('switch-222-btn-a').classList.contains('button-activity'))
        assert.ok(dom.window.document.getElementById('switch-222-btn-c').classList.contains('button-activity'))
        run(dom, 'appSettings.enableButtonHighlightAnimation = false')
        click('d')
        assert.equal(dom.window.document.getElementById('switch-222-btn-d').classList.contains('button-activity'), false)
    } finally { dom.window.close() }
})

function rcmPage({ userAgent } = {}) {
    const dom = page(fs.readFileSync(path.join(root, 'static/rcm.html'), 'utf8'))
    if (userAgent) Object.defineProperty(dom.window.navigator, 'userAgent', { value: userAgent })
    dom.window.jsPlumb = { ready() {} }
    run(dom, `${script('common.js')}\n${script('rcm.js')}\n
        canvasElement = document.getElementById('canvas');
        zoomLevelElement = document.getElementById('zoomLevel');
        const handlers = {};
        const eventListeners = {};
        const liveConnections = [];
        jsPlumbInstance = {
            draggable() {}, makeSource() {}, makeTarget() {}, repaintEverything() {},
            revalidate() {}, setZoom() {}, batch(callback) { callback() },
            getAllConnections() { return [...liveConnections] },
            deleteEveryConnection() { [...liveConnections].forEach(conn => this.deleteConnection(conn)) },
            deleteEveryEndpoint() {},
            deleteConnection(conn) {
                const index = liveConnections.indexOf(conn);
                if (index < 0) return;
                liveConnections.splice(index, 1);
                conn.canvas.remove();
                conn.endpoints.forEach(ep => ep.canvas.remove());
            },
            unbind(event) { eventListeners[event] = [] },
            bind(event, callback) {
                (eventListeners[event] ||= []).push(callback);
                handlers[event] = (...args) => Promise.all(eventListeners[event].map(fn => fn(...args)));
            },
            connect({source, target, paintStyle}) {
                const canvas = document.createElement('div');
                canvas.className = 'jtk-connector';
                document.getElementById('canvas').appendChild(canvas);
                const endpoints = [source, target].map(el => {
                    const endpoint = document.createElement('div');
                    el.appendChild(endpoint);
                    return {canvas: endpoint};
                });
                const conn = { source, target, canvas, endpoints,
                    sourceId: source.id, targetId: target.id, visible: true, paintStyle,
                    setPaintStyle(style) { this.paintStyle = style },
                    getPaintStyle() { return this.paintStyle },
                    setVisible(value) { this.visible = value } };
                liveConnections.push(conn);
                return conn;
            }
        };
        addConnectionHoverEffect = () => {};
        wsManager.send = () => {};`)
    return dom
}

test('RCM arrangements cycle, keep types separated, avoid overlaps and preserve saved positions', () => {
    const dom = rcmPage()
    try {
        run(dom, `for (const id of [1, 2, 3]) {
            createSwitch(id, 'Switch', 3, 0, 0);
            createRelay(id, 'Relay', {}, 0, 0, 8);
        }`)
        const doc = dom.window.document
        doc.querySelectorAll('.device-box').forEach((el, index) => {
            Object.defineProperty(el, 'offsetWidth', { value: 320 + index * 10 })
            Object.defineProperty(el, 'offsetHeight', { value: 200 + index * 70 })
        })
        for (const style of ['columns', 'grid', 'paired', 'connections', 'columns']) {
            run(dom, 'cycleDeviceArrangement()')
            assert.equal(dom.window.localStorage.getItem('rcm_arrangement_style'), style)
            const switches = [...doc.querySelectorAll('.switch-box')]
            const relays = [...doc.querySelectorAll('.relay-box')]
            if (style === 'columns' || style === 'grid') {
                assert.ok(Math.max(...switches.map(el => parseFloat(el.style.left) + el.offsetWidth))
                    < Math.min(...relays.map(el => parseFloat(el.style.left))))
            }
            const cards = [...switches, ...relays]
            cards.forEach((a, i) => cards.slice(i + 1).forEach(b => {
                const x = el => parseFloat(el.style.left), y = el => parseFloat(el.style.top)
                assert.ok(x(a) + a.offsetWidth <= x(b) || x(b) + b.offsetWidth <= x(a)
                    || y(a) + a.offsetHeight <= y(b) || y(b) + b.offsetHeight <= y(a))
            }))
            if (style === 'paired' || style === 'connections') switches.forEach(sw => {
                const relay = doc.getElementById(sw.id.replace('switch', 'relay'))
                assert.equal(sw.style.top, relay.style.top)
                assert.equal(parseFloat(sw.style.left) - parseFloat(relay.style.left) - relay.offsetWidth, 24)
            })
        }
        run(dom, `document.getElementById('switch-1').style.left = '12345px';
            saveDevicePositions(); arrangeDevices('grid', {preserveSaved: true, focus: false});`)
        assert.equal(doc.getElementById('switch-1').style.left, '12345px')
        assert.match(doc.getElementById('arrangeDevicesButton').textContent, /Columns/)
    } finally { dom.window.close() }
})

test('RCM hides only unused relay-switch buttons and hides again after their last connection is removed', async () => {
    const dom = rcmPage()
    try {
        run(dom, `createRelay(1, 'Relay', {}, 0, 0, 8);
            createRelay(2, 'Relay', {}, 0, 0, 8);
            createSwitch(1, 'Relay switch', 3, 0, 0);
            createSwitch(9, 'Normal switch', 3, 0, 0);`)
        const doc = dom.window.document
        assert.equal(doc.getElementById('switch-1-btn-a').style.display, 'none')
        assert.equal(doc.getElementById('switch-9-btn-a').style.display, 'flex')
        run(dom, `showButton(1, 'a'); syncRelaySwitchButtons(1);`)
        assert.equal(doc.getElementById('switch-1-btn-a').style.display, 'flex')
        run(dom, `createConnection(1, 'a', 1, 'a'); createConnection(1, 'a', 2, 'b');
            postForm = async () => ({success: true}); bindJsPlumbEvents();`)
        assert.equal(doc.getElementById('switch-1-btn-a').style.display, 'flex')
        assert.equal(run(dom, 'connections[1].a[0].connection.visible'), true)
        await run(dom, `handlers.dblclick(connections[1].a[0].connection, {preventDefault() {}}); Promise.resolve()`)
        assert.equal(doc.getElementById('switch-1-btn-a').style.display, 'flex')
        await run(dom, `handlers.dblclick(connections[1].a[0].connection, {preventDefault() {}}); Promise.resolve()`)
        assert.equal(doc.getElementById('switch-1-btn-a').style.display, 'none')
        assert.equal(doc.getElementById('switch-1-show-hidden-btn').style.display, 'block')
        assert.deepEqual(JSON.parse(dom.window.localStorage.getItem('rcm_hidden_buttons')), [])
    } finally { dom.window.close() }
})

test('RCM initial load arranges switches left and retains connections on manually hidden buttons', async () => {
    const dom = rcmPage()
    try {
        run(dom, `localStorage.setItem('rcm_hidden_buttons', JSON.stringify(['1-a']));
            const pendingTimers = [];
            window.setTimeout = callback => { pendingTimers.push(callback); return 0 };
            fetchAPI = async path => ({
                '/lights/get_relays': {1: ['Relay', 8]},
                '/lights/get_switches': {1: ['Relay switch', 3], 9: ['Normal switch', 2]},
                '/lights/get_connections': {1: {a: [[1, 'a']], b: [[1, 'b']]}},
                '/lights/get_outputs': {}, '/lights/get_all_buttons': {}, '/lights/get_blind_pairs': {}
            })[path];`)
        await run(dom, 'loadConfiguration()')
        run(dom, 'pendingTimers.splice(0).forEach(callback => callback())')
        const doc = dom.window.document
        assert.equal(doc.getElementById('switch-1-btn-a').style.display, 'none')
        assert.equal(doc.getElementById('switch-1-btn-b').style.display, 'flex')
        assert.equal(doc.getElementById('switch-1-btn-c').style.display, 'none')
        assert.equal(doc.getElementById('switch-9-btn-a').style.display, 'flex')
        assert.ok(parseFloat(doc.getElementById('switch-9').style.left) < parseFloat(doc.getElementById('relay-1').style.left))
        assert.equal(run(dom, 'connections[1].a.length'), 1)
        assert.equal(run(dom, 'connections[1].a[0].connection.visible'), false)
        run(dom, "showButton(1, 'a')")
        assert.equal(run(dom, 'connections[1].a[0].connection.visible'), true)
    } finally { dom.window.close() }
})

test('RCM arrangements target a 16:9 footprint and fit measured cards within the viewport', () => {
    const dom = rcmPage()
    try {
        run(dom, `for (let id = 1; id <= 24; id++) {
            createSwitch(id, 'Switch', 3, 0, 0);
            createRelay(id, 'Relay', {}, 0, 0, 8);
        }`)
        const cards = [...dom.window.document.querySelectorAll('.device-box')]
        cards.forEach(el => {
            Object.defineProperty(el, 'offsetWidth', { value: 320 })
            Object.defineProperty(el, 'offsetHeight', { value: 600 })
        })
        const wrapper = dom.window.document.getElementById('canvas-wrapper')
        Object.defineProperty(wrapper, 'clientWidth', { value: 1280 })
        Object.defineProperty(wrapper, 'clientHeight', { value: 720 })
        for (const style of ['columns', 'grid', 'paired', 'connections']) {
            dom.window.layoutStyle = style
            run(dom, 'arrangeDevices(layoutStyle)')
            const minX = Math.min(...cards.map(el => parseFloat(el.style.left)))
            const maxX = Math.max(...cards.map(el => parseFloat(el.style.left) + el.offsetWidth))
            const minY = Math.min(...cards.map(el => parseFloat(el.style.top)))
            const maxY = Math.max(...cards.map(el => parseFloat(el.style.top) + el.offsetHeight))
            const ratio = (maxX - minX) / (maxY - minY)
            assert.ok(Math.abs(ratio - 16 / 9) < 0.15, `${style} ratio: ${ratio}`)
            const zoom = run(dom, 'zoomLevel'), panX = run(dom, 'panX'), panY = run(dom, 'panY')
            assert.ok(minX * zoom + panX >= 39)
            assert.ok(maxX * zoom + panX <= 1241)
            assert.ok(minY * zoom + panY >= 39)
            assert.ok(maxY * zoom + panY <= 681)
        }
    } finally { dom.window.close() }
})

test('RCM Connections layout groups by wiring and keeps relay switches beside their relays', () => {
    const dom = rcmPage()
    try {
        run(dom, `createRelay(100, 'Relay A', {}, 0, 0, 8);
            createRelay(200, 'Relay B', {}, 0, 0, 8);
            createSwitch(100, 'Relay switch', 3, 0, 0);
            createSwitch(9, 'Switch B', 3, 0, 0);
            createSwitch(10, 'Switch A', 3, 0, 0);
            createSwitch(11, 'Unconnected', 3, 0, 0);
            createSwitch(12, 'Hidden', 3, 123, 456);
            hiddenDevices.add('switch-12');
            createConnection(9, 'a', 200, 'a');
            createConnection(10, 'a', 100, 'a');`)
        const doc = dom.window.document
        doc.querySelectorAll('.device-box').forEach(el => {
            Object.defineProperty(el, 'offsetWidth', { value: 320 })
            Object.defineProperty(el, 'offsetHeight', { value: 300 })
        })
        run(dom, "arrangeDevices('connections')")
        for (const [switchId, relayId] of [[9, 200], [10, 100]]) {
            const sw = doc.getElementById(`switch-${switchId}`)
            const relay = doc.getElementById(`relay-${relayId}`)
            assert.equal(sw.style.top, relay.style.top)
            assert.equal(parseFloat(relay.style.left) - parseFloat(sw.style.left) - sw.offsetWidth, 240)
        }
        const relaySwitch = doc.getElementById('switch-100'), relay = doc.getElementById('relay-100')
        assert.equal(relaySwitch.style.top, relay.style.top)
        assert.equal(parseFloat(relaySwitch.style.left) - parseFloat(relay.style.left) - relay.offsetWidth, 24)
        assert.equal(doc.getElementById('switch-12').style.left, '123px')
        assert.equal(doc.getElementById('switch-12').style.top, '456px')
        // Joining two clusters keeps every card exactly once and without overlaps.
        run(dom, "createConnection(9, 'b', 100, 'b'); arrangeDevices('connections')")
        const cards = [...doc.querySelectorAll('.device-box')].filter(el => el.id !== 'switch-12')
        cards.forEach((a, i) => cards.slice(i + 1).forEach(b => {
            const x = el => parseFloat(el.style.left), y = el => parseFloat(el.style.top)
            assert.ok(x(a) + a.offsetWidth <= x(b) || x(b) + b.offsetWidth <= x(a)
                || y(a) + a.offsetHeight <= y(b) || y(b) + b.offsetHeight <= y(a))
        }))
        assert.equal(run(dom, 'connections[9].a.length + connections[9].b.length + connections[10].a.length'), 3)
    } finally { dom.window.close() }
})

test('RCM knob actions have labels, save commands and restore server choices', () => {
    const dom = page(fs.readFileSync(path.join(root, 'static/rcm.html'), 'utf8'))
    try {
        dom.window.jsPlumb = { ready() {} }
        dom.window.sentCommands = []
        dom.window.knobConfig = {
            switch_id: 281474976710656, topic: "zigbee2mqtt/tyua_knob",
            buttons: {
                a: { label: 'Click', command: 'toggle' },
                b: { label: 'Double-click', command: 'toggle' },
                c: { label: 'Hold', command: 'stop' },
                d: { label: 'Rotate left', command: 'off' },
                e: { label: 'Rotate right', command: 'on' },
                f: { label: 'Hold + rotate left', command: 'up' },
                g: { label: 'Hold + rotate right', command: 'down' },
                h: { label: 'Release hold', command: 'stop' },
                i: { label: 'Saturation move', command: 'toggle' }
            }
        }
        run(dom, `${script('common.js')}\n${script('rcm.js')}\n
            jsPlumbInstance = { draggable() {}, makeSource() {}, makeTarget() {}, repaintEverything() {} };
            wsManager.isConnected = () => true;
            wsManager.send = value => sentCommands.push(JSON.parse(value));
            State.zigbeeDevices = { [knobConfig.switch_id]: knobConfig };
            createSwitch(knobConfig.switch_id, 'tyua_knob', 9, 0, 0);`)
        const doc = dom.window.document
        assert.equal(doc.querySelectorAll('.zigbee-command').length, 9)
        const row = doc.getElementById('switch-281474976710656-btn-d')
        assert.equal(row.querySelector('.button-name').textContent, 'Rotate left')
        const select = row.querySelector('.zigbee-command')
        assert.equal(select.value, 'off')
        assert.deepEqual([...select.options].map(option => option.value), ['toggle', 'on', 'off', 'up', 'down', 'stop'])
        assert.equal(row.querySelector('.button-type-toggle').hidden, true)
        assert.equal(doc.querySelector('.update-btn-switch').hidden, true)
        select.value = 'up'
        select.dispatchEvent(new dom.window.Event('change'))
        assert.equal(select.disabled, true)
        assert.equal(JSON.stringify(dom.window.sentCommands), JSON.stringify([
            { type: 'zigbee_command', switch_id: 281474976710656, button_id: 'd', command: 'up' }
        ]))
        run(dom, `knobConfig.buttons.d.command = 'up'; applyZigbeeControls(knobConfig)`)
        assert.equal(select.value, 'up')
        assert.equal(select.disabled, false)
        assert.equal(doc.querySelectorAll('.zigbee-command').length, 9)
        run(dom, `editDeviceName('switch', knobConfig.switch_id)`)
        assert.equal(doc.getElementById('editButtonNumber').disabled, true)
        run(dom, `createSwitch(222, 'Wall switch', 3, 0, 0); editDeviceName('switch', 222)`)
        assert.equal(doc.getElementById('editButtonNumber').disabled, false)
        assert.equal(doc.getElementById('switch-222').querySelectorAll('.zigbee-command').length, 0)
        run(dom, `wsManager.isConnected = () => false`)
        select.value = 'down'
        select.dispatchEvent(new dom.window.Event('change'))
        assert.equal(select.value, 'up')
        assert.equal(dom.window.sentCommands.length, 1)
    } finally { dom.window.close() }
})

test('RCM renders discovered Zigbee sensors and keeps command selectors scoped to each device', () => {
    const dom = page(fs.readFileSync(path.join(root, 'static/rcm.html'), 'utf8'))
    try {
        dom.window.jsPlumb = { ready() {} }
        dom.window.sentCommands = []
        dom.window.devices = {
            281474976710670: { switch_id: 281474976710670, topic: 'zigbee2mqtt/kitchen/sensor', buttons: {} },
            281474976710671: { switch_id: 281474976710671, topic: 'zigbee2mqtt/first', buttons: { a: { label: 'Click', command: 'on' } } },
            281474976710672: { switch_id: 281474976710672, topic: 'zigbee2mqtt/second', buttons: { a: { label: 'Click', command: 'off' } } }
        }
        run(dom, `${script('common.js')}\n${script('rcm.js')}\n
            jsPlumbInstance = { draggable() {}, makeSource() {}, makeTarget() {}, repaintEverything() {} };
            wsManager.isConnected = () => true;
            wsManager.send = value => sentCommands.push(JSON.parse(value));
            State.zigbeeDevices = devices;
            State.zigbeeStates[281474976710670] = { temperature: 21.5, battery: 90 };
            createSwitch(281474976710670, 'kitchen/sensor', 0, 0, 0);
            createSwitch(281474976710671, 'first', 1, 0, 0);
            createSwitch(281474976710672, 'second', 1, 0, 0);`)
        const doc = dom.window.document
        assert.equal(doc.querySelectorAll('.zigbee-device').length, 3)
        const sensor = doc.getElementById('switch-281474976710670')
        assert.equal(sensor.querySelectorAll('.button-item').length, 0)
        assert.match(sensor.querySelector('.zigbee-details').textContent, /temperature: 21.5/)
        const first = doc.getElementById('switch-281474976710671').querySelector('.zigbee-command')
        const second = doc.getElementById('switch-281474976710672').querySelector('.zigbee-command')
        assert.equal(first.value, 'on')
        assert.equal(second.value, 'off')
        second.value = 'toggle'
        second.dispatchEvent(new dom.window.Event('change'))
        assert.equal(JSON.stringify(dom.window.sentCommands), JSON.stringify([
            { type: 'zigbee_command', switch_id: 281474976710672, button_id: 'a', command: 'toggle' }
        ]))
        assert.equal(first.value, 'on')
    } finally { dom.window.close() }
})


test('RCM repeated refreshes bind one delete handler and coalesce pending double-clicks', async () => {
    const dom = rcmPage()
    try {
        run(dom, `createSwitch(9, 'Switch', 3, 0, 0); createRelay(1, 'Relay', {}, 0, 0);
            createConnection(9, 'a', 1, 'a');
            const posts = [], messages = [];
            let finishRemoval;
            postForm = (...args) => { posts.push(args); return new Promise(resolve => { finishRemoval = resolve }) };
            wsManager.send = message => messages.push(JSON.parse(message));
            for (let i = 0; i < 5; i++) bindJsPlumbEvents();`)
        assert.equal(run(dom, 'eventListeners.dblclick.length'), 1)
        const first = run(dom, `handlers.dblclick(connections[9].a[0].connection, {preventDefault() {}})`)
        const second = run(dom, `handlers.dblclick(connections[9].a[0].connection, {preventDefault() {}})`)
        assert.equal(run(dom, 'posts.length'), 1)
        assert.equal(run(dom, 'posts[0][2]'), false)
        run(dom, 'finishRemoval({status: "Connection removed"})')
        await Promise.all([first, second])
        assert.equal(run(dom, 'connections[9].a.length'), 0)
        assert.equal(run(dom, 'liveConnections.length'), 0)
        assert.equal(run(dom, 'connectionLookupMap["relay-1"].length'), 0)
        assert.equal(run(dom, 'messages.filter(msg => msg.type === "update").length'), 1)
        assert.equal(dom.window.document.querySelectorAll('.device-box').length, 2)
    } finally { dom.window.close() }
})

test('RCM overlapping loads ignore stale HTTP responses and deferred connection rendering', async () => {
    const dom = rcmPage()
    try {
        run(dom, `const timers = [], responses = [];
            window.setTimeout = callback => { timers.push(callback); return timers.length };
            const configuration = {
                '/lights/get_relays': {1: ['Relay', 8]},
                '/lights/get_switches': {9: ['Switch', 3]},
                '/lights/get_connections': {9: {a: [[1, 'a']]}},
                '/lights/get_outputs': {}, '/lights/get_all_buttons': {},
                '/lights/get_blind_pairs': {}, '/lights/get_zigbee_devices': {},
                '/lights/get_presence_sensors': {}, '/lights/get_presence_schedules': {schedules: {}}
            };
            fetchAPI = path => new Promise(resolve => responses.push(() => resolve(configuration[path])));
            const firstLoad = loadConfiguration(), secondLoad = loadConfiguration();
            responses.splice(Object.keys(configuration).length).forEach(resolve => resolve());`)
        await run(dom, 'secondLoad')
        run(dom, 'responses.splice(0).forEach(resolve => resolve())')
        await run(dom, 'firstLoad')
        assert.equal(dom.window.document.querySelectorAll('.device-box').length, 2)
        run(dom, 'timers.splice(0).forEach(callback => callback())')
        assert.equal(run(dom, 'liveConnections.length'), 1)
        // A completed HTTP load can still have stale deferred rendering queued.
        run(dom, 'const thirdLoad = loadConfiguration(); responses.splice(0).forEach(resolve => resolve())')
        await run(dom, 'thirdLoad')
        run(dom, 'const fourthLoad = loadConfiguration(); responses.splice(0).forEach(resolve => resolve())')
        await run(dom, 'fourthLoad')
        run(dom, 'timers.splice(0).forEach(callback => callback())')
        assert.equal(dom.window.document.querySelectorAll('.device-box').length, 2)
        assert.equal(run(dom, 'liveConnections.length'), 1)
        assert.equal(run(dom, 'isLoadingConnections'), false)
    } finally { dom.window.close() }
})

test('RCM highlights only direct live neighbours, ignoring stale lookup entries and shared targets', async () => {
    const dom = rcmPage()
    try {
        run(dom, `for (const id of [1, 2, 3]) createRelay(id, 'Relay', {}, 0, 0);
            for (const id of [8, 9]) createSwitch(id, 'Switch', 3, 0, 0);
            createConnection(8, 'a', 1, 'a'); createConnection(8, 'b', 2, 'b');
            createConnection(9, 'a', 1, 'a'); createConnection(9, 'b', 3, 'b');
            const unrelated = connections[9].b[0].connection;
            connectionLookupMap['switch-8'].push(unrelated);
            highlightDevice('switch-8');`)
        const highlighted = () => [...dom.window.document.querySelectorAll('.device-box.highlighted')].map(el => el.id).sort()
        assert.deepEqual(highlighted(), ['relay-1', 'relay-2', 'switch-8'])
        assert.equal(run(dom, 'unrelated.canvas.classList.contains("highlighted")'), false)
        assert.equal(run(dom, 'connections[9].a[0].connection.canvas.classList.contains("highlighted")'), false)
        run(dom, `highlightDevice('relay-1')`)
        assert.deepEqual(highlighted(), ['relay-1', 'switch-8', 'switch-9'])
        assert.equal(run(dom, 'connections[8].b[0].connection.canvas.classList.contains("highlighted")'), false)
        run(dom, `postForm = async () => ({status: 'Connection removed'})`)
        await run(dom, 'removeConnection(connections[9].a[0].connection)')
        assert.deepEqual(highlighted(), ['relay-1', 'switch-8'])
        run(dom, `highlightDevice('relay-3'); highlightDevice('switch-8')`)
        assert.deepEqual(highlighted(), ['relay-1', 'relay-2', 'switch-8'])
    } finally { dom.window.close() }
})

test('RCM unlink-all includes hidden routes and removes only connections incident to the chosen card', async () => {
    for (const deviceId of ['switch-8', 'relay-1']) {
        const dom = rcmPage()
        try {
            run(dom, `for (const id of [1, 2]) createRelay(id, 'Relay', {}, 0, 0);
                for (const id of [8, 9]) createSwitch(id, 'Switch', 3, 0, 0);
                createConnection(8, 'a', 1, 'a'); createConnection(8, 'b', 2, 'b');
                createConnection(9, 'a', 1, 'a'); createConnection(9, 'b', 2, 'b');
                hideButton(8, 'a'); hideDevice(1, 'relay');
                appSettings.confirmBeforeDelete = false;
                const posts = [], messages = [];
                postForm = async (...args) => { posts.push(args); return {status: 'Connection removed'} };
                wsManager.send = message => messages.push(JSON.parse(message));`)
            await run(dom, `removeAllDeviceConnections('${deviceId}')`)
            assert.equal(run(dom, 'posts.length'), 2)
            assert.equal(run(dom, 'posts.every(args => args[2] === false)'), true)
            assert.equal(run(dom, 'liveConnections.length'), 2)
            assert.equal(run(dom, 'messages.filter(msg => msg.type === "update").length'), 1)
            assert.equal(dom.window.document.querySelectorAll('.device-box').length, 4)
            assert.equal(run(dom, `liveConnections.every(conn => ${deviceId.startsWith('switch')
                ? 'conn.sourceId.startsWith("switch-9-")' : 'conn.targetId.startsWith("relay-2-")'})`), true)
        } finally { dom.window.close() }
    }
})

test('RCM unlink-all respects cancellation and retains routes that fail to delete', async () => {
    const dom = rcmPage()
    try {
        run(dom, `createRelay(1, 'Relay', {}, 0, 0); createSwitch(9, 'Switch', 3, 0, 0);
            createConnection(9, 'a', 1, 'a'); createConnection(9, 'b', 1, 'b');
            const posts = [];
            postForm = async (...args) => { posts.push(args); return args[1].button_id === 'a' ? {} : null };
            window.confirm = () => false;`)
        await run(dom, `removeAllDeviceConnections('switch-9')`)
        assert.equal(run(dom, 'posts.length'), 0)
        assert.equal(run(dom, 'liveConnections.length'), 2)
        run(dom, 'window.confirm = () => true')
        await run(dom, `removeAllDeviceConnections('switch-9')`)
        assert.equal(run(dom, 'liveConnections.length'), 1)
        assert.equal(run(dom, 'connections[9].b.length'), 1)
        assert.equal(run(dom, 'connections[9].a.length'), 0)
        assert.equal(dom.window.document.querySelector('#switch-9 .disconnect-btn').disabled, false)
        assert.match(dom.window.document.getElementById('rcm-toast').textContent, /Removed 1 of 2/)
    } finally { dom.window.close() }
})


test("RCM keeps unrelated lines dimmed across zoom redraws and Firefox's delayed repaint", () => {
    for (const userAgent of ['Chrome', 'Firefox']) {
        const dom = rcmPage({ userAgent })
        try {
            const style = dom.window.document.createElement('style')
            style.textContent = fs.readFileSync(path.join(root, 'static/styles/rcm.css'), 'utf8')
            dom.window.document.head.appendChild(style)
            run(dom, `const timers = [];
                let now = 1000; Date.now = () => now += 100;
                window.setTimeout = callback => { timers.push(callback); return timers.length };
                window.requestAnimationFrame = callback => { callback(); return 0 };
                window.cancelAnimationFrame = () => {};
                jsPlumbInstance.repaintEverything = () => {
                    // jsPlumb rewrites connector positioning styles during redraw.
                    liveConnections.forEach(conn => { conn.canvas.style.cssText = 'position: absolute; left: 10px' });
                };
                jsPlumbInstance.setSuspendDrawing = (suspend, repaint) => {
                    if (!suspend && repaint) jsPlumbInstance.repaintEverything();
                };
                createRelay(1, 'Relay', {}, 0, 0); createRelay(2, 'Relay', {}, 0, 0);
                createSwitch(8, 'Switch', 3, 0, 0); createSwitch(9, 'Switch', 3, 0, 0);
                createConnection(8, 'a', 1, 'a'); createConnection(9, 'a', 1, 'a');
                createConnection(9, 'b', 2, 'b');
                // Cover both jsPlumb connector class names.
                liveConnections[2].canvas.className = '_jsPlumb_connector';
                initPanning(); timers.splice(0).forEach(callback => callback());
                highlightDevice('switch-8');`)
            const assertSelection = () => {
                const lines = run(dom, 'liveConnections.map(conn => conn.canvas)')
                assert.equal(dom.window.getComputedStyle(lines[0]).opacity || '1', '1')
                assert.equal(dom.window.getComputedStyle(lines[1]).opacity, '0.2')
                assert.equal(dom.window.getComputedStyle(lines[2]).opacity, '0.2')
                assert.equal(run(dom, 'highlightedDevice'), 'switch-8')
            }
            assertSelection()
            for (const action of ['zoomIn()', 'zoomOut()', 'resetZoom()', 'zoomAtPoint(1.1, 100, 100)']) {
                run(dom, `${action}; timers.splice(0).forEach(callback => callback())`)
                assertSelection()
            }
            const wrapper = dom.window.document.getElementById('canvas-wrapper')
            for (const deltaY of [-100, 100]) {
                wrapper.dispatchEvent(new dom.window.WheelEvent('wheel', { deltaY, clientX: 100, clientY: 100 }))
                run(dom, 'timers.splice(0).forEach(callback => callback())')
                assertSelection()
            }
            // Pinch zoom resumes drawing through the same path as wheel zoom.
            run(dom, `suspendJsPlumb(); isPinching = true; zoomAtPoint(1.2, 100, 100);
                isPinching = false; resumeJsPlumb(); timers.splice(0).forEach(callback => callback())`)
            assertSelection()
            run(dom, `highlightDevice('relay-2'); syncJsPlumb()`)
            assert.equal(dom.window.getComputedStyle(run(dom, 'liveConnections[0].canvas')).opacity, '0.2')
            assert.equal(dom.window.getComputedStyle(run(dom, 'liveConnections[2].canvas')).opacity || '1', '1')
            run(dom, 'clearHighlights(); syncJsPlumb()')
            assert.equal(wrapper.querySelector('#canvas').classList.contains('has-highlighted-device'), false)
            for (const line of run(dom, 'liveConnections.map(conn => conn.canvas)')) {
                assert.equal(dom.window.getComputedStyle(line).opacity || '1', '1')
            }
        } finally { dom.window.close() }
    }
})

test('RCM keeps the selection through drags and follows connections drawn or removed while selected', async () => {
    const dom = rcmPage()
    try {
        run(dom, `for (const id of [1, 2]) createRelay(id, 'Relay', {}, 0, 0);
            createSwitch(8, 'Switch', 3, 0, 0);
            createConnection(8, 'a', 1, 'a');
            postForm = async () => ({success: true});
            initPanning(); bindJsPlumbEvents(); highlightDevice('switch-8');`)
        const doc = dom.window.document
        const wrapper = doc.getElementById('canvas-wrapper')
        const mouse = (type, target, x, y) => target.dispatchEvent(
            new dom.window.MouseEvent(type, { bubbles: true, clientX: x, clientY: y }))
        const highlighted = () => [...doc.querySelectorAll('.device-box.highlighted')].map(el => el.id).sort()

        // Panning ends with a click on the empty canvas; it must not clear the selection.
        mouse('mousedown', wrapper, 10, 10)
        mouse('mousemove', doc, 220, 160)
        mouse('mouseup', doc, 220, 160)
        mouse('click', wrapper, 220, 160)
        assert.equal(run(dom, 'highlightedDevice'), 'switch-8')

        // Moving another card does not steal the selection either.
        const relay = doc.getElementById('relay-2')
        mouse('mousedown', relay, 300, 300)
        mouse('click', relay, 340, 330)
        assert.equal(run(dom, 'highlightedDevice'), 'switch-8')

        // A connection drawn from the selected switch adds its relay to the selection.
        await run(dom, `const drawn = jsPlumbInstance.connect({source: document.getElementById('switch-8-btn-b'),
                target: document.getElementById('relay-2-output-c'), paintStyle: {}});
            handlers.connection({connection: drawn}, {})`)
        assert.deepEqual(highlighted(), ['relay-1', 'relay-2', 'switch-8'])
        assert.equal(run(dom, 'connections[8].b[0].connection.canvas.classList.contains("highlighted")'), true)
        // New lines stand out from the selection's red and are never dimmed.
        assert.equal(run(dom, 'connections[8].b[0].connection.canvas.classList.contains("new-connection")'), true)
        assert.equal(run(dom, 'connections[8].b[0].connection.paintStyle.stroke'), '#22c55e')
        assert.equal(run(dom, 'connections[8].a[0].connection.paintStyle.stroke'), '#ef4444')

        // A line drawn between other devices while selected stays fully visible too.
        run(dom, 'createSwitch(9, "Other", 3, 0, 0)')
        await run(dom, `const other = jsPlumbInstance.connect({source: document.getElementById('switch-9-btn-a'),
                target: document.getElementById('relay-1-output-d'), paintStyle: {}});
            handlers.connection({connection: other}, {})`)
        assert.equal(run(dom, 'connections[9].a[0].connection.canvas.classList.contains("highlighted")'), true)
        assert.equal(run(dom, 'connections[9].a[0].connection.paintStyle.stroke'), '#22c55e')
        assert.equal(run(dom, 'highlightedDevice'), 'switch-8')

        // The line being dragged is marked as soon as the drag starts.
        run(dom, `const dragging = jsPlumbInstance.connect({source: document.getElementById('switch-8-btn-c'),
                target: document.getElementById('relay-2-output-e'), paintStyle: {}});
            handlers.connectionDrag(dragging)`)
        assert.equal(run(dom, 'dragging.canvas.classList.contains("new-connection")'), true)
        assert.equal(run(dom, 'dragging.paintStyle.stroke'), '#22c55e')
        run(dom, 'jsPlumbInstance.deleteConnection(dragging)')

        // Removing it drops the relay again.
        await run(dom, 'removeConnection(connections[8].b[0].connection)')
        assert.deepEqual(highlighted(), ['relay-1', 'switch-8'])

        // A plain click on empty canvas still clears.
        mouse('mousedown', wrapper, 50, 50)
        mouse('click', wrapper, 50, 50)
        assert.equal(run(dom, 'highlightedDevice'), null)
        assert.equal(run(dom, 'connections[9].a[0].connection.canvas.classList.contains("new-connection")'), false)
        assert.equal(run(dom, 'State.newConnections.size'), 0)
    } finally { dom.window.close() }
})

test('RCM demo fallback draws every demo connection and shows every demo button', async () => {
    const dom = rcmPage()
    try {
        run(dom, `const pendingTimers = [];
            window.setTimeout = callback => { pendingTimers.push(callback); return 0 };
            fetchAPI = async () => null;`)
        await run(dom, 'loadConfiguration()')
        run(dom, 'pendingTimers.splice(0).forEach(callback => callback())')
        const doc = dom.window.document
        const expected = run(dom, `Object.values(getDemoConnections())
            .flatMap(buttons => Object.values(buttons)).reduce((sum, targets) => sum + targets.length, 0)`)
        assert.equal(run(dom, 'jsPlumbInstance.getAllConnections().length'), expected)
        assert.equal(doc.querySelector('.output-name-1-e').textContent, 'Przedpokój')
        const buttons = [...doc.querySelectorAll('.switch-box .button-item')]
        assert.equal(buttons.length, 17)
        assert.ok(buttons.every(button => button.style.display === 'flex'))
    } finally { dom.window.close() }
})

test('RCM gateway toggle shows a pending state until the switch confirms, times out or is rejected', () => {
    const dom = rcmPage()
    try {
        run(dom, `const timers = new Map(); let timerId = 0;
            window.setTimeout = (callback, delay) => { timers.set(++timerId, {callback, delay}); return timerId };
            window.clearTimeout = id => timers.delete(id);
            const toasts = []; showToast = (message, isError = false) => toasts.push({message, isError});
            wsManager.isConnected = () => true; const sent = []; wsManager.send = data => { sent.push(JSON.parse(data)); return true };
            online_switches = new Set([222, 333]);
            State.deviceRoles = {222: {type: 'switch', gateway: false}, 333: {type: 'remote'}};
            createSwitch(222, 'Hall', 3, 0, 0); createSwitch(333, 'Remote', 2, 0, 0); createSwitch(444, 'Unknown', 2, 0, 0);`)
        const doc = dom.window.document
        const button = doc.querySelector('[data-gateway-switch="222"]')
        // Card widths follow capability: gateway switches are widest, others thinner.
        assert.ok(doc.getElementById('switch-222').classList.contains('gateway-capable'))
        assert.ok(doc.getElementById('switch-333').classList.contains('remote-switch'))
        assert.equal(doc.getElementById('switch-444').classList.contains('gateway-capable'), false)
        assert.equal(doc.getElementById('switch-444').classList.contains('remote-switch'), false)

        run(dom, 'toggleGateway(222)')
        assert.equal(run(dom, 'JSON.stringify(sent)'), JSON.stringify([{type: 'gateway_mode', device_id: 222, enabled: true}]))
        assert.equal(button.textContent, '📡 Enabling…')
        assert.equal(button.disabled, true)
        assert.ok(button.classList.contains('pending'))
        assert.equal(run(dom, '[...timers.values()][0].delay'), 45000)
        // Clicking again while pending sends nothing.
        run(dom, 'toggleGateway(222)')
        assert.equal(run(dom, 'sent.length'), 1)
        // A status report that still says "off" keeps waiting.
        const status = gateway => run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'online_status',
            online_relays: [], online_switches: [222, 333], up_to_date_devices: {}, devices_rssi: {}, ping_times: {},
            device_roles: {222: {type: 'switch', gateway: ${gateway}}, 333: {type: 'remote'}}})})`)
        status(false)
        assert.equal(button.textContent, '📡 Enabling…')
        status(true)
        assert.equal(button.textContent, '📡 Gateway: on')
        assert.equal(button.disabled, false)
        assert.equal(run(dom, 'timers.size'), 0)
        assert.match(run(dom, 'toasts.at(-1).message'), /Gateway enabled on Hall/)

        // No confirmation: the timeout restores the control and explains why.
        run(dom, 'toggleGateway(222)')
        assert.equal(button.textContent, '📡 Disabling…')
        run(dom, '[...timers.values()][0].callback()')
        assert.equal(button.textContent, '📡 Gateway: on')
        assert.equal(run(dom, 'toasts.at(-1).isError'), true)

        // A server rejection clears the pending state immediately.
        run(dom, `toggleGateway(222); wsManager.onMessage({data: JSON.stringify({type: 'error',
            message: 'Gateway mode requires an online normal switch'})})`)
        assert.equal(button.textContent, '📡 Gateway: on')
        assert.equal(run(dom, 'State.gatewayPending.size'), 0)

        // Reported device types are cached so card widths are right on the next load.
        assert.deepEqual(JSON.parse(dom.window.localStorage.getItem('rcm_device_roles')),
            {222: {type: 'switch'}, 333: {type: 'remote'}})
    } finally { dom.window.close() }
})

test('RCM presence sensors show fixed inputs, occupancy and a fixed input count', () => {
    const dom = rcmPage()
    try {
        run(dom, `var sensorConfig = ${JSON.stringify(SENSOR_CONFIG)};
            State.presenceSensors = {555: sensorConfig};
            online_switches = new Set([555]);
            State.presenceStates = {555: {presence: 1, distance: 140, radar: 1}};
            createSwitch(555, 'Hall sensor', 3, 0, 0);
            createSwitch(222, 'Wall switch', 3, 0, 0);`)
        const doc = dom.window.document
        const card = doc.getElementById('switch-555')
        assert.ok(card.classList.contains('presence-sensor'))
        assert.equal(doc.getElementById('switch-222').classList.contains('presence-sensor'), false)
        assert.deepEqual([...card.querySelectorAll('.button-name')].map(el => el.textContent),
            ['Presence: on / off', 'Arrival: on', 'Departure: off'])
        assert.ok([...card.querySelectorAll('.button-type-toggle')].every(el => el.hidden))
        assert.equal(doc.getElementById('switch-222').querySelector('.button-type-toggle').hidden, false)
        assert.equal(card.querySelector('.presence-badge').textContent, 'mmWave')
        const status = card.querySelector('[data-presence-status="555"]')
        assert.equal(status.textContent, '● Occupied · 1.40 m')
        assert.equal(status.dataset.state, 'occupied')
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_state', switch_id: 555,
            presence: 0, distance: 140, radar: 1})})`)
        assert.equal(status.textContent, '○ Clear')
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'online_status', online_relays: [],
            online_switches: [555], up_to_date_devices: {}, devices_rssi: {}, ping_times: {},
            device_roles: {555: {type: 'presence'}}, presence_states: {555: {presence: 0, distance: 0, radar: 0}}})})`)
        assert.equal(status.textContent, '⚠ Radar not responding')
        assert.equal(status.dataset.state, 'fault')
        assert.equal(card.querySelector('[data-gateway-switch="555"]').hidden, true)
        run(dom, 'online_switches = new Set(); updatePresenceStatus(555)')
        assert.equal(status.textContent, 'Offline')
        run(dom, `editDeviceName('switch', 555)`)
        assert.equal(doc.getElementById('editButtonNumber').disabled, true)
        run(dom, `editDeviceName('switch', 222)`)
        assert.equal(doc.getElementById('editButtonNumber').disabled, false)
        run(dom, `buttonTypes = {555: {a: 2, b: 3, c: 4}}; toggleButtonType(555, 'a')`)
        assert.equal(run(dom, 'buttonTypes[555].a'), 2)
    } finally { dom.window.close() }
})

test('RCM presence dialog edits the radar settings and waits for the sensor to confirm them', () => {
    const dom = rcmPage()
    try {
        dom.window.hostileName = payloads[0]
        run(dom, `var sensorConfig = ${JSON.stringify(SENSOR_CONFIG)};
            const timers = new Map(); let timerId = 0;
            window.setTimeout = (callback, delay) => { timers.set(++timerId, {callback, delay}); return timerId };
            window.clearTimeout = id => timers.delete(id);
            const toasts = []; showToast = (message, isError = false) => toasts.push({message, isError});
            wsManager.isConnected = () => true;
            const sent = []; wsManager.send = data => { sent.push(JSON.parse(data)); return true };
            window.confirm = () => true;
            State.presenceSensors = {555: sensorConfig};
            online_switches = new Set([555]);
            createSwitch(555, hostileName, 3, 0, 0);`)
        const doc = dom.window.document
        const Event = dom.window.Event
        doc.querySelector('#switch-555 .presence-config-btn').click()
        assert.ok(doc.getElementById('presenceModal').classList.contains('active'))
        assert.equal(doc.getElementById('presenceModalTitle').textContent, `${payloads[0]} · presence sensor`)
        assert.equal(doc.querySelectorAll('[onerror], [onfocus], [autofocus]').length, 0)
        assert.equal(doc.getElementById('presenceMaxGate').value, '12')
        assert.equal(doc.getElementById('presenceMaxGateValue').textContent, '8.4 m · gate 12')
        assert.equal(doc.getElementById('presenceDelay').value, '30')
        const input = (gate, key) => doc.querySelector(`#presenceGates input[data-gate="${gate}"][data-key="${key}"]`)
        assert.equal(input(0, 'trigger').value, '47.78')
        assert.equal(input(15, 'hold').value, '20.00')

        // Gates beyond the range are dimmed; dragging one end past the other moves both.
        const minGate = doc.getElementById('presenceMinGate')
        const maxGate = doc.getElementById('presenceMaxGate')
        maxGate.value = '6'
        maxGate.dispatchEvent(new Event('input'))
        assert.equal(doc.getElementById('presenceMaxGateValue').textContent, '4.2 m · gate 6')
        assert.ok(doc.querySelector('#presenceGates .presence-gate[data-gate="7"]').classList.contains('outside'))
        assert.equal(doc.querySelector('#presenceGates .presence-gate[data-gate="6"]').classList.contains('outside'), false)
        minGate.value = '9'
        minGate.dispatchEvent(new Event('input'))
        assert.equal(maxGate.value, '9')
        minGate.value = '2'
        minGate.dispatchEvent(new Event('input'))
        const delay = doc.getElementById('presenceDelay')
        delay.value = '90'
        delay.dispatchEvent(new Event('input'))
        assert.equal(doc.getElementById('presenceDelayValue').textContent, '1 min 30 s')
        doc.getElementById('presenceLed').click()
        input(3, 'trigger').value = '30'
        input(3, 'trigger').dispatchEvent(new Event('change', { bubbles: true }))
        input(4, 'hold').value = '-5'
        input(4, 'hold').dispatchEvent(new Event('change', { bubbles: true }))
        assert.equal(input(4, 'hold').value, '24.77')
        doc.querySelector('[data-presence-shift="1"]').click()
        assert.equal(input(0, 'trigger').value, '48.78')
        assert.match(doc.getElementById('presenceSummary').textContent, /unsaved changes/)

        const shifted = raw => Math.round(10 ** ((10 * Math.log10(raw) + 1) / 10))
        const trigger = SENSOR_CONFIG.trigger.map(shifted)
        trigger[3] = shifted(1000)
        const hold = SENSOR_CONFIG.hold.map(shifted)
        doc.getElementById('presenceApply').click()
        assert.deepEqual(JSON.parse(run(dom, 'JSON.stringify(sent)')), [{ type: 'presence_command', switch_id: 555,
            action: 'apply', config: { min: 2, max: 9, delay: 90, trigger, hold, led: true } }])
        const apply = doc.getElementById('presenceApply')
        assert.equal(apply.textContent, 'Applying…')
        assert.equal(apply.disabled, true)
        assert.equal(input(0, 'trigger').disabled, true)
        assert.equal(run(dom, '[...timers.values()][0].delay'), 15000)

        // The radar's read-back values become the dialog's values.
        dom.window.stored = JSON.stringify({ ...SENSOR_CONFIG, min: 2, max: 9, delay: 90, led: true, trigger, hold })
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_config', switch_id: 555,
            config: JSON.parse(stored)})})`)
        assert.equal(apply.textContent, 'Apply')
        assert.equal(apply.disabled, false)
        assert.equal(input(0, 'trigger').disabled, false)
        assert.equal(run(dom, 'timers.size'), 0)
        assert.match(run(dom, 'toasts.at(-1).message'), /^Sensor settings saved on /)
        assert.doesNotMatch(doc.getElementById('presenceSummary').textContent, /unsaved/)

        // No report: the request times out with an explanation.
        doc.getElementById('presenceReload').click()
        assert.equal(run(dom, 'sent.at(-1).action'), 'read')
        assert.equal(doc.getElementById('presenceReload').disabled, true)
        run(dom, '[...timers.values()][0].callback()')
        assert.equal(run(dom, 'toasts.at(-1).isError'), true)
        assert.equal(doc.getElementById('presenceReload').disabled, false)
        // A server rejection clears the pending state at once.
        apply.click()
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'error', message: 'The presence sensor is offline'})})`)
        assert.equal(apply.disabled, false)
        assert.equal(run(dom, 'State.presencePending.size'), 0)
    } finally { dom.window.close() }
})

test('RCM presence live view renews while open, stops on close and counts calibration down', () => {
    const dom = rcmPage()
    try {
        run(dom, `var sensorConfig = ${JSON.stringify(SENSOR_CONFIG)};
            const intervals = [];
            window.setInterval = (callback, delay) => { intervals.push({callback, delay}); return intervals.length };
            window.clearInterval = () => {};
            const timers = new Map(); let timerId = 0;
            window.setTimeout = (callback, delay) => { timers.set(++timerId, {callback, delay}); return timerId };
            window.clearTimeout = id => timers.delete(id);
            wsManager.isConnected = () => true;
            const sent = []; wsManager.send = data => { sent.push(JSON.parse(data)); return true };
            let confirmText = ''; window.confirm = text => { confirmText = text; return true };
            State.presenceSensors = {555: sensorConfig};
            online_switches = new Set([555]);
            createSwitch(555, 'Hall sensor', 3, 0, 0);
            showPresenceModal(555);
            var energy = [65535, 0, 1000, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];`)
        const doc = dom.window.document
        const last = () => JSON.parse(run(dom, 'JSON.stringify(sent.at(-1))'))
        doc.getElementById('presenceLive').click()
        assert.deepEqual(last(), { type: 'presence_command', switch_id: 555, action: 'live', seconds: 60 })
        assert.equal(run(dom, 'intervals[0].delay'), 50000)
        run(dom, 'intervals[0].callback()')
        assert.equal(run(dom, 'sent.length'), 2)

        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_live', switch_id: 555, presence: 1,
            distance: 250, energy, calibrating: 0})})`)
        const bar = gate => doc.querySelector(`#presenceGates .presence-gate[data-gate="${gate}"] .presence-energy-bar`)
        assert.ok(bar(0).classList.contains('over-trigger'))
        assert.ok(bar(2).classList.contains('over-hold'))
        assert.equal(bar(5).classList.contains('over-hold') || bar(5).classList.contains('over-trigger'), false)
        assert.equal(doc.querySelector('#presenceGates .presence-gate[data-gate="2"] .presence-energy-value').textContent, '30.0')
        assert.equal(doc.querySelector('[data-presence-status="555"]').textContent, '● Occupied · 2.50 m')
        assert.equal(doc.getElementById('presenceRangeTarget').hidden, false)

        const calibrate = doc.getElementById('presenceCalibrate')
        calibrate.click()
        assert.match(run(dom, 'confirmText'), /Leave the detection area within 10 seconds/)
        assert.deepEqual(last(), { type: 'presence_command', switch_id: 555, action: 'calibrate', seconds: 20 })
        assert.equal(run(dom, '[...timers.values()][0].delay'), 45000)
        assert.equal(calibrate.textContent, 'Calibrating… 30 s')
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_live', switch_id: 555, presence: 0,
            distance: 0, energy, calibrating: 12})})`)
        assert.equal(calibrate.textContent, 'Calibrating… 12 s')
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_config', switch_id: 555, config: sensorConfig})})`)
        assert.equal(calibrate.textContent, 'Calibrate')

        run(dom, `closeModal('presenceModal')`)
        assert.deepEqual(last(), { type: 'presence_command', switch_id: 555, action: 'live', seconds: 0 })
        assert.equal(run(dom, 'State.presenceModal'), null)
    } finally { dom.window.close() }
})

test('RCM loads presence sensors with the configuration and waits for an unreported one', async () => {
    const dom = rcmPage()
    try {
        run(dom, `const pendingTimers = [];
            window.setTimeout = callback => { pendingTimers.push(callback); return 0 };
            fetchAPI = async path => ({
                '/lights/get_relays': {}, '/lights/get_outputs': {}, '/lights/get_connections': {},
                '/lights/get_switches': {9: ['Sensor', 3], 10: ['Switch', 2]},
                '/lights/get_all_buttons': {9: {a: 2, b: 3, c: 4}, 10: {a: 0, b: 1}},
                '/lights/get_blind_pairs': {}, '/lights/get_zigbee_devices': {},
                '/lights/get_presence_sensors': {9: null},
                // PostgreSQL returns JSONB keys in its own order.
                '/lights/get_presence_schedules': {schedules: {9: {to: {offset: -30, sun: 'sunrise'}, from: {time: '22:00'}}},
                    zone: 'CET', sunrise: '07:12', sunset: '15:48'}
            })[path];
            wsManager.isConnected = () => true;
            const sent = []; wsManager.send = data => { sent.push(JSON.parse(data)); return true };
            online_switches = new Set([9]);`)
        await run(dom, 'loadConfiguration()')
        run(dom, 'pendingTimers.splice(0).forEach(callback => callback())')
        const doc = dom.window.document
        assert.ok(doc.getElementById('switch-9').classList.contains('presence-sensor'))
        assert.equal(doc.getElementById('switch-10').classList.contains('presence-sensor'), false)
        assert.equal(doc.querySelector('[data-presence-status="9"]').textContent, 'Waiting for the sensor…')
        // Changing another switch re-sends the fixed presence types unchanged.
        run(dom, `toggleButtonType(10, 'a')`)
        const types = JSON.parse(run(dom, 'JSON.stringify(sent.filter(msg => msg.type === "button_types").at(-1))'))
        assert.deepEqual(types.data['9'], { a: 2, b: 3, c: 4 })
        assert.deepEqual(types.data['10'], { a: 1, b: 1 })
        // Until the sensor reports its settings, they can only be requested.
        run(dom, 'showPresenceModal(9)')
        assert.ok(doc.getElementById('presenceModal').classList.contains('presence-empty'))
        assert.equal(doc.getElementById('presenceApply').disabled, true)
        assert.equal(doc.getElementById('presenceCalibrate').disabled, true)
        assert.equal(doc.getElementById('presenceReload').disabled, false)
        assert.match(doc.getElementById('presenceSummary').textContent, /waiting for the sensor to report its settings/)
        // The schedule is Turbacz's, so it shows and can be edited meanwhile.
        const to = doc.querySelector('#presenceSchedule [data-end="to"]')
        assert.equal(doc.getElementById('presencePause').checked, true)
        assert.equal(doc.querySelector('#presenceSchedule [data-end="from"] [data-end-time]').value, '22:00')
        assert.equal(to.querySelector('[data-end-kind]').value, 'sunrise')
        assert.equal(to.querySelector('[data-end-offset]').value, '-30')
        assert.equal(doc.getElementById('presencePause').disabled, false)
        assert.equal(doc.getElementById('presenceScheduleSave').disabled, true)
        assert.doesNotMatch(doc.getElementById('presenceSummary').textContent, /unsaved schedule/)
        assert.equal(doc.getElementById('presenceSchedulePreview').textContent,
            'Paused every day from 22:00 to 30 min before sunrise. Today 22:00–06:42.')
        assert.equal(doc.getElementById('presenceScheduleZone').textContent, 'Times in CET')
    } finally { dom.window.close() }
})

test('RCM presence schedule pauses a sensor at times or around the sun and waits for Turbacz to store it', () => {
    const dom = rcmPage()
    try {
        dom.window.hostileName = payloads[0]
        run(dom, `var sensorConfig = ${JSON.stringify(SENSOR_CONFIG)};
            const timers = new Map(); let timerId = 0;
            window.setTimeout = (callback, delay) => { timers.set(++timerId, {callback, delay}); return timerId };
            window.clearTimeout = id => timers.delete(id);
            const toasts = []; showToast = (message, isError = false) => toasts.push({message, isError});
            wsManager.isConnected = () => true;
            const sent = []; wsManager.send = data => { sent.push(JSON.parse(data)); return true };
            State.presenceSensors = {555: sensorConfig};
            State.presenceSchedules = {555: null};
            State.presenceToday = {zone: 'CET', sunrise: '07:12', sunset: '15:48'};
            State.presenceStates = {555: {presence: 1, distance: 140, radar: 1, active: 1}};
            online_switches = new Set([555]);
            createSwitch(555, hostileName, 3, 0, 0);
            showPresenceModal(555);`)
        const doc = dom.window.document
        const Event = dom.window.Event
        const last = () => JSON.parse(run(dom, 'JSON.stringify(sent.at(-1))'))
        const end = key => doc.querySelector(`#presenceSchedule [data-end="${key}"]`)
        const preview = doc.getElementById('presenceSchedulePreview')
        const save = doc.getElementById('presenceScheduleSave')
        const set = (element, value, type = 'input') => {
            element.value = value
            element.dispatchEvent(new Event(type, { bubbles: true }))
        }
        assert.equal(preview.textContent, 'The sensor works at all hours.')
        assert.equal(doc.getElementById('presenceScheduleEnds').hidden, true)
        assert.equal(save.disabled, true)

        // A night pause by default; sun events show an offset instead of a time.
        doc.getElementById('presencePause').click()
        assert.equal(doc.getElementById('presenceScheduleEnds').hidden, false)
        assert.equal(preview.textContent, 'Paused every day from 23:00 to 06:00.')
        assert.match(doc.getElementById('presenceSummary').textContent, /unsaved schedule/)
        set(end('to').querySelector('[data-end-kind]'), 'sunrise', 'change')
        assert.equal(end('to').querySelector('[data-end-time]').hidden, true)
        assert.equal(end('to').querySelector('.presence-schedule-offset').hidden, false)
        set(end('to').querySelector('[data-end-offset]'), '30')
        assert.equal(preview.textContent, 'Paused every day from 23:00 to 30 min after sunrise. Today 23:00–07:42.')
        assert.equal(preview.classList.contains('warning'), false)
        save.click()
        assert.deepEqual(last(), { type: 'presence_schedule', switch_id: 555,
            schedule: { from: { time: '23:00' }, to: { sun: 'sunrise', offset: 30 } } })
        assert.equal(save.textContent, 'Saving…')
        assert.equal(save.disabled, true)
        assert.equal(doc.getElementById('presencePause').disabled, true)
        assert.equal(run(dom, '[...timers.values()].at(-1).delay'), 15000)

        // Turbacz confirms by broadcasting what it stored.
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_schedule', switch_id: 555,
            schedule: {to: {offset: 30, sun: 'sunrise'}, from: {time: '23:00'}}})})`)
        assert.equal(run(dom, 'toasts.at(-1).message'), `Schedule saved on ${payloads[0]}`)
        assert.equal(save.textContent, 'Save schedule')
        assert.equal(save.disabled, true)
        assert.equal(doc.getElementById('presencePause').disabled, false)
        assert.doesNotMatch(doc.getElementById('presenceSummary').textContent, /unsaved schedule/)

        // Enter in the schedule saves it, not the radar settings.
        const from = end('from').querySelector('[data-end-time]')
        set(from, '22:30')
        from.focus()
        from.dispatchEvent(new dom.window.KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
        assert.equal(last().type, 'presence_schedule')
        assert.deepEqual(last().schedule.from, { time: '22:30' })
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_schedule', switch_id: 555,
            schedule: {from: {time: '22:30'}, to: {sun: 'sunrise', offset: 30}}})})`)

        // A pause that ends when it starts is refused before it reaches Turbacz.
        set(end('to').querySelector('[data-end-kind]'), 'time', 'change')
        set(end('to').querySelector('[data-end-time]'), '22:30')
        assert.equal(preview.textContent, 'The pause must end at another time than it starts.')
        assert.ok(preview.classList.contains('warning'))
        assert.equal(save.disabled, true)
        set(end('to').querySelector('[data-end-time]'), '')
        assert.equal(preview.textContent, 'Enter the time the pause starts and ends.')

        // Removing the pause sends null; a request without an answer times out.
        doc.getElementById('presencePause').click()
        save.click()
        assert.equal(last().schedule, null)
        run(dom, '[...timers.values()].at(-1).callback()')
        assert.equal(run(dom, 'toasts.at(-1).isError'), true)
        assert.equal(save.disabled, false)
        save.click()
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_schedule', switch_id: 555, schedule: null})})`)
        assert.equal(run(dom, 'toasts.at(-1).message'), `Schedule removed from ${payloads[0]}`)
        assert.equal(run(dom, 'State.presenceSchedules[555]'), null)

        // Firmware that reports no schedule state cannot follow one.
        doc.getElementById('presencePause').click()
        run(dom, `State.presenceStates[555] = {presence: 0, distance: 0, radar: 1}; updatePresenceStatus(555)`)
        assert.match(preview.textContent, /firmware has no schedules/)
        assert.ok(preview.classList.contains('warning'))
        assert.equal(doc.querySelectorAll('[onerror], [onfocus], [autofocus]').length, 0)
    } finally { dom.window.close() }
})

test('RCM shows paused sensors and needs the home location for sun events', () => {
    const dom = rcmPage()
    try {
        run(dom, `var sensorConfig = ${JSON.stringify(SENSOR_CONFIG)};
            State.presenceSensors = {555: sensorConfig};
            State.presenceSchedules = {555: {from: {sun: 'sunrise', offset: 0}, to: {sun: 'sunset', offset: 0}}};
            State.presenceToday = {zone: 'UTC', sunrise: null, sunset: null};
            State.presenceStates = {555: {presence: 1, distance: 140, radar: 1, active: 0}};
            online_switches = new Set([555]);
            createSwitch(555, 'Hall sensor', 3, 0, 0);`)
        const doc = dom.window.document
        const status = doc.querySelector('[data-presence-status="555"]')
        assert.equal(status.textContent, '● Occupied · 1.40 m · ⏸ paused')
        assert.equal(status.dataset.paused, 'true')
        run(dom, `wsManager.onMessage({data: JSON.stringify({type: 'presence_state', switch_id: 555,
            presence: 0, distance: 0, radar: 1, active: 1})})`)
        assert.equal(status.textContent, '○ Clear')
        assert.equal(status.dataset.paused, 'false')

        // A saved sun schedule stays visible, but cannot be saved again without a location.
        run(dom, 'showPresenceModal(555)')
        const kind = doc.querySelector('#presenceSchedule [data-end="from"] [data-end-kind]')
        assert.equal(kind.value, 'sunrise')
        assert.equal(kind.querySelector('option[value="sunrise"]').disabled, false)
        assert.equal(kind.querySelector('option[value="sunset"]').disabled, true)
        const preview = doc.getElementById('presenceSchedulePreview')
        assert.match(preview.textContent, /\[location\] in turbacz\.toml/)
        assert.equal(doc.getElementById('presenceScheduleSave').disabled, true)

        // A pause from sunset to a time is skipped on days the sun sets later.
        run(dom, `State.presenceToday = {zone: 'CEST', sunrise: '04:35', sunset: '21:40'};
            closeModal('presenceModal');
            State.presenceSchedules = {555: {from: {sun: 'sunset', offset: 0}, to: {time: '21:00'}}};
            showPresenceModal(555);`)
        assert.equal(preview.textContent, 'Paused every day from sunset to 21:00. No pause today.')
        assert.equal(doc.getElementById('presenceScheduleZone').textContent, 'Times in CEST')
    } finally { dom.window.close() }
})
