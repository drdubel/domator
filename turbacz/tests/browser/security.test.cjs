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
        assert.equal(button.textContent, 'Gateway: on')
        assert.match(button.title, /02:00:00:00:00:01/)
        run(dom, `State.deviceRoles[222] = {type: 'remote'}; updateGatewayControls()`)
        assert.equal(button.hidden, true)
        assert.equal(dom.window.document.querySelector('[data-remote-switch="222"]').hidden, false)
        assert.ok(dom.window.document.getElementById('switch-222-btn-g'))
    } finally { dom.window.close() }
})

function rcmPage() {
    const dom = page(fs.readFileSync(path.join(root, 'static/rcm.html'), 'utf8'))
    dom.window.jsPlumb = { ready() {} }
    run(dom, `${script('common.js')}\n${script('rcm.js')}\n
        canvasElement = document.getElementById('canvas');
        zoomLevelElement = document.getElementById('zoomLevel');
        const handlers = {};
        jsPlumbInstance = {
            draggable() {}, makeSource() {}, makeTarget() {}, repaintEverything() {},
            revalidate() {}, setZoom() {}, batch(callback) { callback() },
            deleteEveryConnection() {}, deleteEveryEndpoint() {}, deleteConnection() {},
            unbind() {}, bind(event, callback) { handlers[event] = callback },
            connect({source, target}) {
                return { sourceId: source.id, targetId: target.id, visible: true,
                    setVisible(value) { this.visible = value } };
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
        for (const style of ['columns', 'grid', 'paired', 'columns']) {
            run(dom, 'cycleDeviceArrangement()')
            assert.equal(dom.window.localStorage.getItem('rcm_arrangement_style'), style)
            const switches = [...doc.querySelectorAll('.switch-box')]
            const relays = [...doc.querySelectorAll('.relay-box')]
            assert.ok(Math.max(...switches.map(el => parseFloat(el.style.left) + el.offsetWidth))
                < Math.min(...relays.map(el => parseFloat(el.style.left))))
            const cards = [...switches, ...relays]
            cards.forEach((a, i) => cards.slice(i + 1).forEach(b => {
                const x = el => parseFloat(el.style.left), y = el => parseFloat(el.style.top)
                assert.ok(x(a) + a.offsetWidth <= x(b) || x(b) + b.offsetWidth <= x(a)
                    || y(a) + a.offsetHeight <= y(b) || y(b) + b.offsetHeight <= y(a))
            }))
            if (style === 'paired') switches.forEach(sw => {
                assert.equal(sw.style.top, doc.getElementById(sw.id.replace('switch', 'relay')).style.top)
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
        for (const style of ['columns', 'grid', 'paired']) {
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
