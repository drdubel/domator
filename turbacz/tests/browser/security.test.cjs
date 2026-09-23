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
