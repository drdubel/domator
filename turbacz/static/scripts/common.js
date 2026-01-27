// ============================================
// COMMON UTILITIES - Turbacz
// Shared functions used across multiple pages
// ============================================

/**
 * Get a cookie value by name
 * @param {string} cname - Cookie name
 * @returns {string} Cookie value or empty string if not found
 */
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

/**
 * WebSocket Manager - Handles WebSocket connections with automatic reconnection
 */
class WebSocketManager {
	constructor(endpoint, onMessage) {
		this.wsId = Math.floor(Math.random() * 2000000000)
		this.ws = null
		this.endpoint = endpoint
		this.onMessage = onMessage
		this.reconnectTimeout = null
		this.reconnectDelay = 1000
		this.maxReconnectDelay = 30000
		this.isReconnecting = false
		this.onOpenCallback = null
		this.onCloseCallback = null
		this.onErrorCallback = null
	}

	/**
	 * Set callback for when connection opens
	 */
	onOpen(callback) {
		this.onOpenCallback = callback
	}

	/**
	 * Set callback for when connection closes
	 */
	onClose(callback) {
		this.onCloseCallback = callback
	}

	/**
	 * Set callback for when an error occurs
	 */
	onError(callback) {
		this.onErrorCallback = callback
	}

	/**
	 * Connect to the WebSocket server
	 */
	connect() {
		if (this.isReconnecting) return
		this.isReconnecting = true

		console.log('Connecting WebSocket...')
		const auth_token = getCookie("access_token")
		this.ws = new WebSocket(`wss://${window.location.host}${this.endpoint}${this.wsId}?token=${auth_token}`)

		this.ws.onopen = () => {
			console.log('WebSocket connected!')
			this.isReconnecting = false
			this.reconnectDelay = 1000
			if (this.onOpenCallback) {
				this.onOpenCallback()
			}
		}

		this.ws.onmessage = (event) => {
			if (this.onMessage) {
				this.onMessage(event)
			}
		}

		this.ws.onerror = (error) => {
			console.error('WebSocket error:', error)
			if (this.onErrorCallback) {
				this.onErrorCallback(error)
			}
		}

		this.ws.onclose = (event) => {
			console.log('WebSocket disconnected')
			this.isReconnecting = false

			if (this.reconnectTimeout) {
				clearTimeout(this.reconnectTimeout)
			}

			console.log(`Reconnecting in ${this.reconnectDelay / 1000}s...`)
			this.reconnectTimeout = setTimeout(() => {
				this.connect()
				this.reconnectDelay = Math.min(this.reconnectDelay * 2, this.maxReconnectDelay)
			}, this.reconnectDelay)

			if (this.onCloseCallback) {
				this.onCloseCallback(event)
			}
		}
	}

	/**
	 * Send a message through the WebSocket
	 */
	send(data) {
		if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
			console.log('WebSocket not connected, trying to reconnect...')
			this.connect()
			return false
		}

		try {
			this.ws.send(data)
			return true
		} catch (e) {
			console.error('Failed to send:', e)
			this.connect()
			return false
		}
	}

	/**
	 * Check if WebSocket is connected
	 */
	isConnected() {
		return this.ws && this.ws.readyState === WebSocket.OPEN
	}

	/**
	 * Start periodic connection check
	 */
	startConnectionCheck(interval = 30000) {
		setInterval(() => {
			if (!this.ws || this.ws.readyState === WebSocket.CLOSED) {
				console.log('WebSocket closed, reconnecting...')
				this.connect()
			}
		}, interval)
	}

	/**
	 * Setup visibility change handler to reconnect when tab becomes visible
	 */
	setupVisibilityHandler() {
		document.addEventListener('visibilitychange', () => {
			if (!document.hidden) {
				if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
					console.log('Tab visible again, checking connection...')
					this.connect()
				}
			}
		})
	}
}

// Navigation sidebar functions
var sidebarOpen = false

/**
 * Open the navigation sidebar
 */
function openNav() {
	if (sidebarOpen) return
	sidebarOpen = true
	const sidenav = document.getElementById("sidenav")
	const main = document.getElementById("main")
	const btn = document.querySelector(".openbtn")
	
	if (sidenav) sidenav.style.width = "160px"
	if (main) main.style.marginLeft = "160px"
	if (btn) btn.style.visibility = "hidden"
}

/**
 * Close the navigation sidebar
 */
function closeNav() {
	if (!sidebarOpen) return
	sidebarOpen = false
	const sidenav = document.getElementById("sidenav")
	const main = document.getElementById("main")
	const btn = document.querySelector(".openbtn")
	
	if (sidenav) sidenav.style.width = "0"
	if (main) main.style.marginLeft = "0"
	if (btn) btn.style.visibility = "visible"
}
