var wsId = Math.floor(Math.random() * 2000000000)
var ws = new WebSocket("ws://127.0.0.1/heating/ws/" + wsId)

Chart.defaults.color = '#FFF';

const data = {
	labels: [],
	datasets: [{
		label: 'Cold water',
		data: [],
		fill: false,
		borderColor: 'rgb(100, 220, 230)',
	}, {
		label: 'Mixed water',
		data: [],
		fill: false,
		borderColor: 'rgb(0, 0, 230)',
	}, {
		label: 'Hot water',
		data: [],
		fill: false,
		borderColor: 'rgb(200, 0, 0)',
	}]
}

fetch("/static/data/heating_chart.json")
	.then(response => response.json())
	.then(data => {
		const json_chart_data = data;
		chart.data.labels = json_chart_data["labels"]
		chart.data.datasets[0].data = json_chart_data["cold"]
		chart.data.datasets[1].data = json_chart_data["mixed"]
		chart.data.datasets[2].data = json_chart_data["hot"]
	});

const ctx = document.getElementById('myChart');
var chart = new Chart(ctx, {
	type: 'line',
	data: data,
	options: {
		transitions: {
			show: {
				animations: {
					x: {
						from: 0
					},
					y: {
						from: 0
					}
				}
			},
			hide: {
				animations: {
					x: {
						to: 0
					},
					y: {
						to: 0
					}
				}
			}
		}
	}
})

chart.options.animation = false

ws.onmessage = function (event) {
	var msg = JSON.parse(event.data)
	const date = new Date();

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
	chart.data.labels.push(currentDate)
	chart.data.datasets[0].data.push(msg["cold"])
	chart.data.datasets[1].data.push(msg["mixed"])
	chart.data.datasets[2].data.push(msg["hot"])
	chart.update()
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