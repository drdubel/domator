// Fetch CSRF tokens only from this origin; never attach them to third parties.
let csrfTokenPromise = null
async function apiFetch(input, options = {}) {
    const url = new URL(input, window.location.href)
    const method = (options.method || 'GET').toUpperCase()
    const headers = new Headers(options.headers || {})
    if (!['GET', 'HEAD', 'OPTIONS'].includes(method)) {
        if (url.origin !== window.location.origin) throw new Error('Cross-origin mutation rejected')
        if (!csrfTokenPromise) {
            csrfTokenPromise = window.fetch('/csrf-token', { credentials: 'same-origin', cache: 'no-store' })
                .then(response => {
                    if (!response.ok) throw new Error('Session expired; please sign in again')
                    return response.json()
                }).then(data => data.token).catch(error => { csrfTokenPromise = null; throw error })
        }
        headers.set('X-CSRF-Token', await csrfTokenPromise)
    }
    const response = await window.fetch(url, { ...options, headers, credentials: 'same-origin' })
    if (response.status === 401) {
        window.location.assign('/')
        throw new Error('Session expired; please sign in again')
    }
    return response
}

document.addEventListener('DOMContentLoaded', () => {
    document.querySelectorAll('a[href="/logout"]').forEach(link => {
        link.addEventListener('click', async event => {
            event.preventDefault()
            try {
                const response = await apiFetch('/logout', { method: 'POST' })
                if (response.ok) window.location.assign('/')
            } catch (error) { window.location.assign('/') }
        })
    })
})

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
		this.connectionTimeout = null
		this.reconnectDelay = 1000
		this.maxReconnectDelay = 30000
		this.connectionTimeoutMs = 10000
		this.isReconnecting = false
		this.onOpenCallback = null
		this.onCloseCallback = null
		this.onErrorCallback = null
		this.connectionCheckInterval = null
		this.visibilityHandlerBound = false
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

		const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
		const url = `${protocol}//${window.location.host}${this.endpoint}${this.wsId}`
		const socket = new WebSocket(url)
		this.ws = socket

		console.log(`Connecting WebSocket to ${url}...`)
		this.connectionTimeout = setTimeout(() => {
			if (this.ws === socket && socket.readyState === WebSocket.CONNECTING) {
				console.warn(`WebSocket handshake timed out after ${this.connectionTimeoutMs}ms`)
				socket.close()
			}
		}, this.connectionTimeoutMs)

		socket.onopen = () => {
			this.clearConnectionTimeout()
			console.log('WebSocket connected!')
			this.isReconnecting = false
			this.reconnectDelay = 1000
			if (this.onOpenCallback) {
				this.onOpenCallback()
			}
		}

		socket.onmessage = (event) => {
			if (JSON.parse(event.data).type === 'pong') return
			if (this.onMessage) {
				this.onMessage(event)
			}
		}

		socket.onerror = (error) => {
			console.error('WebSocket error:', error)
			if (this.onErrorCallback) {
				this.onErrorCallback(error)
			}
		}

		socket.onclose = (event) => {
			this.clearConnectionTimeout()
			console.log(`WebSocket disconnected (code=${event.code}, reason=${event.reason || 'none'})`)
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

	clearConnectionTimeout() {
		if (this.connectionTimeout) {
			clearTimeout(this.connectionTimeout)
			this.connectionTimeout = null
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
		// Clear any existing interval first
		if (this.connectionCheckInterval) {
			clearInterval(this.connectionCheckInterval)
		}
		
		this.connectionCheckInterval = setInterval(() => {
			if (!this.ws || this.ws.readyState === WebSocket.CLOSED) {
				console.log('WebSocket closed, reconnecting...')
				this.connect()
			} else if (this.isConnected()) {
				this.send(JSON.stringify({ type: 'ping' }))
			}
		}, interval)
	}

	/**
	 * Setup visibility change handler to reconnect when tab becomes visible
	 */
	setupVisibilityHandler() {
		// Only add the listener once
		if (this.visibilityHandlerBound) return
		this.visibilityHandlerBound = true
		
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

/**
 * Escape a value for safe interpolation into an HTML string
 * @param {*} str - Value to escape (null/undefined become "")
 * @returns {string} HTML-escaped text
 */
function escapeHtml(str) {
	if (str === null || str === undefined) return ""
	var d = document.createElement('div')
	d.appendChild(document.createTextNode(String(str)))
	return d.innerHTML.replace(/"/g, '&quot;').replace(/'/g, '&#39;')
}
