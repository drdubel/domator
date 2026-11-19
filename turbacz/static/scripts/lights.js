// ============================================
// LIGHTS - switching lights and editing their layout
// Requires: common.js
// ============================================

const LONG_PRESS_MS = 380
const LONG_PRESS_MOVE_CANCEL_PX = 10
// Relays answer a command with their new state well within this.
const PENDING_TIMEOUT_MS = 5000
const COLLAPSED_SECTIONS_KEY = 'lights_collapsed_sections'
const IMAGE_ON = '/static/data/img/on.png'
const IMAGE_OFF = '/static/data/img/off.png'
const NOT_CONNECTED = 'Not connected to Turbacz. Reconnecting…'
const VIEW_ONLY = 'Your account can view the lights but not switch them'
const CONNECTION_LABELS = { connecting: 'Connecting…', connected: 'Connected', reconnecting: 'Reconnecting…' }
const SECTION_ID = /^[0-9]{1,10}$/
const RELAY_ID = /^[0-9]{1,19}$/
const OUTPUT_ID = /^[a-p]$/

const Lights = {
	sections: [], // [{ id, name }] in display order
	outputs: new Map(), // card id -> { cardId, relayId, outputId, name, sectionId, outputIdx }
	states: new Map(), // card id -> 0 | 1, missing while unknown
	cards: new Map(), // card id -> rendered card
	onlineRelays: null, // relay ids, null until the server reports them
	permissions: { control: false, edit: false },
	defaultSection: '1',
	configured: false,
	connection: 'connecting', // 'connecting' | 'connected' | 'reconnecting'
	editMode: false,
	pending: new Map(), // card id -> timer of the command waiting for its relay
	collapsed: loadCollapsedSections(),
	drag: null, // the light being dragged, see bindMouseDrag() and startTouchDrag()
	press: null, // a touch that becomes a drag if held
	deferredConfiguration: null,
	suppressClickUntil: 0,
	confirmResolve: null,
	moveCardId: null,
	renamedSectionId: null, // null while the section dialog adds one
}

let invisibleDragImage = null

function byId(id) {
	return document.getElementById(id)
}

function textElement(tag, className, text) {
	const element = document.createElement(tag)
	element.className = className
	element.textContent = text
	return element
}

// ============================================
// SERVER MESSAGES
// ============================================

const wsManager = new WebSocketManager('/lights/ws/', function (event) {
	handleMessage(JSON.parse(event.data))
})

function handleMessage(msg) {
	if (msg.type === 'configuration') {
		receiveConfiguration(msg)
	} else if (msg.type === 'light_state') {
		receiveLightState(msg)
	} else if (msg.type === 'relay_status') {
		receiveRelayStatus(msg.online_relays)
	} else if (msg.type === 'error') {
		showToast(msg.message || 'Turbacz rejected the change', 'error')
	}
}

function receiveConfiguration(msg) {
	// Rebuilding the page would drop the light being dragged.
	if (Lights.drag) {
		Lights.deferredConfiguration = msg
		return
	}
	applyConfiguration(msg)
}

function parseOutputMeta(outputMeta) {
	if (Array.isArray(outputMeta)) {
		return {
			name: String(outputMeta[0] ?? ''),
			sectionId: String(outputMeta[1]),
			outputIdx: Number(outputMeta[2]) || 0,
		}
	}

	return { name: String(outputMeta ?? ''), sectionId: '1', outputIdx: 0 }
}

function applyConfiguration(msg) {
	const names = msg.sections && typeof msg.sections === 'object' ? msg.sections : {}
	const ids = Object.keys(names).filter(id => SECTION_ID.test(id))
	const order = (Array.isArray(msg.section_order) ? msg.section_order : []).map(String).filter(id => ids.includes(id))
	// Sections missing from the order follow by id; a plain sort() would put 10 before 2.
	const rest = ids.filter(id => !order.includes(id)).sort((a, b) => Number(a) - Number(b))
	Lights.sections = [...new Set([...order, ...rest])].map(id => ({ id, name: String(names[id]) }))
	if (msg.default_section !== undefined) {
		Lights.defaultSection = String(msg.default_section)
	}

	const known = new Set(Lights.sections.map(section => section.id))
	const fallback = known.has(Lights.defaultSection) ? Lights.defaultSection : Lights.sections[0]?.id
	Lights.outputs = new Map()
	for (const [relayId, outputs] of Object.entries(msg.named_outputs || {})) {
		if (!RELAY_ID.test(relayId) || !outputs || typeof outputs !== 'object') continue
		for (const [outputId, outputMeta] of Object.entries(outputs)) {
			if (!OUTPUT_ID.test(outputId)) continue
			const output = parseOutputMeta(outputMeta)
			// A light left in a section that no longer exists stays reachable.
			if (!known.has(output.sectionId)) output.sectionId = fallback
			const cardId = `${relayId}${outputId}`
			Lights.outputs.set(cardId, { cardId, relayId, outputId, ...output })
		}
	}

	// Only a page's own configuration carries these; broadcasts keep them.
	if (msg.permissions) {
		Lights.permissions = { control: msg.permissions.control === true, edit: msg.permissions.edit === true }
		if (!Lights.permissions.edit) Lights.editMode = false
	}
	if (Array.isArray(msg.online_relays)) {
		Lights.onlineRelays = new Set(msg.online_relays.map(String))
	}
	if (msg.states && typeof msg.states === 'object') {
		Lights.states = new Map()
		for (const [relayId, outputs] of Object.entries(msg.states)) {
			for (const [outputId, state] of Object.entries(outputs || {})) {
				Lights.states.set(`${relayId}${outputId}`, state === 1 ? 1 : 0)
			}
		}
	}

	Lights.configured = true
	render()
}

function receiveLightState(msg) {
	const cardId = `${msg.relay_id}${msg.output_id}`
	Lights.states.set(cardId, msg.state === 1 ? 1 : 0)
	Lights.pending.delete(cardId)
	const card = Lights.cards.get(cardId)
	if (card) updateCard(card)
	updateSummaries()
}

function receiveRelayStatus(onlineRelays) {
	if (!Array.isArray(onlineRelays)) return
	Lights.onlineRelays = new Set(onlineRelays.map(String))
	updateCards()
}

function setConnection(connection) {
	Lights.connection = connection
	if (connection !== 'connected') {
		// Answers to earlier commands are lost with the connection.
		Lights.pending.clear()
	}

	const status = byId('connectionStatus')
	status.dataset.state = connection
	status.querySelector('.connection-label').textContent = CONNECTION_LABELS[connection]
	status.title = connection === 'connected' ? 'Connected to Turbacz' : 'Lights can be switched once Turbacz is reachable again'
	document.body.classList.toggle('lights-disconnected', connection !== 'connected')
	updateCards()
}

// ============================================
// RENDERING
// ============================================

function render() {
	const focusKey = document.activeElement?.dataset?.focusKey
	Lights.cards = new Map()
	const sections = Lights.sections.map(renderSection)
	const room = byId('room')
	room.replaceChildren(...sections)
	if (sections.every(section => section.hidden)) {
		room.appendChild(textElement('p', 'room-placeholder',
			'No lights to show yet. Lights appear here once their relay outputs are named in Connections.'))
	}

	document.body.classList.toggle('lights-edit-mode', Lights.editMode)
	updateSummaries()
	updateNotice()
	if (focusKey) {
		const target = [...document.querySelectorAll('[data-focus-key]')].find(element => element.dataset.focusKey === focusKey)
		if (target) target.focus()
	}
}

function sectionOutputs(sectionId) {
	return [...Lights.outputs.values()]
		.filter(output => output.sectionId === sectionId)
		.sort((a, b) => a.outputIdx - b.outputIdx
			|| a.relayId.localeCompare(b.relayId, undefined, { numeric: true })
			|| a.outputId.localeCompare(b.outputId))
}

function isOn(output) {
	return Lights.states.get(output.cardId) === 1
}

function renderSection(section, index) {
	const outputs = sectionOutputs(section.id)
	const collapsed = Lights.collapsed.has(section.id)

	const element = document.createElement('section')
	element.className = 'room'
	element.classList.toggle('collapsed', collapsed)
	element.dataset.sectionId = section.id
	// Empty sections only matter while arranging lights.
	element.hidden = !Lights.editMode && outputs.length === 0

	const toggle = document.createElement('button')
	toggle.type = 'button'
	toggle.className = 'section-toggle'
	toggle.dataset.focusKey = `section-toggle-${section.id}`
	toggle.setAttribute('aria-expanded', String(!collapsed))
	toggle.setAttribute('aria-controls', `section-grid-${section.id}`)
	toggle.addEventListener('click', () => toggleSection(section.id))
	const chevron = textElement('span', 'section-chevron', '▾')
	chevron.setAttribute('aria-hidden', 'true')
	const count = textElement('span', 'section-count', '')
	count.hidden = true
	toggle.append(chevron, textElement('span', 'section-name', section.name), count)
	const heading = document.createElement('h2')
	heading.appendChild(toggle)

	const actions = document.createElement('div')
	actions.className = 'section-actions'
	if (Lights.editMode) {
		const deletable = section.id !== Lights.defaultSection && Lights.sections.length > 1
		actions.append(
			iconButton('↑', `Move ${section.name} up`, `section-up-${section.id}`, () => moveSection(section.id, -1), index === 0),
			iconButton('↓', `Move ${section.name} down`, `section-down-${section.id}`, () => moveSection(section.id, 1),
				index === Lights.sections.length - 1),
			iconButton('✏️', `Rename ${section.name}`, `section-rename-${section.id}`, () => openSectionDialog(section.id)),
			iconButton('🗑️', deletable ? `Delete ${section.name}` : `${section.name} can't be deleted: new lights are added to it`,
				`section-delete-${section.id}`, () => deleteSection(section.id), !deletable),
		)
	} else {
		const allOff = textElement('button', 'section-all-off', 'All off')
		allOff.type = 'button'
		allOff.hidden = true
		allOff.dataset.focusKey = `section-off-${section.id}`
		allOff.setAttribute('aria-label', `Turn off the lights in ${section.name}`)
		allOff.addEventListener('click', () => switchSectionOff(section.id))
		actions.appendChild(allOff)
	}

	const header = document.createElement('div')
	header.className = 'section-header'
	header.append(heading, actions)

	const grid = document.createElement('div')
	grid.className = 'light-grid'
	grid.id = `section-grid-${section.id}`
	grid.dataset.sectionId = section.id
	grid.hidden = collapsed
	for (const output of outputs) {
		const card = Lights.editMode ? renderEditableCard(output) : renderCard(output)
		Lights.cards.set(output.cardId, card)
		grid.appendChild(card)
	}
	if (Lights.editMode) bindDropTarget(grid)

	element.append(header, grid)
	return element
}

function iconButton(icon, label, focusKey, onClick, disabled = false) {
	const button = textElement('button', 'icon-btn', icon)
	button.type = 'button'
	button.title = label
	button.setAttribute('aria-label', label)
	button.dataset.focusKey = focusKey
	button.disabled = disabled
	button.addEventListener('click', onClick)
	return button
}

function renderCard(output) {
	const card = document.createElement('button')
	card.type = 'button'
	card.className = 'light-card'
	card.dataset.cardId = output.cardId
	card.dataset.focusKey = `card-${output.cardId}`
	card.addEventListener('click', () => toggleLight(output.cardId))

	const image = document.createElement('img')
	image.alt = ''
	card.append(image, textElement('span', 'light-label', output.name), textElement('span', 'light-status', ''))
	updateCard(card)
	return card
}

function renderEditableCard(output) {
	// Firefox cannot drag <button>s, so lights being arranged are plain elements.
	const card = document.createElement('div')
	card.className = 'light-card is-editing'
	card.dataset.cardId = output.cardId
	card.dataset.focusKey = `card-${output.cardId}`
	card.tabIndex = 0
	card.draggable = true
	card.setAttribute('role', 'button')
	card.setAttribute('aria-haspopup', 'dialog')
	card.setAttribute('aria-label', `Move ${output.name}`)

	const grip = textElement('span', 'drag-grip', '⠿')
	grip.setAttribute('aria-hidden', 'true')
	const image = document.createElement('img')
	image.alt = ''
	image.src = IMAGE_OFF
	card.append(grip, image, textElement('span', 'light-label', output.name))

	card.addEventListener('click', () => {
		if (Date.now() >= Lights.suppressClickUntil) openMoveDialog(output.cardId)
	})
	card.addEventListener('keydown', event => {
		if (event.key === 'Enter' || event.key === ' ') {
			event.preventDefault()
			openMoveDialog(output.cardId)
		}
	})
	bindMouseDrag(card)
	bindTouchDrag(card)
	return card
}

function updateCard(card) {
	const output = Lights.outputs.get(card.dataset.cardId)
	if (!output || card.classList.contains('is-editing')) return

	const state = Lights.states.get(output.cardId)
	const offline = Lights.onlineRelays !== null && !Lights.onlineRelays.has(output.relayId)
	const pending = Lights.pending.has(output.cardId)
	const usable = Lights.permissions.control && Lights.connection === 'connected'
	card.classList.toggle('is-on', state === 1)
	card.classList.toggle('is-unknown', state === undefined)
	card.classList.toggle('is-offline', offline)
	card.classList.toggle('is-pending', pending)
	card.setAttribute('aria-pressed', String(state === 1))
	card.setAttribute('aria-busy', String(pending))
	card.setAttribute('aria-disabled', String(!usable))
	card.title = offline ? 'Its relay is not responding, so this light may not switch' : ''
	card.querySelector('img').src = state === 1 ? IMAGE_ON : IMAGE_OFF

	const status = card.querySelector('.light-status')
	status.textContent = offline ? 'Offline' : state === undefined ? 'No status' : ''
	status.hidden = !status.textContent
}

function updateCards() {
	Lights.cards.forEach(updateCard)
}

function updateSummaries() {
	for (const element of document.querySelectorAll('#room .room')) {
		const on = sectionOutputs(element.dataset.sectionId).filter(isOn).length
		const count = element.querySelector('.section-count')
		count.textContent = on ? `${on} on` : ''
		count.hidden = !on
		const allOff = element.querySelector('.section-all-off')
		if (allOff) allOff.hidden = !on || !Lights.permissions.control
	}
	updateToolbar()
}

function updateToolbar() {
	const editing = Lights.editMode
	byId('editButton').hidden = !Lights.permissions.edit || editing
	byId('addSectionButton').hidden = !editing
	byId('rcmLink').hidden = !editing
	byId('doneButton').hidden = !editing
	byId('allOffButton').hidden = editing || !Lights.permissions.control || ![...Lights.outputs.values()].some(isOn)
}

function updateNotice() {
	const notice = byId('pageNotice')
	if (Lights.editMode) {
		notice.textContent = 'Drag lights to reorder them or move them to another section, or select a light to move it with buttons.'
	} else if (Lights.configured && !Lights.permissions.control) {
		notice.textContent = 'View only: your account can see the lights but not switch them.'
	} else {
		notice.textContent = ''
	}
	notice.hidden = !notice.textContent
}

// ============================================
// SWITCHING
// ============================================

function toggleLight(cardId) {
	const output = Lights.outputs.get(cardId)
	if (!output || Lights.pending.has(cardId)) return
	if (!Lights.permissions.control) {
		showToast(VIEW_ONLY, 'error')
		return
	}

	// An unknown state is switched on, as before states were reported.
	const state = Lights.states.get(cardId) === 1 ? 0 : 1
	if (sendCommand({ relay_id: output.relayId, output_id: output.outputId, state })) {
		markPending([cardId])
	}
}

function switchSectionOff(sectionId) {
	const on = sectionOutputs(sectionId).filter(isOn)
	if (sendCommand({ type: 'all_off', section: Number(sectionId) })) {
		markPending(on.map(output => output.cardId))
	}
}

async function switchEverythingOff() {
	const on = [...Lights.outputs.values()].filter(isOn)
	const confirmed = await confirmDialog({
		title: 'Turn off all lights?',
		message: on.length === 1 ? `${on[0].name} is on.` : `${on.length} lights are on.`,
		confirmLabel: 'Turn all off',
	})
	if (confirmed && sendCommand({ type: 'all_off' })) {
		markPending(on.map(output => output.cardId))
	}
}

function markPending(cardIds) {
	const waiting = cardIds.filter(cardId => !Lights.pending.has(cardId))
	if (!waiting.length) return

	const timer = setTimeout(() => {
		const late = waiting.filter(cardId => Lights.pending.get(cardId) === timer)
		if (!late.length) return
		late.forEach(cardId => Lights.pending.delete(cardId))
		updateCards()
		const name = Lights.outputs.get(late[0])?.name
		showToast(late.length === 1 && name
			? `${name} did not respond. Its relay may be offline.`
			: `${late.length} lights did not respond. Their relays may be offline.`, 'error')
	}, PENDING_TIMEOUT_MS)
	waiting.forEach(cardId => Lights.pending.set(cardId, timer))
	updateCards()
}

function sendCommand(command) {
	if (Lights.connection === 'connected' && wsManager.send(JSON.stringify(command))) {
		return true
	}

	showToast(NOT_CONNECTED, 'error')
	return false
}

// ============================================
// SECTIONS
// ============================================

function sectionElement(sectionId) {
	return [...document.querySelectorAll('#room .room')].find(element => element.dataset.sectionId === sectionId) || null
}

function gridElement(sectionId) {
	return [...document.querySelectorAll('#room .light-grid')].find(grid => grid.dataset.sectionId === sectionId) || null
}

function toggleSection(sectionId) {
	const collapsed = !Lights.collapsed.has(sectionId)
	if (collapsed) {
		Lights.collapsed.add(sectionId)
	} else {
		Lights.collapsed.delete(sectionId)
	}
	saveCollapsedSections()

	const element = sectionElement(sectionId)
	if (!element) return
	element.classList.toggle('collapsed', collapsed)
	element.querySelector('.section-toggle').setAttribute('aria-expanded', String(!collapsed))
	element.querySelector('.light-grid').hidden = collapsed
}

function loadCollapsedSections() {
	try {
		const ids = JSON.parse(localStorage.getItem(COLLAPSED_SECTIONS_KEY) || '[]')
		return new Set(Array.isArray(ids) ? ids.map(String) : [])
	} catch (error) {
		return new Set()
	}
}

function saveCollapsedSections() {
	const known = new Set(Lights.sections.map(section => section.id))
	try {
		localStorage.setItem(COLLAPSED_SECTIONS_KEY, JSON.stringify([...Lights.collapsed].filter(id => known.has(id))))
	} catch (error) {
		// Storage can be unavailable, e.g. in private browsing; collapsing still works until a reload.
	}
}

function moveSection(sectionId, delta) {
	const order = Lights.sections.map(section => section.id)
	const index = order.indexOf(sectionId)
	const target = index + delta
	if (index < 0 || target < 0 || target >= order.length) return

	order.splice(index, 1)
	order.splice(target, 0, sectionId)
	sendCommand({ type: 'reorder_sections', order: order.map(Number) })
}

function openSectionDialog(sectionId = null) {
	const section = Lights.sections.find(candidate => candidate.id === sectionId)
	Lights.renamedSectionId = section ? section.id : null
	byId('sectionDialogTitle').textContent = section ? `Rename ${section.name}` : 'Add section'
	byId('sectionSubmit').textContent = section ? 'Rename' : 'Add'
	byId('sectionName').value = section ? section.name : ''
	showSectionNameError('')
	openDialog(byId('sectionDialog'))
	byId('sectionName').focus()
}

function showSectionNameError(message) {
	const error = byId('sectionNameError')
	error.textContent = message
	error.hidden = !message
	byId('sectionName').setAttribute('aria-invalid', String(Boolean(message)))
}

function submitSectionForm(event) {
	event.preventDefault()
	const name = byId('sectionName').value.trim()
	const renamed = Lights.renamedSectionId
	if (!name) {
		showSectionNameError('Enter a name for the section.')
		return
	}
	if (Lights.sections.some(section => section.id !== renamed && section.name.trim().toLowerCase() === name.toLowerCase())) {
		showSectionNameError('A section with this name already exists.')
		return
	}

	const command = renamed === null
		? { type: 'add_section', name }
		: { type: 'rename_section', section: Number(renamed), name }
	if (sendCommand(command)) closeDialog(byId('sectionDialog'))
}

async function deleteSection(sectionId) {
	const section = Lights.sections.find(candidate => candidate.id === sectionId)
	if (!section) return

	const lights = sectionOutputs(sectionId).length
	const target = Lights.sections.find(candidate => candidate.id === Lights.defaultSection)
		|| Lights.sections.find(candidate => candidate.id !== sectionId)
	const confirmed = await confirmDialog({
		title: `Delete ${section.name}?`,
		message: !lights ? 'It has no lights.'
			: `${lights === 1 ? 'Its light moves' : `Its ${lights} lights move`} to ${target ? target.name : 'another section'}.`,
		confirmLabel: 'Delete section',
	})
	if (confirmed) sendCommand({ type: 'remove_section', section: Number(sectionId) })
}

// ============================================
// EDIT MODE AND MOVING LIGHTS
// ============================================

function setEditMode(on) {
	if (on && !Lights.permissions.edit) return
	Lights.editMode = on
	cancelLongPress()
	render()
	byId(on ? 'doneButton' : 'editButton').focus()
}

function positionOf(card) {
	return { grid: card.parentElement, next: card.nextElementSibling }
}

function restorePosition(card, from) {
	from.grid.insertBefore(card, from.next)
}

function openMoveDialog(cardId) {
	const output = Lights.outputs.get(cardId)
	const card = Lights.cards.get(cardId)
	if (!output || !card || !card.parentElement) return

	Lights.moveCardId = cardId
	const sectionId = card.parentElement.dataset.sectionId
	const section = Lights.sections.find(candidate => candidate.id === sectionId)
	const siblings = [...card.parentElement.children]
	const index = siblings.indexOf(card)
	byId('moveDialogTitle').textContent = `Move ${output.name}`
	byId('moveDialogText').textContent = `Light ${index + 1} of ${siblings.length} in ${section ? section.name : 'its section'}.`
	byId('moveEarlierButton').disabled = index === 0
	byId('moveLaterButton').disabled = index === siblings.length - 1

	const select = byId('moveSection')
	select.replaceChildren(...Lights.sections.map(candidate => {
		const option = document.createElement('option')
		option.value = candidate.id
		option.textContent = candidate.name
		return option
	}))
	select.value = sectionId
	openDialog(byId('moveDialog'))
}

function moveWithinSection(delta) {
	const card = Lights.cards.get(Lights.moveCardId)
	closeDialog(byId('moveDialog'))
	if (!card || !card.parentElement) return

	const from = positionOf(card)
	if (delta < 0 && card.previousElementSibling) {
		card.parentElement.insertBefore(card, card.previousElementSibling)
	} else if (delta > 0 && card.nextElementSibling) {
		card.parentElement.insertBefore(card.nextElementSibling, card)
	}
	commitMove(card, from)
}

function submitMoveForm(event) {
	event.preventDefault()
	const card = Lights.cards.get(Lights.moveCardId)
	const grid = gridElement(byId('moveSection').value)
	closeDialog(byId('moveDialog'))
	if (!card || !grid || grid === card.parentElement) return

	const from = positionOf(card)
	grid.appendChild(card)
	commitMove(card, from)
}

function commitMove(card, from) {
	const grid = card.parentElement
	// Put back where it started.
	if (grid === from.grid && card.nextElementSibling === from.next) return

	const moves = []
	for (const affected of new Set([from.grid, grid])) {
		[...affected.querySelectorAll('.light-card[data-card-id]')].forEach((element, index) => {
			const output = Lights.outputs.get(element.dataset.cardId)
			if (output) moves.push({ output, sectionId: affected.dataset.sectionId, index })
		})
	}

	const moved = Lights.outputs.get(card.dataset.cardId)
	const command = {
		type: 'layout_update',
		positions: moves.map(({ output, index }) => ({ relay_id: output.relayId, output_id: output.outputId, output_idx: index })),
	}
	if (grid !== from.grid) {
		Object.assign(command, { relay_id: moved.relayId, output_id: moved.outputId, section: Number(grid.dataset.sectionId) })
	}
	if (!sendCommand(command)) {
		restorePosition(card, from)
		return
	}

	// Match the page until the server broadcasts the new layout.
	for (const { output, sectionId, index } of moves) {
		output.sectionId = sectionId
		output.outputIdx = index
	}
}

function endDrag() {
	Lights.drag = null
	const deferred = Lights.deferredConfiguration
	Lights.deferredConfiguration = null
	if (deferred) applyConfiguration(deferred)
}

function clearDropHighlights() {
	document.querySelectorAll('.light-grid.drag-over').forEach(grid => grid.classList.remove('drag-over'))
}

function dragImage() {
	if (!invisibleDragImage) {
		invisibleDragImage = document.createElement('canvas')
		invisibleDragImage.width = 1
		invisibleDragImage.height = 1
	}
	return invisibleDragImage
}

function getDragAfterElement(container, x, y) {
	const cards = [...container.querySelectorAll('.light-card:not(.dragging):not(.touch-dragging)')]

	let closest = null
	let closestScore = Infinity

	cards.forEach(card => {
		const box = card.getBoundingClientRect()
		const offsetY = y - (box.top + box.height / 2)
		const offsetX = x - (box.left + box.width / 2)

		if (offsetY < 0 || (Math.abs(offsetY) < box.height / 2 && offsetX < 0)) {
			const score = Math.abs(offsetY) * 1000 + Math.abs(offsetX)
			if (score < closestScore) {
				closestScore = score
				closest = card
			}
		}
	})

	return closest
}

function placeAt(grid, element, x, y) {
	const after = getDragAfterElement(grid, x, y)
	if (!after) {
		if (grid.lastElementChild !== element) grid.appendChild(element)
	} else if (after !== element && after.previousElementSibling !== element) {
		grid.insertBefore(element, after)
	}
}

function bindMouseDrag(card) {
	card.addEventListener('dragstart', event => {
		if (Lights.drag) return
		Lights.drag = { card, kind: 'mouse', from: positionOf(card), dropped: false }
		card.classList.add('dragging')
		if (event.dataTransfer) {
			event.dataTransfer.effectAllowed = 'move'
			event.dataTransfer.setData('text/plain', card.dataset.cardId)
			// The card itself moves through the page, so hide the browser's ghost image.
			if (event.dataTransfer.setDragImage) event.dataTransfer.setDragImage(dragImage(), 0, 0)
		}
	})

	card.addEventListener('dragend', () => {
		const drag = Lights.drag
		if (!drag || drag.card !== card) return
		card.classList.remove('dragging')
		clearDropHighlights()
		// Released outside a section or cancelled with Escape.
		if (!drag.dropped) restorePosition(card, drag.from)
		endDrag()
	})
}

function bindDropTarget(grid) {
	grid.addEventListener('dragover', event => {
		const drag = Lights.drag
		if (!drag || drag.kind !== 'mouse') return
		event.preventDefault()
		grid.classList.add('drag-over')
		placeAt(grid, drag.card, event.clientX, event.clientY)
	})

	grid.addEventListener('dragleave', () => grid.classList.remove('drag-over'))

	grid.addEventListener('drop', event => {
		const drag = Lights.drag
		if (!drag || drag.kind !== 'mouse') return
		event.preventDefault()
		drag.dropped = true
		grid.classList.remove('drag-over')
		if (drag.card.parentElement !== grid) placeAt(grid, drag.card, event.clientX, event.clientY)
		Lights.suppressClickUntil = Date.now() + 250
		commitMove(drag.card, drag.from)
	})
}

function bindTouchDrag(card) {
	card.addEventListener('touchstart', event => {
		cancelLongPress()
		if (event.touches.length !== 1 || Lights.drag) return

		const touch = event.touches[0]
		const press = { card, x: touch.clientX, y: touch.clientY }
		press.timer = setTimeout(() => {
			if (Lights.press === press) startTouchDrag(press)
		}, LONG_PRESS_MS)
		Lights.press = press
	}, { passive: true })

	card.addEventListener('touchmove', event => {
		const drag = Lights.drag
		if (drag && drag.kind === 'touch' && drag.card === card) {
			event.preventDefault()
			if (event.touches.length) moveTouchDrag(event.touches[0].clientX, event.touches[0].clientY)
			return
		}

		// Scrolling the page, not holding the light.
		const press = Lights.press
		if (!press || press.card !== card || !event.touches.length) return
		const touch = event.touches[0]
		if (Math.hypot(touch.clientX - press.x, touch.clientY - press.y) > LONG_PRESS_MOVE_CANCEL_PX) cancelLongPress()
	}, { passive: false })

	card.addEventListener('touchend', () => endTouch(card, false), { passive: true })
	card.addEventListener('touchcancel', () => endTouch(card, true), { passive: true })
}

function cancelLongPress() {
	if (Lights.press) clearTimeout(Lights.press.timer)
	Lights.press = null
}

function endTouch(card, cancelled) {
	cancelLongPress()
	const drag = Lights.drag
	if (drag && drag.kind === 'touch' && drag.card === card) finishTouchDrag(cancelled)
}

function startTouchDrag(press) {
	Lights.press = null
	const { card, x, y } = press
	const grid = card.parentElement
	if (!grid || !grid.classList.contains('light-grid')) return

	const rect = card.getBoundingClientRect()
	const from = positionOf(card)
	const placeholder = document.createElement('div')
	placeholder.className = 'light-card-placeholder'
	placeholder.style.width = `${rect.width}px`
	placeholder.style.height = `${rect.height}px`
	grid.insertBefore(placeholder, card.nextSibling)

	Lights.drag = { card, kind: 'touch', from, placeholder, offsetX: x - rect.left, offsetY: y - rect.top }
	document.body.classList.add('touch-reordering')
	card.classList.add('touch-dragging')
	Object.assign(card.style, {
		width: `${rect.width}px`,
		height: `${rect.height}px`,
		left: '0',
		top: '0',
		position: 'fixed',
		zIndex: '2500',
		pointerEvents: 'none',
	})
	moveTouchDrag(x, y)
}

function moveTouchDrag(x, y) {
	const drag = Lights.drag
	drag.card.style.transform = `translate3d(${x - drag.offsetX}px, ${y - drag.offsetY}px, 0)`

	const target = document.elementFromPoint ? document.elementFromPoint(x, y) : null
	const grid = target ? target.closest('.light-grid') : null
	if (!grid || grid.hidden) return

	clearDropHighlights()
	grid.classList.add('drag-over')
	placeAt(grid, drag.placeholder, x, y)
}

function finishTouchDrag(cancelled) {
	const { card, placeholder, from } = Lights.drag
	clearDropHighlights()
	if (cancelled || !placeholder.parentElement) {
		restorePosition(card, from)
	} else {
		placeholder.parentElement.insertBefore(card, placeholder)
	}
	placeholder.remove()

	card.classList.remove('touch-dragging')
	for (const property of ['width', 'height', 'left', 'top', 'position', 'zIndex', 'pointerEvents', 'transform']) {
		card.style[property] = ''
	}
	document.body.classList.remove('touch-reordering')
	// The lifted finger still produces a click.
	Lights.suppressClickUntil = Date.now() + 400

	if (!cancelled) commitMove(card, from)
	endDrag()
}

// ============================================
// DIALOGS AND MESSAGES
// ============================================

function openDialog(dialog) {
	document.querySelectorAll('dialog[open]').forEach(other => {
		if (other !== dialog) other.close()
	})
	if (!dialog.open) dialog.showModal()
}

function closeDialog(dialog) {
	if (dialog && dialog.open) dialog.close()
}

function confirmDialog({ title, message, confirmLabel }) {
	settleConfirm(false)
	byId('confirmDialogTitle').textContent = title
	byId('confirmDialogText').textContent = message
	byId('confirmSubmit').textContent = confirmLabel
	return new Promise(resolve => {
		Lights.confirmResolve = resolve
		openDialog(byId('confirmDialog'))
	})
}

function settleConfirm(confirmed) {
	const resolve = Lights.confirmResolve
	Lights.confirmResolve = null
	if (resolve) resolve(confirmed)
}

function showToast(message, kind = 'info') {
	const region = byId('toastRegion')
	const toast = textElement('div', `toast toast-${kind}`, message)
	region.appendChild(toast)
	while (region.children.length > 3) region.firstElementChild.remove()
	setTimeout(() => toast.remove(), kind === 'error' ? 6000 : 4000)
}

// ============================================
// SETUP
// ============================================

document.querySelector('.openbtn').addEventListener('click', openNav)
document.querySelector('.closebtn').addEventListener('click', closeNav)
byId('editButton').addEventListener('click', () => setEditMode(true))
byId('doneButton').addEventListener('click', () => setEditMode(false))
byId('addSectionButton').addEventListener('click', () => openSectionDialog())
byId('allOffButton').addEventListener('click', switchEverythingOff)
byId('sectionForm').addEventListener('submit', submitSectionForm)
byId('sectionName').addEventListener('input', () => showSectionNameError(''))
byId('moveForm').addEventListener('submit', submitMoveForm)
byId('moveEarlierButton').addEventListener('click', () => moveWithinSection(-1))
byId('moveLaterButton').addEventListener('click', () => moveWithinSection(1))
byId('confirmForm').addEventListener('submit', event => {
	event.preventDefault()
	settleConfirm(true)
	closeDialog(byId('confirmDialog'))
})
byId('confirmDialog').addEventListener('close', () => settleConfirm(false))
document.querySelectorAll('dialog').forEach(dialog => {
	// A click on the backdrop lands on the dialog itself.
	dialog.addEventListener('click', event => {
		if (event.target === dialog) closeDialog(dialog)
	})
})
document.querySelectorAll('[data-close-dialog]').forEach(button => {
	button.addEventListener('click', () => closeDialog(button.closest('dialog')))
})

wsManager.onOpen(() => setConnection('connected'))
wsManager.onClose(() => setConnection(Lights.connection === 'connecting' ? 'connecting' : 'reconnecting'))
wsManager.connect()
wsManager.startConnectionCheck()
wsManager.setupVisibilityHandler()
