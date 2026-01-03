var wsId = Math.floor(Math.random() * 2000000000)
var ws = null
var lights = {}
var pendingClicks = new Set()
var reconnectTimeout = null
var reconnectDelay = 1000
var maxReconnectDelay = 30000
var isReconnecting = false
var namedOutputs = {}
var sections = {}
var active_sections = new Set()


function getCookie(cname) {
	let name = cname + "="
	let decodedCookie = decodeURIComponent(document.cookie)
	let ca = decodedCookie.split(';')
	for (let i = 0; i < ca.length; i++) {
		let c = ca[i]
		while (c.charAt(0) == ' ') {
			c = c.substring(1)
		}
		if (c.indexOf(name) == 0) {
			return c.substring(name.length, c.length)
		}
	}
	return ""
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
		console.log('Received:', msg)

		if (msg.type == "configuration") {
			namedOutputs = msg.named_outputs || {}
			const select = document.getElementById("buttons")
			select.innerHTML = ""
			Object.entries(msg.named_outputs).forEach(([relayId, outputs]) => {
				// Optional optgroup per relay
				const group = document.createElement("optgroup")
				group.label = `Relay ${relayId}`

				Object.entries(outputs).forEach(([outputKey, outputName]) => {
					const option = document.createElement("option")

					// Value encodes relay + output
					option.value = `${relayId}:${outputKey}`
					option.textContent = `${outputName[0]}`

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
				Object.entries(outputs).forEach(([outputKey, [outputName, sectionId]]) => {
					const section = document.querySelector(`section-${sectionId} .light-grid`)
					if (!section) return

					const button = document.createElement("div")
					button.className = "light-card"
					button.onclick = function () {
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

	console.log('Changing state of', id, 'to', newState)
	var msg = JSON.stringify({ "relay_id": id.slice(0, -1), "output_id": id.slice(-1), "state": newState })
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

	ws.send(JSON.stringify({ "type": "change_section", "relay_id": buttonOutput.slice(0, -2), "output_id": buttonOutput.slice(-1), "section": buttonSection }))
	closeModal('changeSectionModal')
}

function submitAddSectionForm() {
	var sectionName = document.getElementById('sectionName').value.trim()

	if (sectionName === '') {
		alert('Please enter a section name.')
		return
	}

	ws.send(JSON.stringify({ "type": "add_section", "name": sectionName }))
	closeModal('addSectionModal')
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