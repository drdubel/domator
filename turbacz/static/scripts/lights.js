var lights = {}
var pendingClicks = new Set()
var namedOutputs = {}
var sections = {}
var active_sections = new Set()
var draggedCard = null
var preventCardToggleUntil = 0
var LONG_PRESS_MS = 380
var LONG_PRESS_MOVE_CANCEL_PX = 10
var touchPressTimer = null
var touchPressCandidate = null
var touchDragState = {
	active: false,
	card: null,
	placeholder: null,
	sourceGrid: null,
	offsetX: 0,
	offsetY: 0,
}
var invisibleDragImage = null

function parseOutputMeta(outputMeta) {
	if (Array.isArray(outputMeta)) {
		return {
			name: outputMeta[0],
			sectionId: outputMeta[1],
			outputIdx: typeof outputMeta[2] === 'number' ? outputMeta[2] : Number(outputMeta[2]) || 0,
		}
	}

	return {
		name: String(outputMeta || ''),
		sectionId: 1,
		outputIdx: 0,
	}
}

// Initialize WebSocket manager
var wsManager = new WebSocketManager('/lights/ws/', function (event) {
	var msg = JSON.parse(event.data)
	console.log('Received:', msg)

	if (msg.type == "configuration") {
		namedOutputs = msg.named_outputs || {}
		active_sections.clear()
		const select = document.getElementById("buttons")
		select.innerHTML = ""
		Object.entries(msg.named_outputs).forEach(([relayId, outputs]) => {
			const sortedOutputs = Object.entries(outputs)
				.map(([outputKey, outputMeta]) => [outputKey, parseOutputMeta(outputMeta)])
				.sort((a, b) => a[1].outputIdx - b[1].outputIdx)

			// Optional optgroup per relay
			const group = document.createElement("optgroup")
			group.label = `Relay ${relayId}`

			sortedOutputs.forEach(([outputKey, outputMeta]) => {
				const option = document.createElement("option")

				// Value encodes relay + output
				option.value = `${relayId}:${outputKey}`
				option.textContent = `${outputMeta.name}`

				group.appendChild(option)
			})

			select.appendChild(group)
		})

		const buttonSectionSelect = document.getElementById("sections")
		buttonSectionSelect.innerHTML = ""
		sections = Object.entries(msg.sections)
		sections.sort()
		sections.forEach(([sectionId, sectionName]) => {
			const option = document.createElement("option")
			option.value = sectionId
			option.textContent = sectionName
			buttonSectionSelect.appendChild(option)
		})

		const room = document.getElementById("room")
		room.innerHTML = ""

		Object.entries(msg.sections).forEach(([sectionId, sectionName]) => {
			const section = document.createElement(`section-${sectionId}`)
			section.className = "room"

			const header = document.createElement("h2")
			header.textContent = sectionName
			section.appendChild(header)

			const grid = document.createElement("div")
			grid.className = "light-grid"
			grid.dataset.sectionId = `${sectionId}`
			section.appendChild(grid)

			room.appendChild(section)
		})

		const outputsBySection = {}
		Object.entries(msg.named_outputs).forEach(([relayId, outputs]) => {
			Object.entries(outputs).forEach(([outputKey, outputMeta]) => {
				const parsed = parseOutputMeta(outputMeta)
				const sectionId = `${parsed.sectionId}`
				if (!outputsBySection[sectionId]) {
					outputsBySection[sectionId] = []
				}

				outputsBySection[sectionId].push({
					relayId: `${relayId}`,
					outputKey: `${outputKey}`,
					outputName: parsed.name,
					outputIdx: parsed.outputIdx,
					sectionId: sectionId,
				})
			})
		})

		Object.values(outputsBySection).forEach(cards => {
			cards.sort((a, b) => {
				if (a.outputIdx !== b.outputIdx) {
					return a.outputIdx - b.outputIdx
				}

				if (a.relayId !== b.relayId) {
					return a.relayId.localeCompare(b.relayId)
				}

				return a.outputKey.localeCompare(b.outputKey)
			})
		})

		Object.entries(outputsBySection).forEach(([sectionId, cards]) => {
			const section = document.querySelector(`section-${sectionId} .light-grid`)
			if (!section) return

			cards.forEach(card => {
				const button = document.createElement("div")
				button.className = "light-card"
				button.draggable = true
				button.dataset.cardId = `${card.relayId}${card.outputKey}`
				button.dataset.relayId = card.relayId
				button.dataset.outputId = card.outputKey
				button.dataset.sectionId = card.sectionId
				button.dataset.outputIdx = `${card.outputIdx}`
				button.onclick = function () {
					if (Date.now() < preventCardToggleUntil) {
						return
					}
					changeSwitchState(`${card.relayId}${card.outputKey}`)
				}

				const img = document.createElement("img")
				img.id = `${card.relayId}${card.outputKey}`
				img.src = "/static/data/img/off.png"
				img.alt = card.outputName

				const label = document.createElement("span")
				label.className = "light-label"
				label.textContent = card.outputName

				button.appendChild(img)
				button.appendChild(label)
				section.appendChild(button)
				active_sections.add(Number(card.sectionId))
			})
		})

		bindLightCardDnDHandlers()

		sections.forEach(([section, sectionName]) => {
			section = parseInt(section)
			if (!(active_sections.has(section))) {
				document.querySelector(`section-${section}`).style.display = "none"
			} else {
				document.querySelector(`section-${section}`).style.display = "block"
			}
		})

		makeSectionsCollapsible()
		for (const [id, state] of Object.entries(lights)) {
			updateLightUI(id, state)
		}
	}

	if (msg.type == "light_state") {
		pendingClicks.delete(msg.relay_id + msg.output_id)
		lights[msg.relay_id + msg.output_id] = msg.state
		updateLightUI(msg.relay_id + msg.output_id, msg.state)
	}
})

// Setup WebSocket callbacks
wsManager.onOpen(function () {
	pendingClicks.clear()
})

// Connect and start connection monitoring
wsManager.connect()
wsManager.startConnectionCheck()
wsManager.setupVisibilityHandler()

function updateLightUI(id, state) {
	var element = document.getElementById(id)
	if (!element) return

	if (state === 1) {
		element.src = "/static/data/img/on.png"
		element.parentElement.classList.add('active')
	} else {
		element.src = "/static/data/img/off.png"
		element.parentElement.classList.remove('active')
	}
}

function changeSwitchState(id) {
	if (!wsManager.isConnected()) {
		return
	}

	if (pendingClicks.has(id)) {
		return
	}

	pendingClicks.add(id)

	var newState = lights[id] === 1 ? 0 : 1

	console.log('Changing state of', id, 'to', newState)
	var msg = JSON.stringify({ "relay_id": id.slice(0, -1), "output_id": id.slice(-1), "state": newState })
	console.log(msg)

	if (!wsManager.send(msg)) {
		pendingClicks.delete(id)
	}

	setTimeout(function () {
		pendingClicks.delete(id)
	}, 2000)
}

function bindLightCardDnDHandlers() {
	const cards = document.querySelectorAll('.light-card')
	const grids = document.querySelectorAll('.light-grid')
	ensureInvisibleDragImage()

	cards.forEach(card => {
		card.style.cursor = 'grab'

		card.addEventListener('dragstart', function (e) {
			draggedCard = this
			this.dataset.sourceSectionId = this.dataset.sectionId || ''
			this.classList.add('dragging')
			e.dataTransfer.effectAllowed = 'move'
			e.dataTransfer.setData('text/plain', this.dataset.cardId || '')
			if (invisibleDragImage && e.dataTransfer.setDragImage) {
				e.dataTransfer.setDragImage(invisibleDragImage, 0, 0)
			}
		})

		card.addEventListener('dragend', function () {
			this.classList.remove('dragging')
			delete this.dataset.sourceSectionId
			draggedCard = null
			document.querySelectorAll('.light-grid.drag-over').forEach(grid => grid.classList.remove('drag-over'))
		})

		bindTouchReorderHandlers(card)
	})

	grids.forEach(grid => {
		grid.addEventListener('dragover', function (e) {
			e.preventDefault()
			if (!draggedCard) return

			this.classList.add('drag-over')

			const afterElement = getDragAfterElement(this, e.clientX, e.clientY)
			if (!afterElement) {
				this.appendChild(draggedCard)
			} else if (afterElement !== draggedCard) {
				this.insertBefore(draggedCard, afterElement)
			}
		})

		grid.addEventListener('dragleave', function () {
			this.classList.remove('drag-over')
		})

		grid.addEventListener('drop', function (e) {
			e.preventDefault()
			if (!draggedCard) return

			const sourceGrid = getCardGridFromSectionId(draggedCard.dataset.sourceSectionId)
			const movedCard = draggedCard
			const sourceSectionId = movedCard.dataset.sourceSectionId || movedCard.dataset.sectionId
			const targetSectionId = this.dataset.sectionId
			const sectionChanged = !!targetSectionId && sourceSectionId !== targetSectionId

			movedCard.dataset.sectionId = targetSectionId || movedCard.dataset.sectionId

			preventCardToggleUntil = Date.now() + 250
			const positions = collectPositionsFromGrids([sourceGrid, this])
			sendLayoutUpdate(
				positions,
				movedCard.dataset.relayId,
				movedCard.dataset.outputId,
				sectionChanged ? targetSectionId : null,
			)
			this.classList.remove('drag-over')
		})
	})
}

function ensureInvisibleDragImage() {
	if (invisibleDragImage) {
		return
	}

	const canvas = document.createElement('canvas')
	canvas.width = 1
	canvas.height = 1
	invisibleDragImage = canvas
}

function bindTouchReorderHandlers(card) {
	card.addEventListener('touchstart', function (e) {
		if (e.touches.length !== 1) {
			clearTouchPressState()
			return
		}

		const touch = e.touches[0]
		touchPressCandidate = {
			card: this,
			startX: touch.clientX,
			startY: touch.clientY,
		}

		clearTimeout(touchPressTimer)
		touchPressTimer = setTimeout(function () {
			if (!touchPressCandidate || touchDragState.active) {
				return
			}

			startTouchDrag(touchPressCandidate.card, touchPressCandidate.startX, touchPressCandidate.startY)
		}, LONG_PRESS_MS)
	}, { passive: true })

	card.addEventListener('touchmove', function (e) {
		if (touchDragState.active) {
			e.preventDefault()
			if (!e.touches.length) return

			moveTouchDrag(e.touches[0].clientX, e.touches[0].clientY)
			return
		}

		if (!touchPressCandidate || touchPressCandidate.card !== this || !e.touches.length) {
			return
		}

		const touch = e.touches[0]
		const dx = touch.clientX - touchPressCandidate.startX
		const dy = touch.clientY - touchPressCandidate.startY
		if (Math.hypot(dx, dy) > LONG_PRESS_MOVE_CANCEL_PX) {
			clearTouchPressState()
		}
	}, { passive: false })

	card.addEventListener('touchend', function () {
		if (touchDragState.active) {
			finishTouchDrag(false)
			return
		}

		clearTouchPressState()
	}, { passive: true })

	card.addEventListener('touchcancel', function () {
		if (touchDragState.active) {
			finishTouchDrag(true)
			return
		}

		clearTouchPressState()
	}, { passive: true })
}

function startTouchDrag(card, startX, startY) {
	const sourceGrid = card.parentElement
	if (!sourceGrid || !sourceGrid.classList.contains('light-grid')) {
		return
	}

	const rect = card.getBoundingClientRect()
	const placeholder = document.createElement('div')
	placeholder.className = 'light-card-placeholder'
	placeholder.style.width = `${rect.width}px`
	placeholder.style.height = `${rect.height}px`

	sourceGrid.insertBefore(placeholder, card.nextSibling)

	touchDragState.active = true
	touchDragState.card = card
	touchDragState.placeholder = placeholder
	touchDragState.sourceGrid = sourceGrid
	touchDragState.offsetX = startX - rect.left
	touchDragState.offsetY = startY - rect.top

	preventCardToggleUntil = Date.now() + 800
	document.body.classList.add('touch-reordering')

	card.classList.add('touch-dragging')
	card.style.width = `${rect.width}px`
	card.style.height = `${rect.height}px`
	card.style.left = '0'
	card.style.top = '0'
	card.style.position = 'fixed'
	card.style.zIndex = '2500'
	card.style.pointerEvents = 'none'

	moveTouchDrag(startX, startY)
}

function moveTouchDrag(clientX, clientY) {
	if (!touchDragState.active || !touchDragState.card) {
		return
	}

	const card = touchDragState.card
	const x = clientX - touchDragState.offsetX
	const y = clientY - touchDragState.offsetY
	card.style.transform = `translate3d(${x}px, ${y}px, 0)`

	const elementUnderTouch = document.elementFromPoint(clientX, clientY)
	const targetGrid = elementUnderTouch ? elementUnderTouch.closest('.light-grid') : null
	if (!targetGrid) {
		return
	}

	const afterElement = getDragAfterElement(targetGrid, clientX, clientY)
	if (!afterElement) {
		targetGrid.appendChild(touchDragState.placeholder)
	} else if (afterElement !== touchDragState.placeholder) {
		targetGrid.insertBefore(touchDragState.placeholder, afterElement)
	}

	targetGrid.classList.add('drag-over')
}

function finishTouchDrag(cancelled) {
	if (!touchDragState.active || !touchDragState.card) {
		clearTouchPressState()
		return
	}

	const card = touchDragState.card
	const placeholder = touchDragState.placeholder

	document.querySelectorAll('.light-grid.drag-over').forEach(grid => grid.classList.remove('drag-over'))

	if (!cancelled && placeholder && placeholder.parentElement) {
		const targetGrid = placeholder.parentElement
		const sourceGrid = touchDragState.sourceGrid
		const sourceSectionId = card.dataset.sectionId
		const targetSectionId = targetGrid.dataset.sectionId
		const sectionChanged = !!targetSectionId && sourceSectionId !== targetSectionId

		placeholder.parentElement.insertBefore(card, placeholder)
		card.dataset.sectionId = targetSectionId || card.dataset.sectionId

		const positions = collectPositionsFromGrids([sourceGrid, targetGrid])
		sendLayoutUpdate(
			positions,
			card.dataset.relayId,
			card.dataset.outputId,
			sectionChanged ? targetSectionId : null,
		)
	} else if (touchDragState.sourceGrid) {
		touchDragState.sourceGrid.appendChild(card)
	}

	if (placeholder && placeholder.parentElement) {
		placeholder.parentElement.removeChild(placeholder)
	}

	card.classList.remove('touch-dragging')
	card.style.width = ''
	card.style.height = ''
	card.style.left = ''
	card.style.top = ''
	card.style.position = ''
	card.style.zIndex = ''
	card.style.pointerEvents = ''
	card.style.transform = ''

	document.body.classList.remove('touch-reordering')

	touchDragState.active = false
	touchDragState.card = null
	touchDragState.placeholder = null
	touchDragState.sourceGrid = null
	touchDragState.offsetX = 0
	touchDragState.offsetY = 0

	clearTouchPressState()
}

function clearTouchPressState() {
	if (touchPressTimer) {
		clearTimeout(touchPressTimer)
	}

	touchPressTimer = null
	touchPressCandidate = null
}

function canDropInGrid(card, grid) {
	if (!card || !grid) {
		return false
	}

	return !!grid.dataset.sectionId
}

function getCardGridFromSectionId(sectionId) {
	if (!sectionId) {
		return null
	}

	return document.querySelector(`.light-grid[data-section-id="${sectionId}"]`)
}

function getDragAfterElement(container, x, y) {
	const cards = [...container.querySelectorAll('.light-card:not(.dragging)')]

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

function sendSectionPositions(sectionGrid) {
	if (!wsManager.isConnected()) {
		return
	}

	const cards = [...sectionGrid.querySelectorAll('.light-card')]
	const positions = cards.map((card, index) => {
		card.dataset.outputIdx = `${index}`
		return {
			relay_id: card.dataset.relayId,
			output_id: card.dataset.outputId,
			output_idx: index,
		}
	})

	if (!positions.length) {
		return
	}

	wsManager.send(JSON.stringify({ type: 'change_positions', positions: positions }))
}

function collectPositionsFromGrids(grids) {
	const uniqueGrids = [...new Set(grids.filter(Boolean))]
	const positions = []

	uniqueGrids.forEach(grid => {
		const cards = [...grid.querySelectorAll('.light-card')]
		cards.forEach((card, index) => {
			card.dataset.outputIdx = `${index}`
			positions.push({
				relay_id: card.dataset.relayId,
				output_id: card.dataset.outputId,
				output_idx: index,
			})
		})
	})

	return positions
}

function sendLayoutUpdate(positions, relayId, outputId, sectionId) {
	if (!wsManager.isConnected()) {
		return
	}

	if (!positions || !positions.length) {
		return
	}

	const payload = {
		type: 'layout_update',
		positions: positions,
	}

	if (sectionId && relayId && outputId) {
		payload.relay_id = relayId
		payload.output_id = outputId
		payload.section = sectionId
	}

	wsManager.send(JSON.stringify(payload))
}

function sendChangeSection(relayId, outputId, sectionId) {
	if (!wsManager.isConnected()) {
		return
	}

	if (!relayId || !outputId || !sectionId) {
		return
	}

	wsManager.send(JSON.stringify({
		type: 'change_section',
		relay_id: relayId,
		output_id: outputId,
		section: sectionId,
	}))
}

// Navigation functions are now in common.js

function showChangeSectionModal() {
	document.getElementById('changeSectionModal').classList.add('active')
	document.getElementById('buttons').value = ''
	document.getElementById('sections').value = ''
	document.getElementById('buttons').focus()
	document.addEventListener('keydown', function handleModalKeys(e) {
		if (e.key === 'Escape') {
			closeModal('changeSectionModal')
			document.removeEventListener('keydown', handleModalKeys)
		} else if (e.key === 'Enter') {
			submitChangeSectionForm()
			document.removeEventListener('keydown', handleModalKeys)
		}
	})
}

function showAddSectionModal() {
	document.getElementById('addSectionModal').classList.add('active')
	document.getElementById('sectionName').value = ''
	document.addEventListener('keydown', function handleModalKeys(e) {
		if (e.key === 'Escape') {
			closeModal('addSectionModal')
			document.removeEventListener('keydown', handleModalKeys)
		} else if (e.key === 'Enter') {
			submitAddSectionForm()
			document.removeEventListener('keydown', handleModalKeys)
		}
	})
}

function closeModal(modalId) {
	document.getElementById(modalId).classList.remove('active')
}

function submitChangeSectionForm() {
	var buttonOutput = document.getElementById('buttons').value.trim()
	var buttonSection = document.getElementById('sections').value.trim()
	console.log('Changing section of', buttonOutput, 'to', buttonSection)

	if (buttonOutput === '' || buttonSection === '') {
		alert('Please fill in all fields.')
		return
	}

	if (wsManager.send(JSON.stringify({ "type": "change_section", "relay_id": buttonOutput.slice(0, -2), "output_id": buttonOutput.slice(-1), "section": buttonSection }))) {
		closeModal('changeSectionModal')
	}
}

function submitAddSectionForm() {
	var sectionName = document.getElementById('sectionName').value.trim()

	if (sectionName === '') {
		alert('Please enter a section name.')
		return
	}

	if (wsManager.send(JSON.stringify({ "type": "add_section", "name": sectionName }))) {
		closeModal('addSectionModal')
	}
}

function makeSectionsCollapsible() {
	const headers = document.querySelectorAll('.room h2');

	headers.forEach(header => {
		// Prevent duplicate listeners
		if (header.dataset.collapsibleBound) return;

		header.dataset.collapsibleBound = "true";

		header.addEventListener('click', function () {
			const grid = this.nextElementSibling;
			if (!grid) return;

			this.classList.toggle('collapsed');
			grid.classList.toggle('collapsed');
		});
	});
}