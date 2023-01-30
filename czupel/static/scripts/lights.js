var wsId = Math.floor(Math.random() * 2000000000)
var ws = new WebSocket("ws://127.0.0.1:8000/lights/ws/" + wsId)


ws.onmessage = function (event) {
	var msg = JSON.parse(event.data)
	console.log(msg.id, msg.state)
	document.getElementById(msg.id).checked = msg.state
}


function changeSwitchState(id, state) {
	var msg = JSON.stringify({ "id": String.fromCharCode('a'.charCodeAt(0) + parseInt(id.slice(1))), "state": state | 0 })
	console.log(msg)
	ws.send(msg)
}