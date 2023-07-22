import json

with open("./heating_chart.json", "w") as chart_data_json:
    json.dump({"labels": [], "cold": [], "mixed": [], "hot": []}, chart_data_json)
