from flask import Flask, jsonify, render_template, request
from flask_cors import CORS
import threading
from datetime import datetime

app = Flask(__name__)
CORS(app)

sensor_data = {
    'temperature': 0.0,
    'pm25': 0.0,
    'pm10': 0.0,
    'humidity': 0.0,
    'predicted_pm25': 0.0,
    'timestamp': 'N/A'
}

historical_data = []
lock = threading.Lock()

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/data', methods=['POST'])
def handle_data():
    try:
        data = request.get_json()
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        
        with lock:
            # Use .get() with defaults to prevent KeyError
            sensor_data['temperature'] = data.get('temperature', 0.0)
            sensor_data['pm25'] = data.get('pm25', 0.0)
            sensor_data['pm10'] = data.get('pm10', 0.0)
            sensor_data['humidity'] = data.get('humidity', 0.0)
            sensor_data['predicted_pm25'] = data.get('predicted_pm25', 0.0)
            sensor_data['timestamp'] = timestamp
            
            historical_data.append({
                'temperature': sensor_data['temperature'],
                'pm25': sensor_data['pm25'],
                'pm10': sensor_data['pm10'],
                'humidity': sensor_data['humidity'],
                'predicted_pm25': sensor_data['predicted_pm25'],
                'timestamp': timestamp
            })
        
        return jsonify(success=True), 200
    except Exception as e:
        print(f"Error: {str(e)}")  # Add debug print
        return jsonify(error=str(e)), 400

@app.route('/getdata')
def get_data():
    with lock:
        return jsonify({
            'current': sensor_data,
            'history': historical_data[-50:]  # Keep last 50 entries
        })

@app.route('/cleardata', methods=['POST'])
def clear_data():
    global historical_data
    with lock:
        historical_data = []
    return jsonify(success=True), 200

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=10000, debug=True)