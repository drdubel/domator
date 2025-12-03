var wsId = Math.floor(Math.random() * 2000000000)
var ws = null
var reconnectTimeout = null
var reconnectDelay = 1000
var maxReconnectDelay = 30000
var isReconnecting = false

function connectWebSocket() {
	if (isReconnecting) return
	isReconnecting = true

	console.log('Connecting WebSocket...')
	ws = new WebSocket(`wss://${window.location.host}/blinds/ws/` + wsId)

	ws.onopen = function () {
		console.log('WebSocket connected!')
		isReconnecting = false
		reconnectDelay = 1000
	}

	ws.onmessage = function (event) {
		var msg = JSON.parse(event.data)
		console.info(msg)
		$("#" + msg.blind).slider("value", 999 - msg.current_position)
	}

	ws.onerror = function (error) {
		console.error('WebSocket error:', error)
	}

	ws.onclose = function (event) {
		console.log('WebSocket disconnected')
		isReconnecting = false

		if (reconnectTimeout) {
			clearTimeout(reconnectTimeout)
		}

		console.log(`Reconnecting in ${reconnectDelay / 1000}s...`)
		reconnectTimeout = setTimeout(function () {
			connectWebSocket()
			reconnectDelay = Math.min(reconnectDelay * 2, maxReconnectDelay)
		}, reconnectDelay)
	}
}

$(function () {
	// Initialize connection
	connectWebSocket()

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
				if (!ws || ws.readyState !== WebSocket.OPEN) {
					console.log('WebSocket not connected, trying to reconnect...')
					connectWebSocket()
					return
				}

				try {
					ws.send(JSON.stringify({ "blind": event.target.id, "position": parseInt(999 - ui.value) }))
				} catch (e) {
					console.error('Failed to send:', e)
					connectWebSocket()
				}
			}
		})
	})
})

// Check connection every 30 seconds
setInterval(function () {
	if (!ws || ws.readyState === WebSocket.CLOSED) {
		console.log('WebSocket closed, reconnecting...')
		connectWebSocket()
	}
}, 30000)

// Handling visibility API
document.addEventListener('visibilitychange', function () {
	if (!document.hidden) {
		if (!ws || ws.readyState !== WebSocket.OPEN) {
			console.log('Tab visible again, checking connection...')
			connectWebSocket()
		}
	}
})

function openNav() {
	document.getElementById("sidenav").style.width = "160px"
	document.getElementById("main").style.marginLeft = "160px"
	document.getElementById("openbtn").style.visibility = "hidden"
}

function closeNav() {
	document.getElementById("sidenav").style.width = "0"
	document.getElementById("main").style.marginLeft = "0"
	document.getElementById("openbtn").style.visibility = "visible"
}