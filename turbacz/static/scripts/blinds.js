// cardIndex maps "relayId:powerId:dirId" -> {card, powerState, dirState}
var cardIndex = {}

// Initialize WebSocket manager
var wsManager = new WebSocketManager('/blinds/ws/', function (event) {
	var msg = JSON.parse(event.data)
	console.info(msg)

	if (msg.type === 'relay_blinds') {
		renderRelayBlinds(msg.pairs)
		return
	}

	if (msg.type === 'light_state') {
		handleLightState(msg.relay_id, msg.output_id, msg.state)
		return
	}

	// Legacy position slider update
	$("#" + msg.blind).slider("value", 999 - msg.current_position)
})

// Connect and start connection monitoring
wsManager.connect()
wsManager.startConnectionCheck()
wsManager.setupVisibilityHandler()

$(function () {
	// Initialize sliders
	$(".flex-container > span").each(function () {
		var value = parseInt($(this).text(), 10)
		$(this).empty().slider({
			value: value,
			range: "max",
			max: 999,
			animate: true,
			orientation: "vertical",
			stop: function (event, ui) {
				console.info(event.target.id, ui.value)

				if (!wsManager.isConnected()) {
					return
				}

				wsManager.send(JSON.stringify({ "blind": event.target.id, "position": parseInt(999 - ui.value) }))
			}
		})
	})
})

function handleLightState(relayId, outputId, state) {
	for (var key in cardIndex) {
		var entry = cardIndex[key]
		if (entry.relayId !== relayId) continue

		var changed = false
		if (outputId === entry.powerId) {
			entry.powerState = state
			changed = true
		} else if (outputId === entry.dirId) {
			entry.dirState = state
			changed = true
		}
		if (changed) applyCardState(entry)
	}
}

function applyCardState(entry) {
	var controls = entry.card.querySelector('.rblind-controls')
	controls.classList.remove('state-up', 'state-down', 'state-stopped')

	var p = entry.powerState
	var d = entry.dirState

	if (p === null || d === null) return

	if (p === 0) {
		controls.classList.add('state-stopped')
	} else if (d === 0) {
		controls.classList.add('state-up')
	} else {
		controls.classList.add('state-down')
	}
}

function renderRelayBlinds(pairs) {
	var section = document.getElementById('relay-blinds-section')
	var container = document.getElementById('relay-blinds-cards')

	cardIndex = {}

	if (!pairs || pairs.length === 0) {
		section.style.display = 'none'
		return
	}

	container.innerHTML = ''

	pairs.forEach(function (pair) {
		var card = document.createElement('div')
		card.className = 'relay-blind-card'
		card.dataset.relayId = pair.relay_id
		card.dataset.powerId = pair.power_id
		card.dataset.directionId = pair.direction_id

		card.innerHTML =
			'<div class="rblind-header">' +
				'<div class="rblind-name-wrap">' +
					'<span class="rblind-name-text">' + escapeHtml(pair.name) + '</span>' +
					'<input class="rblind-name-input" type="text" value="' + escapeHtml(pair.name) + '" style="display:none">' +
					'<button class="rblind-edit-btn" title="Rename">✎</button>' +
					'<button class="rblind-save-btn" title="Save" style="display:none">✓</button>' +
					'<button class="rblind-cancel-btn" title="Cancel" style="display:none">✕</button>' +
				'</div>' +
				'<div class="rblind-subname">' + escapeHtml(pair.relay_name) + '</div>' +
			'</div>' +
			'<div class="rblind-controls">' +
				'<button class="rblind-btn rblind-up" title="Up">' +
					'<span class="rblind-btn-icon">▲</span>' +
					'<span class="rblind-btn-label">Up</span>' +
				'</button>' +
				'<button class="rblind-btn rblind-stop" title="Stop">' +
					'<span class="rblind-btn-icon">■</span>' +
					'<span class="rblind-btn-label">Stop</span>' +
				'</button>' +
				'<button class="rblind-btn rblind-down" title="Down">' +
					'<span class="rblind-btn-icon">▼</span>' +
					'<span class="rblind-btn-label">Down</span>' +
				'</button>' +
			'</div>'

		// Register in cardIndex for state tracking
		var indexKey = pair.relay_id + ':' + pair.power_id + ':' + pair.direction_id
		cardIndex[indexKey] = {
			card: card,
			relayId: pair.relay_id,
			powerId: pair.power_id,
			dirId: pair.direction_id,
			powerState: null,
			dirState: null,
		}

		// Control buttons
		card.querySelector('.rblind-up').addEventListener('click', function () {
			sendRelayBlindControl(pair, 'up')
		})
		card.querySelector('.rblind-stop').addEventListener('click', function () {
			sendRelayBlindControl(pair, 'stop')
		})
		card.querySelector('.rblind-down').addEventListener('click', function () {
			sendRelayBlindControl(pair, 'down')
		})

		// Rename logic
		var nameText = card.querySelector('.rblind-name-text')
		var nameInput = card.querySelector('.rblind-name-input')
		var editBtn = card.querySelector('.rblind-edit-btn')
		var saveBtn = card.querySelector('.rblind-save-btn')
		var cancelBtn = card.querySelector('.rblind-cancel-btn')

		function startEdit() {
			nameText.style.display = 'none'
			editBtn.style.display = 'none'
			nameInput.style.display = ''
			saveBtn.style.display = ''
			cancelBtn.style.display = ''
			nameInput.focus()
			nameInput.select()
		}

		function cancelEdit() {
			nameInput.value = nameText.textContent
			nameInput.style.display = 'none'
			saveBtn.style.display = 'none'
			cancelBtn.style.display = 'none'
			nameText.style.display = ''
			editBtn.style.display = ''
		}

		function saveEdit() {
			var newName = nameInput.value.trim()
			if (!newName) { cancelEdit(); return }

			var body = new URLSearchParams()
			body.append('relay_id', pair.relay_id)
			body.append('output_id_power', pair.power_id)
			body.append('name', newName)

			fetch('/lights/rename_blind_pair', { method: 'POST', body: body })
				.then(function (r) { return r.json() })
				.then(function (data) {
					if (data.status === 'Blind pair renamed') {
						nameText.textContent = newName
						pair.name = newName
					}
					cancelEdit()
				})
				.catch(function () { cancelEdit() })
		}

		editBtn.addEventListener('click', startEdit)
		nameText.addEventListener('dblclick', startEdit)
		saveBtn.addEventListener('click', saveEdit)
		cancelBtn.addEventListener('click', cancelEdit)
		nameInput.addEventListener('keydown', function (e) {
			if (e.key === 'Enter') saveEdit()
			if (e.key === 'Escape') cancelEdit()
		})

		container.appendChild(card)
	})

	section.style.display = 'block'
}

function sendRelayBlindControl(pair, action) {
	if (!wsManager.isConnected()) return
	wsManager.send(JSON.stringify({
		type: 'relay_blind_control',
		relay_id: pair.relay_id,
		power_id: pair.power_id,
		direction_id: pair.direction_id,
		action: action
	}))
}

function escapeHtml(str) {
	var d = document.createElement('div')
	d.appendChild(document.createTextNode(str))
	return d.innerHTML
}
