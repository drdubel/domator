var wsId = Math.floor(Math.random() * 2000000000)
var ws = new WebSocket("ws://127.0.0.1:8000/blinds/ws/" + wsId);

ws.onmessage = function (event) {
	console.info(event)
	var msg = JSON.parse(event.data)
	msg.current_position = msg.current_position
	$("#" + msg.blind).slider("value", 999 - msg.current_position)
}

$(function () {
	$("#blind > span").each(function () {
		var value = parseInt($(this).text(), 10)
		$(this).empty().slider({
			value: value,
			range: "max",
			max: 999,
			animate: true,
			orientation: "vertical",
			stop: function (event, ui) {
				console.info(event.target.id, ui.value)
				ws.send(JSON.stringify({ "blind": event.target.id, "position": parseInt(999 - ui.value) }))
			}
		})
	})
})