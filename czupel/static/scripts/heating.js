var wsId = Math.floor(Math.random() * 2000000000)
var ws = new WebSocket("ws://127.0.0.1:8000/heating/ws/" + wsId)

ws.onmessage = function (event) {
	var msg = JSON.parse(event.data)
	for (let id in msg) {
		console.log(id, msg[id]);
		document.getElementById(id).innerHTML = msg[id]
	}
}

function send_value(prevalue, value) {
	for (var i = 0; i < value.length; i++) {
		if (((value[i] < '0') | (value[i] > '9')) && (value[i] != '.') && (value[i] != '-')) {
			console.log("NIE")
			return 0
		}
	}
	if (value.length == 0) {
		console.log("NIE")
		return 0
	}
	console.info(prevalue + value)
	ws.send(JSON.stringify(prevalue + value))
}