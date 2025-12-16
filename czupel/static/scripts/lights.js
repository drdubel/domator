var wsId = Math.floor(Math.random() * 2000000000)
var ws = null
var lights = { "s0": 0, "s1": 0, "s2": 0, "s3": 0, "s4": 0, "s5": 0, "s6": 0, "s7": 0, "s8": 0, "s9": 0, "s10": 0, "s11": 0, "s12": 0, "s13": 0, "s14": 0, "s15": 0, "s16": 0, "s20": 0, "s21": 0, "s22": 0, "s23": 0 }
var pendingClicks = new Set()
var reconnectTimeout = null
var reconnectDelay = 1000
var maxReconnectDelay = 30000
var isReconnecting = false

function getCookie(cname) {
	let name = cname + "=";
	let decodedCookie = decodeURIComponent(document.cookie);
	let ca = decodedCookie.split(';');
	for (let i = 0; i < ca.length; i++) {
		let c = ca[i];
		while (c.charAt(0) == ' ') {
			c = c.substring(1);
		}
		if (c.indexOf(name) == 0) {
			return c.substring(name.length, c.length);
		}
	}
	return "";
}

function connectWebSocket() {
	if (isReconnecting) return
	isReconnecting = true

	console.log('Connecting WebSocket...')
	auth_token = getCookie("access_token")
	ws = new WebSocket(`wss://${window.location.host}/lights/ws/` + wsId + `?token=` + auth_token)

	ws.onopen = function () {
		console.log('WebSocket connected!')
		isReconnecting = false
		reconnectDelay = 1000
		pendingClicks.clear()
	}

	ws.onmessage = function (event) {
		var msg = JSON.parse(event.data)
		console.log(msg.id, msg.state)

		pendingClicks.delete(msg.id)
		lights[msg.id] = msg.state
		updateLightUI(msg.id, msg.state)
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

connectWebSocket()

setInterval(function () {
	if (!ws || ws.readyState === WebSocket.CLOSED) {
		console.log('WebSocket closed, reconnecting...')
		connectWebSocket()
	}
}, 30000)

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
	if (!ws || ws.readyState !== WebSocket.OPEN) {
		console.log('WebSocket not connected, trying to reconnect...')
		connectWebSocket()
		return
	}

	if (pendingClicks.has(id)) {
		return
	}

	pendingClicks.add(id)

	var newState = lights[id] === 1 ? 0 : 1

	var msg = JSON.stringify({ "id": id, "state": newState })
	console.log(msg)

	try {
		ws.send(msg)
	} catch (e) {
		console.error('Failed to send:', e)
		pendingClicks.delete(id)
		connectWebSocket()
	}

	setTimeout(function () {
		pendingClicks.delete(id)
	}, 2000)
}

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

document.addEventListener('visibilitychange', function () {
	if (!document.hidden) {
		if (!ws || ws.readyState !== WebSocket.OPEN) {
			console.log('Tab visible again, checking connection...')
			connectWebSocket()
		}
	}
})

// Collapsible sections
document.addEventListener('DOMContentLoaded', function () {
	var headers = document.querySelectorAll('.room h2')

	headers.forEach(function (header) {
		header.addEventListener('click', function () {
			var grid = this.nextElementSibling
			this.classList.toggle('collapsed')
			grid.classList.toggle('collapsed')
		})
	})
})