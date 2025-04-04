from flask import Flask, jsonify, render_template, request
from flask_cors import CORS
import threading
from datetime import datetime
import pytz

app = Flask(__name__)
CORS(app)

toronto_tz = pytz.timezone('America/Toronto')

sensor_data = {
    'temperature': 0.0,
    'pm25': 0.0,
    'pm10': 0.0,
    'humidity': 0.0,
    'timestamp': 'N/A',
    'co2': 0.0,
}

aqi_data = {
    'aqi': 0,
    'aqi_prediction': 0,
}

historical_data = []
historical_aqi = []
lock = threading.Lock()

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/data', methods=['POST'])
def handle_data():
    try:
        raw_data = request.get_data(as_text=True)
        values = raw_data.strip().split(',')
        timestamp = datetime.now(toronto_tz).strftime("%Y-%m-%d %H:%M:%S")

        with lock:
            try:
                sensor_data['temperature'] = float(values[0]) if len(values) > 0 else 0.0
                sensor_data['pm25'] = int(values[1]) if len(values) > 1 else 0
                sensor_data['pm10'] = int(values[2]) if len(values) > 2 else 0
                sensor_data['humidity'] = float(values[3]) if len(values) > 3 else 0.0
                sensor_data['co2'] = int(values[4]) if len(values) > 4 else 0
                sensor_data['timestamp'] = timestamp

                historical_data.append({
                    'temperature': sensor_data['temperature'],
                    'pm25': sensor_data['pm25'],
                    'pm10': sensor_data['pm10'],
                    'humidity': sensor_data['humidity'],
                    'co2': sensor_data['co2'],
                    'timestamp': timestamp
                })
            except (ValueError, IndexError) as e:
                print(f"Error parsing values: {str(e)}, raw data: {raw_data}")

        return jsonify(success=True), 200
    except Exception as e:
        print(f"Error: {str(e)}")  # Add debug print
        return jsonify(error=str(e)), 400

@app.route('/updateaqi', methods=['POST'])
def update_aqi():
    try:
        raw_data = request.get_data(as_text=True)
        values = raw_data.strip().split(',')

        with lock:
            try:
                aqi_data['aqi'] = int(values[0]) if len(values) > 0 else 0
                aqi_data['aqi_prediction'] = int(values[1]) if len(values) > 1 else 0

                historical_aqi.append({
                    'aqi': aqi_data['aqi'],
                    'aqi_prediction': aqi_data['aqi_prediction'],
                    'timestamp': datetime.now(toronto_tz).strftime("%Y-%m-%d %H:%M:%S")
                })
            except (ValueError, IndexError) as e:
                print(f"Error parsing values: {str(e)}, raw data: {raw_data}")

        return jsonify(success=True), 200
    except Exception as e:
        print(f"Error: {str(e)}")
    

@app.route('/getdata')
def get_data():
    with lock:
        return jsonify({
            'current': sensor_data,
            'aqi_current': aqi_data,
            'history': historical_data[-50:],  # Keep last 50 entries
            'aqi_history': historical_aqi[-50:]  # Keep last 50 entries
        })

@app.route('/getonedata')
def get_one_data():
    with lock:
        response = f"{sensor_data['temperature']},{sensor_data['pm25']},{sensor_data['pm10']},{sensor_data['humidity']},{sensor_data['co2']}"
        return response, 200, {'Content-Type': 'text/plain'}

@app.route('/cleardata', methods=['POST'])
def clear_data():
    global historical_data
    with lock:
        historical_data = []
    return jsonify(success=True), 200

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=10000, debug=True)