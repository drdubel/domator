var lights = {}
var pendingClicks = new Set()
var namedOutputs = {}
var sections = {}
var active_sections = new Set()
var draggedCard = null
var preventCardToggleUntil = 0

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
			section.appendChild(grid)

			room.appendChild(section)
		})

		Object.entries(msg.named_outputs).forEach(([relayId, outputs]) => {
			const sortedOutputs = Object.entries(outputs)
				.map(([outputKey, outputMeta]) => [outputKey, parseOutputMeta(outputMeta)])
				.sort((a, b) => a[1].outputIdx - b[1].outputIdx)

			sortedOutputs.forEach(([outputKey, outputMeta]) => {
				const outputName = outputMeta.name
				const sectionId = outputMeta.sectionId
				const section = document.querySelector(`section-${sectionId} .light-grid`)
				if (!section) return

				const button = document.createElement("div")
				button.className = "light-card"
				button.draggable = true
				button.dataset.cardId = `${relayId}${outputKey}`
				button.dataset.relayId = `${relayId}`
				button.dataset.outputId = `${outputKey}`
				button.dataset.sectionId = `${sectionId}`
				button.dataset.outputIdx = `${outputMeta.outputIdx}`
				button.onclick = function () {
					if (Date.now() < preventCardToggleUntil) {
						return
					}
					changeSwitchState(`${relayId}${outputKey}`)
				}

				const img = document.createElement("img")
				img.id = `${relayId}${outputKey}`
				img.src = "/static/data/img/off.png"
				img.alt = outputName

				const label = document.createElement("span")
				label.className = "light-label"
				label.textContent = outputName

				button.appendChild(img)
				button.appendChild(label)
				section.appendChild(button)
				active_sections.add(sectionId)
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

	cards.forEach(card => {
		card.addEventListener('dragstart', function (e) {
			draggedCard = this
			this.classList.add('dragging')
			e.dataTransfer.effectAllowed = 'move'
			e.dataTransfer.setData('text/plain', this.dataset.cardId || '')
		})

		card.addEventListener('dragend', function () {
			this.classList.remove('dragging')
			draggedCard = null
		})
	})

	grids.forEach(grid => {
		grid.addEventListener('dragover', function (e) {
			e.preventDefault()
			if (!draggedCard) return

			const afterElement = getDragAfterElement(this, e.clientX, e.clientY)
			if (!afterElement) {
				this.appendChild(draggedCard)
			} else if (afterElement !== draggedCard) {
				this.insertBefore(draggedCard, afterElement)
			}
		})

		grid.addEventListener('drop', function (e) {
			e.preventDefault()
			if (!draggedCard) return

			preventCardToggleUntil = Date.now() + 250
			sendSectionPositions(this)
		})
	})
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
	const positions = cards.map((card, index) => ({
		relay_id: card.dataset.relayId,
		output_id: card.dataset.outputId,
		output_idx: index,
	}))

	if (!positions.length) {
		return
	}

	wsManager.send(JSON.stringify({ type: 'change_positions', positions: positions }))
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