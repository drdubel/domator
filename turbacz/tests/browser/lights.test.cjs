const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')
const vm = require('node:vm')
const { test } = require('node:test')
const { JSDOM } = require('jsdom')

const root = path.join(__dirname, '../..')
const read = name => fs.readFileSync(path.join(root, 'static', name), 'utf8')
const payloads = ['<img src=x onerror="alert(1)">', '" autofocus onfocus="alert(1)', "' onmouseover='alert(1)", '&quot;<script>alert(1)</script>']

// Relay 111 reports; relay 222 has not been heard from.
const CONFIGURATION = {
    type: 'configuration',
    sections: { 1: 'Default', 2: 'Kitchen', 3: 'Hall', 10: 'Garage' },
    section_order: [10, 2, 3, 1],
    default_section: 1,
    named_outputs: {
        111: { a: ['Ceiling', 2, 0, 0], b: ['Counter', 2, 1, 900], c: ['Hood', 2, 2, 0], d: ['Door', 3, 0, 0] },
        222: { a: ['Workbench', 10, 0, 0] },
    },
    permissions: { control: true, edit: true },
    online_relays: [111],
    states: { 111: { a: 1, b: 0, c: 0, d: 0 } },
}

function lightsPage({ storage = {} } = {}) {
    const dom = new JSDOM(read('lights.html'), { url: 'https://domator.example:8443/lights', runScripts: 'outside-only' })
    const { window } = dom
    const timers = new Map()
    let timerId = 0
    window.setTimeout = (callback, delay) => {
        timers.set(++timerId, { callback, delay })
        return timerId
    }
    window.clearTimeout = id => timers.delete(id)
    window.setInterval = () => 0
    window.console = { log() {}, warn() {}, info() {}, error() {} }
    // jsdom has no modal dialogs.
    window.HTMLDialogElement.prototype.showModal = function () { this.open = true }
    window.HTMLDialogElement.prototype.close = function () {
        if (!this.open) return
        this.open = false
        this.dispatchEvent(new window.Event('close'))
    }
    const sockets = []
    window.WebSocket = class {
        static CONNECTING = 0
        static OPEN = 1
        static CLOSING = 2
        static CLOSED = 3
        constructor(url) {
            this.url = url
            this.readyState = 0
            this.sent = []
            sockets.push(this)
        }
        send(data) { this.sent.push(JSON.parse(data)) }
        close() { this.readyState = 3 }
    }
    for (const [key, value] of Object.entries(storage)) window.localStorage.setItem(key, value)
    const run = source => vm.runInContext(source, dom.getInternalVMContext())
    run(read('scripts/common.js'))
    run(read('scripts/lights.js'))

    const document = window.document
    const socket = () => sockets.at(-1)
    const fire = delay => {
        for (const [id, timer] of [...timers]) {
            if (timer.delay === delay) {
                timers.delete(id)
                timer.callback()
            }
        }
    }
    return {
        dom, window, document, run, sockets,
        open() {
            socket().readyState = 1
            socket().onopen()
        },
        receive(message) { socket().onmessage({ data: JSON.stringify(message) }) },
        drop() {
            socket().readyState = 3
            socket().onclose({ code: 1006, reason: '' })
        },
        sent() { return socket().sent.filter(message => message.type !== 'ping') },
        clearSent() { socket().sent.length = 0 },
        card: name => [...document.querySelectorAll('.light-card')].find(card => card.querySelector('.light-label').textContent === name),
        section: name => [...document.querySelectorAll('.room')].find(section => section.querySelector('.section-name').textContent === name),
        grid: name => [...document.querySelectorAll('.room')].find(section => section.querySelector('.section-name').textContent === name).querySelector('.light-grid'),
        visibleSections: () => [...document.querySelectorAll('.room:not([hidden]) .section-name')].map(name => name.textContent),
        labels: grid => [...grid.querySelectorAll('.light-label')].map(label => label.textContent),
        toasts: () => [...document.querySelectorAll('.toast')].map(toast => toast.textContent),
        firePending: () => fire(5000),
        fireLongPress: () => fire(380),
        click: id => document.getElementById(id).click(),
    }
}

function connectedPage(options) {
    const page = lightsPage(options)
    page.open()
    page.receive(CONFIGURATION)
    return page
}

// Lets a confirmation dialog's promise reach the code awaiting it.
const settle = () => new Promise(resolve => setImmediate(resolve))

function touch(window, element, type, x = 5, y = 5) {
    const event = new window.Event(type, { bubbles: true, cancelable: true })
    Object.defineProperty(event, 'touches', { value: type === 'touchend' ? [] : [{ clientX: x, clientY: y }] })
    element.dispatchEvent(event)
}

test('sections follow Turbacz order, and light names, states and badges render literally', () => {
    const page = connectedPage()
    try {
        // Default has no lights, so it stays out of the way.
        assert.deepEqual(page.visibleSections(), ['Garage', 'Kitchen', 'Hall'])
        const ceiling = page.card('Ceiling')
        assert.equal(ceiling.tagName, 'BUTTON')
        assert.equal(ceiling.getAttribute('aria-pressed'), 'true')
        assert.ok(ceiling.classList.contains('is-on'))
        assert.equal(ceiling.querySelector('img').getAttribute('alt'), '')
        assert.equal(page.card('Counter').getAttribute('aria-pressed'), 'false')
        assert.equal(page.section('Kitchen').querySelector('.section-count').textContent, '1 on')
        assert.equal(page.section('Hall').querySelector('.section-count').hidden, true)
        // Relay 222 never reported: its light is neither on nor off.
        const workbench = page.card('Workbench')
        assert.ok(workbench.classList.contains('is-unknown') && workbench.classList.contains('is-offline'))
        assert.equal(workbench.querySelector('.light-status').textContent, 'Offline')
        page.receive({ type: 'relay_status', online_relays: [111, 222] })
        assert.equal(workbench.classList.contains('is-offline'), false)
        assert.equal(workbench.querySelector('.light-status').textContent, 'No status')

        // Without an order, sections follow their ids as numbers, not as text.
        page.receive({ ...CONFIGURATION, section_order: undefined })
        assert.deepEqual(page.visibleSections(), ['Kitchen', 'Hall', 'Garage'])

        for (const value of payloads) {
            page.receive({ ...CONFIGURATION, sections: { ...CONFIGURATION.sections, 2: value }, named_outputs: { 111: { a: [value, 2, 0, 0] } } })
            assert.deepEqual(page.visibleSections(), [value])
            assert.equal(page.document.querySelectorAll('.light-card').length, 1)
            assert.ok(page.card(value))
            assert.equal(page.document.getElementById('room').querySelectorAll('script, [onerror], [onfocus], [onmouseover], [autofocus]').length, 0)
        }
    } finally { page.window.close() }
})

test('a tap sends one command, shows it pending and reports a relay that does not answer', () => {
    const page = connectedPage()
    try {
        const ceiling = page.card('Ceiling')
        ceiling.click()
        ceiling.click()
        assert.deepEqual(page.sent(), [{ relay_id: '111', output_id: 'a', state: 0 }])
        assert.ok(ceiling.classList.contains('is-pending'))
        assert.equal(ceiling.getAttribute('aria-busy'), 'true')
        page.receive({ type: 'light_state', relay_id: 111, output_id: 'a', state: 0 })
        assert.equal(ceiling.classList.contains('is-pending'), false)
        assert.equal(ceiling.getAttribute('aria-pressed'), 'false')
        assert.equal(page.document.getElementById('allOffButton').hidden, true)

        page.card('Counter').click()
        page.firePending()
        assert.equal(page.card('Counter').classList.contains('is-pending'), false)
        assert.deepEqual(page.toasts(), ['Counter did not respond. Its relay may be offline.'])
    } finally { page.window.close() }
})

test('the connection state is shown and taps while disconnected explain instead of vanishing', () => {
    const page = lightsPage()
    try {
        const status = page.document.getElementById('connectionStatus')
        assert.equal(status.textContent.trim(), 'Connecting…')
        page.open()
        page.receive(CONFIGURATION)
        assert.equal(status.dataset.state, 'connected')
        page.card('Counter').click()
        page.drop()
        assert.equal(status.dataset.state, 'reconnecting')
        assert.equal(status.textContent.trim(), 'Reconnecting…')
        assert.ok(page.document.body.classList.contains('lights-disconnected'))
        // The answer to the earlier tap is lost with the connection.
        assert.equal(page.card('Counter').classList.contains('is-pending'), false)
        assert.equal(page.card('Hood').getAttribute('aria-disabled'), 'true')
        page.clearSent()
        page.card('Hood').click()
        assert.deepEqual(page.sent(), [])
        assert.deepEqual(page.toasts(), ['Not connected to Turbacz. Reconnecting…'])
    } finally { page.window.close() }
})

test('the page offers only what the account may do', () => {
    const page = lightsPage()
    try {
        page.open()
        page.receive({ ...CONFIGURATION, permissions: { control: false, edit: false } })
        const document = page.document
        assert.equal(document.getElementById('editButton').hidden, true)
        assert.equal(document.getElementById('allOffButton').hidden, true)
        assert.match(document.getElementById('pageNotice').textContent, /View only/)
        assert.equal(page.section('Kitchen').querySelector('.section-all-off').hidden, true)
        assert.equal(page.card('Ceiling').getAttribute('aria-disabled'), 'true')
        page.card('Ceiling').click()
        assert.deepEqual(page.sent(), [])
        assert.deepEqual(page.toasts(), ['Your account can view the lights but not switch them'])

        page.receive({ ...CONFIGURATION, permissions: { control: true, edit: false } })
        assert.equal(document.getElementById('editButton').hidden, true)
        assert.equal(document.getElementById('allOffButton').hidden, false)
        assert.equal(document.getElementById('pageNotice').hidden, true)
        // Layout broadcasts carry no permissions; the page keeps its own.
        const { permissions, ...broadcast } = CONFIGURATION
        page.receive(broadcast)
        assert.equal(document.getElementById('editButton').hidden, true)
        assert.equal(page.card('Ceiling').getAttribute('aria-disabled'), 'false')
    } finally { page.window.close() }
})

test('edit mode arranges lights without switching them and shows empty sections', () => {
    const page = connectedPage()
    try {
        const document = page.document
        page.click('editButton')
        assert.ok(document.body.classList.contains('lights-edit-mode'))
        assert.equal(document.activeElement, document.getElementById('doneButton'))
        for (const id of ['addSectionButton', 'rcmLink', 'doneButton']) assert.equal(document.getElementById(id).hidden, false)
        for (const id of ['editButton', 'allOffButton']) assert.equal(document.getElementById(id).hidden, true)
        assert.deepEqual(page.visibleSections(), ['Garage', 'Kitchen', 'Hall', 'Default'])
        const ceiling = page.card('Ceiling')
        assert.equal(ceiling.tagName, 'DIV')
        assert.equal(ceiling.getAttribute('role'), 'button')
        assert.equal(ceiling.draggable, true)
        assert.equal(ceiling.getAttribute('aria-label'), 'Move Ceiling')
        ceiling.click()
        assert.equal(document.getElementById('moveDialog').open, true)
        assert.equal(document.getElementById('moveDialogText').textContent, 'Light 1 of 3 in Kitchen.')
        assert.deepEqual(page.sent(), [])
        // New lights land in the default section, so it stays.
        assert.equal(page.section('Default').querySelector('[aria-label^="Default can\'t be deleted"]').disabled, true)

        page.click('doneButton')
        assert.equal(page.card('Ceiling').tagName, 'BUTTON')
        assert.deepEqual(page.visibleSections(), ['Garage', 'Kitchen', 'Hall'])
    } finally { page.window.close() }
})

test('lights move with buttons as well as by dragging', () => {
    const page = connectedPage()
    try {
        page.click('editButton')
        page.card('Ceiling').click()
        page.click('moveLaterButton')
        assert.deepEqual(page.sent(), [{
            type: 'layout_update',
            positions: [
                { relay_id: '111', output_id: 'b', output_idx: 0 },
                { relay_id: '111', output_id: 'a', output_idx: 1 },
                { relay_id: '111', output_id: 'c', output_idx: 2 },
            ],
        }])
        page.clearSent()
        page.card('Hood').click()
        page.document.getElementById('moveSection').value = '3'
        page.document.getElementById('moveForm').requestSubmit()
        assert.deepEqual(page.sent(), [{
            type: 'layout_update',
            positions: [
                { relay_id: '111', output_id: 'b', output_idx: 0 },
                { relay_id: '111', output_id: 'a', output_idx: 1 },
                { relay_id: '111', output_id: 'd', output_idx: 0 },
                { relay_id: '111', output_id: 'c', output_idx: 1 },
            ],
            relay_id: '111',
            output_id: 'c',
            section: 3,
        }])
        assert.deepEqual(page.labels(page.grid('Hall')), ['Door', 'Hood'])
    } finally { page.window.close() }
})

test('a cancelled dialog is never submitted by a later Enter', () => {
    const page = connectedPage()
    try {
        const { document, window } = page
        page.click('editButton')
        page.card('Ceiling').click()
        document.getElementById('moveSection').value = '3'
        document.querySelector('#moveDialog [data-close-dialog]').click()
        assert.equal(document.getElementById('moveDialog').open, false)
        document.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))

        page.click('addSectionButton')
        const name = document.getElementById('sectionName')
        name.value = ' kitchen '
        document.getElementById('sectionForm').requestSubmit()
        assert.equal(document.getElementById('sectionNameError').textContent, 'A section with this name already exists.')
        assert.equal(name.getAttribute('aria-invalid'), 'true')
        assert.equal(document.getElementById('sectionDialog').open, true)
        name.value = 'Garden'
        name.dispatchEvent(new window.Event('input'))
        assert.equal(document.getElementById('sectionNameError').hidden, true)
        document.getElementById('sectionForm').requestSubmit()
        assert.deepEqual(page.sent(), [{ type: 'add_section', name: 'Garden' }])
        assert.equal(document.getElementById('sectionDialog').open, false)
    } finally { page.window.close() }
})

test('a drag released outside a section puts the light back and an unmoved drop saves nothing', () => {
    const page = connectedPage()
    try {
        const { window } = page
        page.click('editButton')
        const ceiling = page.card('Ceiling')
        const drag = (card, type, target = card) => target.dispatchEvent(new window.MouseEvent(type, { bubbles: true, cancelable: true }))
        drag(ceiling, 'dragstart')
        drag(ceiling, 'dragover', page.grid('Hall'))
        assert.equal(ceiling.parentElement, page.grid('Hall'))
        drag(ceiling, 'dragend')
        assert.deepEqual(page.labels(page.grid('Kitchen')), ['Ceiling', 'Counter', 'Hood'])
        assert.deepEqual(page.labels(page.grid('Hall')), ['Door'])

        const hood = page.card('Hood')
        drag(hood, 'dragstart')
        drag(hood, 'dragover', page.grid('Kitchen'))
        drag(hood, 'drop', page.grid('Kitchen'))
        drag(hood, 'dragend')
        assert.deepEqual(page.sent(), [])

        drag(ceiling, 'dragstart')
        drag(ceiling, 'dragover', page.grid('Default'))
        drag(ceiling, 'drop', page.grid('Default'))
        drag(ceiling, 'dragend')
        assert.deepEqual(page.sent(), [{
            type: 'layout_update',
            positions: [
                { relay_id: '111', output_id: 'b', output_idx: 0 },
                { relay_id: '111', output_id: 'c', output_idx: 1 },
                { relay_id: '111', output_id: 'a', output_idx: 0 },
            ],
            relay_id: '111',
            output_id: 'a',
            section: 1,
        }])
    } finally { page.window.close() }
})

test('a held but unmoved light sends nothing, and the layout waits until the finger lifts', () => {
    const page = connectedPage()
    try {
        const { window } = page
        page.click('editButton')
        const ceiling = page.card('Ceiling')
        touch(window, ceiling, 'touchstart')
        page.fireLongPress()
        assert.ok(ceiling.classList.contains('touch-dragging'))
        page.receive({ ...CONFIGURATION, named_outputs: { 111: { ...CONFIGURATION.named_outputs[111], a: ['Chandelier', 2, 0, 0] } } })
        // Still the element being dragged; rebuilding the page would have dropped it.
        assert.equal(ceiling.isConnected, true)
        touch(window, ceiling, 'touchend')
        assert.deepEqual(page.sent(), [])
        assert.equal(page.document.querySelector('.light-card-placeholder'), null)
        assert.ok(page.card('Chandelier'))

        // Scrolling past a light is not holding it.
        const counter = page.card('Counter')
        touch(window, counter, 'touchstart')
        touch(window, counter, 'touchmove', 5, 60)
        page.fireLongPress()
        assert.equal(counter.classList.contains('touch-dragging'), false)
    } finally { page.window.close() }
})

test('collapsed sections stay collapsed across layout updates and reloads', () => {
    const page = connectedPage()
    try {
        const toggle = page.section('Kitchen').querySelector('.section-toggle')
        toggle.click()
        assert.equal(toggle.getAttribute('aria-expanded'), 'false')
        assert.equal(page.grid('Kitchen').hidden, true)
        page.receive(CONFIGURATION)
        assert.equal(page.grid('Kitchen').hidden, true)
        assert.equal(page.section('Kitchen').querySelector('.section-toggle').getAttribute('aria-expanded'), 'false')
        const stored = page.window.localStorage.getItem('lights_collapsed_sections')
        assert.equal(stored, '["2"]')

        const reloaded = connectedPage({ storage: { lights_collapsed_sections: stored } })
        try {
            assert.equal(reloaded.grid('Kitchen').hidden, true)
            assert.equal(reloaded.grid('Hall').hidden, false)
        } finally { reloaded.window.close() }
    } finally { page.window.close() }
})

test('focus stays on a light when the layout is broadcast again', () => {
    const page = connectedPage()
    try {
        page.card('Hood').focus()
        page.receive(CONFIGURATION)
        assert.equal(page.document.activeElement, page.card('Hood'))
    } finally { page.window.close() }
})

test('all off works per section at once and for the whole house after a confirmation', async () => {
    const page = connectedPage()
    try {
        const document = page.document
        page.section('Kitchen').querySelector('.section-all-off').click()
        assert.deepEqual(page.sent(), [{ type: 'all_off', section: 2 }])
        assert.ok(page.card('Ceiling').classList.contains('is-pending'))
        page.receive({ type: 'light_state', relay_id: 111, output_id: 'a', state: 0 })
        page.receive({ type: 'light_state', relay_id: 111, output_id: 'd', state: 1 })
        page.clearSent()

        page.click('allOffButton')
        assert.equal(document.getElementById('confirmDialog').open, true)
        assert.equal(document.getElementById('confirmDialogText').textContent, 'Door is on.')
        document.querySelector('#confirmDialog [data-close-dialog]').click()
        await settle()
        assert.deepEqual(page.sent(), [])

        page.click('allOffButton')
        document.getElementById('confirmForm').requestSubmit()
        await settle()
        assert.deepEqual(page.sent(), [{ type: 'all_off' }])
        assert.ok(page.card('Door').classList.contains('is-pending'))
    } finally { page.window.close() }
})

test('sections are reordered, renamed and deleted from edit mode', async () => {
    const page = connectedPage()
    try {
        const document = page.document
        page.click('editButton')
        page.section('Garage').querySelector('[aria-label="Move Garage down"]').click()
        assert.deepEqual(page.sent(), [{ type: 'reorder_sections', order: [2, 10, 3, 1] }])
        assert.equal(page.section('Garage').querySelector('[aria-label="Move Garage up"]').disabled, true)
        page.clearSent()

        page.section('Hall').querySelector('[aria-label="Rename Hall"]').click()
        assert.equal(document.getElementById('sectionDialogTitle').textContent, 'Rename Hall')
        assert.equal(document.getElementById('sectionName').value, 'Hall')
        // Its own name in another case is not taken.
        document.getElementById('sectionName').value = 'HALL'
        document.getElementById('sectionForm').requestSubmit()
        assert.deepEqual(page.sent(), [{ type: 'rename_section', section: 3, name: 'HALL' }])
        page.clearSent()

        page.section('Kitchen').querySelector('[aria-label="Delete Kitchen"]').click()
        assert.equal(document.getElementById('confirmDialogTitle').textContent, 'Delete Kitchen?')
        assert.equal(document.getElementById('confirmDialogText').textContent, 'Its 3 lights move to Default.')
        document.getElementById('confirmForm').requestSubmit()
        await settle()
        assert.deepEqual(page.sent(), [{ type: 'remove_section', section: 2 }])
    } finally { page.window.close() }
})

test('a rejected change is explained and the layout Turbacz resends replaces the local move', () => {
    const page = connectedPage()
    try {
        page.click('editButton')
        page.card('Ceiling').click()
        page.click('moveLaterButton')
        assert.deepEqual(page.labels(page.grid('Kitchen')), ['Counter', 'Ceiling', 'Hood'])
        page.receive({ type: 'error', message: 'Only admins can change the lights layout' })
        page.receive(CONFIGURATION)
        assert.deepEqual(page.toasts(), ['Only admins can change the lights layout'])
        assert.deepEqual(page.labels(page.grid('Kitchen')), ['Ceiling', 'Counter', 'Hood'])
    } finally { page.window.close() }
})
