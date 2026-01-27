// Enhanced Chart.js styling for the new dark theme
Chart.defaults.color = '#cbd5e0'
Chart.defaults.font.family = '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Inter", sans-serif'
Chart.defaults.font.size = 13

// Initialize WebSocket manager
var wsManager = new WebSocketManager('/heating/ws/', function (event) {
	try {
		var msg = JSON.parse(event.data)
		console.log('WebSocket data received:', msg)

		const date = new Date()

		let year = date.getFullYear()
		let month = date.getMonth() + 1
		let day = date.getDate()
		let hour = date.getHours()
		let minute = date.getMinutes()
		let second = date.getSeconds()
		year = ('0000' + year).slice(-4)
		month = ('00' + month).slice(-2)
		day = ('00' + day).slice(-2)
		hour = ('00' + hour).slice(-2)
		minute = ('00' + minute).slice(-2)
		second = ('00' + second).slice(-2)
		let currentDate = `${year}-${month}-${day} ${hour}:${minute}:${second}`

		// Update chart data
		chart.data.labels.push(currentDate)
		chart.data.datasets[0].data.push(msg["cold"])
		chart.data.datasets[1].data.push(msg["mixed"])
		chart.data.datasets[2].data.push(msg["hot"])
		chart.data.datasets[3].data.push(msg["target"])

		// Limit chart data points to last 50 to prevent performance issues
		if (chart.data.labels.length > 50) {
			chart.data.labels.shift()
			chart.data.datasets.forEach(dataset => {
				dataset.data.shift()
			})
		}

		chart.update('none') // Update without animation for better performance

		// Update ALL display fields from the WebSocket message
		for (let id in msg) {
			const element = document.getElementById(id)
			if (element) {
				// Format numbers to 2 decimal places if it's a number
				let value = msg[id]
				if (typeof value === 'number') {
					value = value.toFixed(2)
				}

				// Update the element content
				element.textContent = value

				// Add a subtle flash effect when value updates
				element.style.transition = 'color 0.3s ease'
				const originalColor = '#667eea'
				element.style.color = '#10b981'
				setTimeout(() => {
					element.style.color = originalColor
				}, 300)
			}
		}
	} catch (error) {
		console.error('Error processing WebSocket message:', error)
	}
})


const data = {
	labels: [],
	datasets: [{
		label: 'Cold water',
		data: [],
		fill: false,
		borderColor: 'rgb(100, 220, 230)',
		backgroundColor: 'rgba(100, 220, 230, 0.1)',
		tension: 0.4,
		borderWidth: 2,
		pointRadius: 0,
		pointHoverRadius: 5,
		pointHoverBackgroundColor: 'rgb(100, 220, 230)',
		pointHoverBorderColor: '#fff',
		pointHoverBorderWidth: 2
	}, {
		label: 'Mixed water',
		data: [],
		fill: false,
		borderColor: 'rgb(102, 126, 234)',
		backgroundColor: 'rgba(102, 126, 234, 0.1)',
		tension: 0.4,
		borderWidth: 2,
		pointRadius: 0,
		pointHoverRadius: 5,
		pointHoverBackgroundColor: 'rgb(102, 126, 234)',
		pointHoverBorderColor: '#fff',
		pointHoverBorderWidth: 2
	}, {
		label: 'Hot water',
		data: [],
		fill: false,
		borderColor: 'rgb(239, 68, 68)',
		backgroundColor: 'rgba(239, 68, 68, 0.1)',
		tension: 0.4,
		borderWidth: 2,
		pointRadius: 0,
		pointHoverRadius: 5,
		pointHoverBackgroundColor: 'rgb(239, 68, 68)',
		pointHoverBorderColor: '#fff',
		pointHoverBorderWidth: 2
	}, {
		label: 'Target temperature',
		data: [],
		fill: false,
		borderColor: 'rgb(16, 185, 129)',
		backgroundColor: 'rgba(16, 185, 129, 0.1)',
		tension: 0.4,
		borderWidth: 2,
		borderDash: [5, 5],
		pointRadius: 0,
		pointHoverRadius: 5,
		pointHoverBackgroundColor: 'rgb(16, 185, 129)',
		pointHoverBorderColor: '#fff',
		pointHoverBorderWidth: 2
	}]
}

const now = new Date()
const oneHourAgo = new Date(now.getTime() - 60 * 60 * 1000)

const startParam = Math.floor(oneHourAgo.getTime() / 1000)
const endParam = Math.floor(now.getTime() / 1000)


fetch(`https://${window.location.hostname}/api/temperatures?start=${startParam}&end=${endParam}&step=1`)
	.then(response => response.json())
	.then(data => {
		// data is expected to be a list of dicts with keys: cold, mixed, hot, target, timestamp
		console.log('Chart data loaded:', data)
		const labels = data.map(item => {
			const date = new Date(item.timestamp * 1000)
			let year = date.getFullYear()
			let month = date.getMonth() + 1
			let day = date.getDate()
			let hour = date.getHours()
			let minute = date.getMinutes()
			let second = date.getSeconds()
			year = ('0000' + year).slice(-4)
			month = ('00' + month).slice(-2)
			day = ('00' + day).slice(-2)
			hour = ('00' + hour).slice(-2)
			minute = ('00' + minute).slice(-2)
			second = ('00' + second).slice(-2)

			return `${year}-${month}-${day} ${hour}:${minute}:${second}`
		})
		const cold = data.map(item => item.cold)
		const mixed = data.map(item => item.mixed)
		const hot = data.map(item => item.hot)
		const target = data.map(item => item.target)

		chart.data.labels = labels
		chart.data.datasets[0].data = cold
		chart.data.datasets[1].data = mixed
		chart.data.datasets[2].data = hot
		chart.data.datasets[3].data = target
		chart.update()
	})
	.catch(error => {
		console.error('Error loading chart data:', error)
	})

const ctx = document.getElementById('myChart')
var chart = new Chart(ctx, {
	type: 'line',
	data: data,
	options: {
		responsive: true,
		maintainAspectRatio: true,
		aspectRatio: 2,
		interaction: {
			mode: 'index',
			intersect: false,
		},
		plugins: {
			legend: {
				display: true,
				position: 'top',
				labels: {
					padding: 15,
					usePointStyle: true,
					pointStyle: 'circle',
					font: {
						size: 13,
						weight: '500'
					},
					color: '#cbd5e0'
				}
			},
			tooltip: {
				enabled: true,
				backgroundColor: 'rgba(26, 31, 46, 0.95)',
				titleColor: '#fff',
				bodyColor: '#cbd5e0',
				borderColor: 'rgba(102, 126, 234, 0.5)',
				borderWidth: 1,
				padding: 12,
				displayColors: true,
				callbacks: {
					label: function (context) {
						let label = context.dataset.label || ''
						if (label) {
							label += ': '
						}
						if (context.parsed.y !== null) {
							label += context.parsed.y.toFixed(2) + '°C'
						}
						return label
					}
				}
			}
		},
		scales: {
			x: {
				grid: {
					display: true,
					color: 'rgba(255, 255, 255, 0.08)',
					drawBorder: false
				},
				ticks: {
					color: '#a0aec0',
					maxRotation: 45,
					minRotation: 45,
					font: {
						size: 11
					}
				}
			},
			y: {
				grid: {
					display: true,
					color: 'rgba(255, 255, 255, 0.08)',
					drawBorder: false
				},
				ticks: {
					color: '#a0aec0',
					callback: function (value) {
						return value.toFixed(2) + '°C'
					}
				}
			}
		},
		transitions: {
			show: {
				animations: {
					x: { from: 0 },
					y: { from: 0 }
				}
			},
			hide: {
				animations: {
					x: { to: 0 },
					y: { to: 0 }
				}
			}
		}
	}
})

chart.options.animation = false

// Connect and start connection monitoring
wsManager.connect()
wsManager.startConnectionCheck()
wsManager.setupVisibilityHandler()

function send_value(prevalue, value) {
	// Validate input: must be numeric with optional decimal point and minus sign
	for (var i = 0; i < value.length; i++) {
		if (((value[i] < '0') || (value[i] > '9')) && (value[i] != '.') && (value[i] != '-')) {
			console.warn("Invalid input: only numbers, decimal points, and minus signs allowed")
			showNotification('Invalid input', 'error')
			return 0
		}
	}

	if (value.length == 0) {
		console.warn("Empty input")
		showNotification('Please enter a value', 'error')
		return 0
	}

	// Sprawdź czy WebSocket jest podłączony
	if (!wsManager.isConnected()) {
		console.log('WebSocket not connected, trying to reconnect...')
		showNotification('Connection lost, reconnecting...', 'error')
		return 0
	}

	console.info('Sending:', prevalue + value)

	if (wsManager.send(JSON.stringify(prevalue + value))) {
		showNotification('Value sent successfully', 'success')
	} else {
		showNotification('Failed to send value', 'error')
	}
}

// Show visual feedback for user actions
function showNotification(message, type) {
	// Create notification element if it doesn't exist
	let notification = document.getElementById('notification')
	if (!notification) {
		notification = document.createElement('div')
		notification.id = 'notification'
		notification.style.cssText = `
			position: fixed;
			top: 80px;
			right: 20px;
			padding: 12px 20px;
			border-radius: 8px;
			font-weight: 500;
			z-index: 1000;
			opacity: 0;
			transition: opacity 0.3s ease;
			box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
		`
		document.body.appendChild(notification)
	}

	// Set color based on type
	if (type === 'success') {
		notification.style.background = 'rgba(16, 185, 129, 0.9)'
		notification.style.color = '#fff'
	} else {
		notification.style.background = 'rgba(239, 68, 68, 0.9)'
		notification.style.color = '#fff'
	}

	notification.textContent = message
	notification.style.opacity = '1'

	setTimeout(() => {
		notification.style.opacity = '0'
	}, 2000)
}