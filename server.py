#!/usr/bin/env python3
"""
Lab 7 - Weather Station Server
Run on Raspberry Pi or Laptop

Usage:
    python3 server.py              # Auto-detect location from IP
    python3 server.py "Santa Cruz" # Manual location override
"""

import http.server
import socketserver
from urllib.parse import urlparse
import urllib.request
import json
import sys
import socket
from datetime import datetime

PORT = 1234
LOCATION = None

def get_location_from_ip():
    """Auto-detect location from public IP using ipinfo.io"""
    try:
        with urllib.request.urlopen("https://ipinfo.io/json", timeout=5) as response:
            data = json.loads(response.read().decode())
            city = data.get("city", "Unknown")
            region = data.get("region", "")
            if city and region:
                return f"{city}, {region}"
            return city
    except Exception as e:
        print(f"Failed to get location from IP: {e}")
        return "Santa Cruz"  # Fallback

class WeatherHandler(http.server.BaseHTTPRequestHandler):
    
    def log_message(self, format, *args):
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] {args[0]}")
    
    def do_GET(self):
        parsed_path = urlparse(self.path)
        path = parsed_path.path
        
        if path == '/location':
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.end_headers()
            self.wfile.write(LOCATION.encode())
            print(f"[GET /location] Responded: {LOCATION}")
        else:
            self.send_response(404)
            self.end_headers()
    
    def do_POST(self):
        parsed_path = urlparse(self.path)
        path = parsed_path.path
        
        content_length = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_length).decode('utf-8')
        
        params = {}
        if post_data:
            for item in post_data.split('&'):
                if '=' in item:
                    key, value = item.split('=', 1)
                    params[key] = value
        
        if path == '/' or path == '/temperature':
            # Lab 7.2
            sensor_temp = params.get('sensor_temp', 'N/A')
            print("\n" + "="*50)
            print("     LAB 7.2 - TEMPERATURE RECEIVED")
            print("="*50)
            print(f"  ESP32 Sensor Temp: {sensor_temp}°C")
            print("="*50 + "\n")
            
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'OK')
            
        elif path == '/weather':
            # Lab 7.3
            location = params.get('location', 'Unknown').replace('+', ' ')
            outdoor_temp = params.get('outdoor_temp', 'N/A')
            sensor_temp = params.get('sensor_temp', 'N/A')
            
            print("\n" + "="*50)
            print("     LAB 7.3 - WEATHER REPORT")
            print("="*50)
            print(f"  Location:            {location}")
            print(f"  Outdoor Temperature: {outdoor_temp}°C")
            print(f"  Sensor Temperature:  {sensor_temp}°C")
            print("="*50 + "\n")
            
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'OK')
        else:
            self.send_response(404)
            self.end_headers()


def get_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return "127.0.0.1"


def main():
    global LOCATION
    
    if len(sys.argv) > 1:
        LOCATION = sys.argv[1]
        print(f"Using manual location: {LOCATION}")
    else:
        print("Auto-detecting location from IP...")
        LOCATION = get_location_from_ip()
        print(f"Detected location: {LOCATION}")
    
    ip = get_ip()
    
    print("\n" + "="*50)
    print("       WEATHER STATION SERVER")
    print("="*50)
    print(f"  IP:       {ip}")
    print(f"  Port:     {PORT}")
    print(f"  Location: {LOCATION}")
    print("="*50)
    print(f"\nTest: curl http://{ip}:{PORT}/location")
    print("="*50 + "\n")
    
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), WeatherHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")


if __name__ == "__main__":
    main()
