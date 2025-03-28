from flask import Flask, jsonify, render_template, request
from flask_cors import CORS
import threading
from datetime import datetime

app = Flask(__name__)
CORS(app)

sensor_data = {
    'air_quality': 0,
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
            sensor_data['air_quality'] = data.get('air_quality', 0)
            sensor_data['timestamp'] = timestamp
            historical_data.append({
                'value': data.get('air_quality', 0),
                'timestamp': timestamp
            })
        
        return jsonify(success=True), 200
    except Exception as e:
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