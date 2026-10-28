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
                '/lights/get_blind_pairs': {}, '/lights/get_zigbee_devices': {}
            };
            fetchAPI = path => new Promise(resolve => responses.push(() => resolve(configuration[path])));
            const firstLoad = loadConfiguration(), secondLoad = loadConfiguration();
            responses.splice(7).forEach(resolve => resolve());`)
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
