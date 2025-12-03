var wsId = Math.floor(Math.random() * 2000000000)
var ws = null
var lights = { "s0": 0, "s1": 0, "s2": 0, "s3": 0, "s4": 0, "s5": 0, "s6": 0, "s7": 0, "s8": 0, "s9": 0, "s10": 0, "s11": 0, "s12": 0, "s13": 0, "s14": 0, "s15": 0, "s16": 0, "s20": 0, "s21": 0, "s22": 0, "s23": 0 }
var pendingClicks = new Set()
var reconnectTimeout = null
var reconnectDelay = 1000 // Start with 1 second
var maxReconnectDelay = 30000 // Max 30 seconds
var isReconnecting = false

function connectWebSocket() {
	if (isReconnecting) return
	isReconnecting = true

	console.log('Connecting WebSocket...')
	ws = new WebSocket(`wss://${window.location.host}/lights/ws/` + wsId)

	ws.onopen = function () {
		console.log('WebSocket connected!')
		isReconnecting = false
		reconnectDelay = 1000 // Reset delay after successful connection

		// Clear pending clicks after connection
		pendingClicks.clear()
	}

	ws.onmessage = function (event) {
		var msg = JSON.parse(event.data)
		console.log(msg.id, msg.state)

		// Remove from pending
		pendingClicks.delete(msg.id)

		// Update state
		lights[msg.id] = msg.state
		updateLightUI(msg.id, msg.state)
	}

	ws.onerror = function (error) {
		console.error('WebSocket error:', error)
	}

	ws.onclose = function (event) {
		console.log('WebSocket disconnected')
		isReconnecting = false

		// Automatic reconnection
		if (reconnectTimeout) {
			clearTimeout(reconnectTimeout)
		}

		console.log(`Reconnecting in ${reconnectDelay / 1000}s...`)
		reconnectTimeout = setTimeout(function () {
			connectWebSocket()
			// Increase delay for next attempt (exponential backoff)
			reconnectDelay = Math.min(reconnectDelay * 2, maxReconnectDelay)
		}, reconnectDelay)
	}
}

// Initialize connection
connectWebSocket()

// Check connection every 30 seconds and resume if needed
setInterval(function () {
	if (!ws || ws.readyState === WebSocket.CLOSED) {
		console.log('WebSocket closed, reconnecting...')
		connectWebSocket()
	}
}, 30000)

// Faster UI update - only change CSS class instead of src
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
	// Check if WebSocket is connected
	if (!ws || ws.readyState !== WebSocket.OPEN) {
		console.log('WebSocket not connected, trying to reconnect...')
		connectWebSocket()
		return
	}

	// Block if already waiting for response
	if (pendingClicks.has(id)) {
		return
	}

	// Mark as pending
	pendingClicks.add(id)

	var newState = lights[id] === 1 ? 0 : 1
	lights[id] = newState
	updateLightUI(id, newState)

	// Send to server
	var msg = JSON.stringify({ "id": id, "state": newState })
	console.log(msg)

	try {
		ws.send(msg)
	} catch (e) {
		console.error('Failed to send:', e)
		lights[id] = newState === 1 ? 0 : 1
		updateLightUI(id, lights[id])
		pendingClicks.delete(id)
		// Try to reconnect
		connectWebSocket()
	}

	// Safety timeout - remove pending after 2s
	setTimeout(function () {
		pendingClicks.delete(id)
	}, 2000)
}

// Sidebar optimization
var sidebarOpen = false
function openNav() {
	if (sidebarOpen) return
	sidebarOpen = true
	document.getElementById("sidenav").style.width = "160px"
	document.getElementById("main").style.marginLeft = "160px"
	var btn = document.querySelector(".openbtn")
	if (btn) btn.style.visibility = "hidden"
}

function closeNav() {
	if (!sidebarOpen) return
	sidebarOpen = false
	document.getElementById("sidenav").style.width = "0"
	document.getElementById("main").style.marginLeft = "0"
	var btn = document.querySelector(".openbtn")
	if (btn) btn.style.visibility = "visible"
}

// Handle visibility API - detect when user returns to tab
document.addEventListener('visibilitychange', function () {
	if (!document.hidden) {
		// User returned to tab
		if (!ws || ws.readyState !== WebSocket.OPEN) {
			console.log('Tab visible again, checking connection...')
			connectWebSocket()
		}
	}
})