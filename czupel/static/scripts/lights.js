var wsId = Math.floor(Math.random() * 2000000000)
var ws = new WebSocket(`wss://${window.location.host}/lights/ws/` + wsId)
var lights = { "s0": 0, "s1": 0, "s2": 0, "s3": 0, "s4": 0, "s5": 0, "s6": 0, "s7": 0, "s8": 0, "s9": 0, "s10": 0, "s11": 0, "s12": 0, "s13": 0, "s14": 0, "s15": 0 }

ws.onmessage = function (event) {
	var msg = JSON.parse(event.data)
	console.log(msg.id, msg.state)
	switch (msg.state) {
		case 1:
			lights[msg.id] = 1
			document.getElementById(msg.id).src = "static/data/img/on.png"
			break;
		case 0:
			lights[msg.id] = 0
			document.getElementById(msg.id).src = "static/data/img/off.png"
			break;
	}
}


function changeSwitchState(id) {
	switch (lights[id]) {
		case 1:
			lights[id] = 0
			document.getElementById(id).src = "static/data/img/off.png"
			break;
		case 0:
			lights[id] = 1
			document.getElementById(id).src = "static/data/img/on.png"
			break;
	}
	var msg = JSON.stringify({ "id": id, "state": lights[id] })
	console.log(msg)
	ws.send(msg)
}

function openNav() {
	document.getElementById("sidenav").style.width = "160px";
	document.getElementById("main").style.marginLeft = "160px";
	document.getElementById("openbtn").style.visibility = "hidden";
}

function closeNav() {
	document.getElementById("sidenav").style.width = "0";
	document.getElementById("main").style.marginLeft = "0";
	document.getElementById("openbtn").style.visibility = "visible";
} 
