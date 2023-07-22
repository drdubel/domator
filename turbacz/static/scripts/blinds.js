var wsId = Math.floor(Math.random() * 2000000000)
var ws = new WebSocket("ws://127.0.0.1:8000/blinds/ws/" + wsId);

ws.onmessage = function (event) {
	var msg = JSON.parse(event.data)
	console.info(msg)
	$("#" + msg.blind).slider("value", 999 - msg.current_position)
}

$(function () {
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
				ws.send(JSON.stringify({ "blind": event.target.id, "position": parseInt(999 - ui.value) }))
			}
		})
	})
})

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