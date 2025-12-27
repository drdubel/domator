var wsId = Math.floor(Math.random() * 2000000000)
var ws = new WebSocket("wss://rdest.dry.pl/lights/ws/" + wsId)
var lights = { "1a": 0, "1b": 0, "1c": 0, "1d": 0, "1e": 0, "1f": 0, "1g": 0, "1h": 0, "2a": 0, "2b": 0, "2c": 0, "2d": 0, "2e": 0, "2f": 0, "2g": 0, "2h": 0, "3a": 0, "3b": 0, "3c": 0, "3d": 0, "3e": 0, "3f": 0, "3g": 0, "3h": 0 }

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

document.querySelectorAll('.editable').forEach(el => {
  el.addEventListener('click', () => {
    el.contentEditable = "true";
    el.focus();
  });

  el.addEventListener('blur', () => {
    el.contentEditable = "false";
  });

  el.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault(); // prevent line break
      el.blur(); // trigger blur to save
    }
  });
});

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
	var msg = JSON.stringify({ "id": id[0], "light": id[1], "state": lights[id] })
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
