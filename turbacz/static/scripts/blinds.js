// Initialize WebSocket manager
var wsManager = new WebSocketManager('/blinds/ws/', function (event) {
	var msg = JSON.parse(event.data)
	console.info(msg)

	if (msg.type === 'relay_blinds') {
		renderRelayBlinds(msg.pairs)
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

				// Check if WebSocket is connected
				if (!wsManager.isConnected()) {
					return
				}

				wsManager.send(JSON.stringify({ "blind": event.target.id, "position": parseInt(999 - ui.value) }))
			}
		})
	})
})

function renderRelayBlinds(pairs) {
	var section = document.getElementById('relay-blinds-section')
	var container = document.getElementById('relay-blinds-cards')

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
			'<div class="relay-blind-name">' + escapeHtml(pair.relay_name) + '</div>' +
			'<div class="relay-blind-subname">' + escapeHtml(pair.power_name) + ' / ' + escapeHtml(pair.direction_name) + '</div>' +
			'<div class="relay-blind-controls">' +
				'<button class="rblind-btn rblind-up" title="Up">▲</button>' +
				'<button class="rblind-btn rblind-stop" title="Stop">■</button>' +
				'<button class="rblind-btn rblind-down" title="Down">▼</button>' +
			'</div>'

		card.querySelector('.rblind-up').addEventListener('click', function () {
			sendRelayBlindControl(pair, 'up')
		})
		card.querySelector('.rblind-stop').addEventListener('click', function () {
			sendRelayBlindControl(pair, 'stop')
		})
		card.querySelector('.rblind-down').addEventListener('click', function () {
			sendRelayBlindControl(pair, 'down')
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
