// Initialize WebSocket manager
var wsManager = new WebSocketManager('/blinds/ws/', function (event) {
	var msg = JSON.parse(event.data)
	console.info(msg)
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