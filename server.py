"""
Flask Server — runs on your LAPTOP (10.34.229.138)
Receives sensor data from ESP32 and serves a live dashboard
accessible to ALL devices on the same WiFi network.

Features:
  - Google & email login with session management
  - Profile setup (user details, patient info, medical conditions)
  - Dashboard to view/edit all user & patient data
  - Live sensor monitoring from ESP32

Usage:
  pip install flask flask-cors google-auth requests
  python server.py

Then open in any browser on the same network:
  http://10.34.229.138:5000
"""

from flask import Flask, request, jsonify, render_template, session, redirect, url_for
from flask_cors import CORS
from datetime import datetime
import threading
import json
import os
import hashlib
import secrets

# Google auth
try:
    from google.oauth2 import id_token
    from google.auth.transport import requests as google_requests
    GOOGLE_AUTH_AVAILABLE = True
except ImportError:
    GOOGLE_AUTH_AVAILABLE = False
    print("[WARN] google-auth not installed. Google login will be disabled.")

app = Flask(__name__, template_folder="templates", static_folder="static")
app.secret_key = secrets.token_hex(32)
CORS(app)

# ---------- GOOGLE CLIENT ID ----------
# Replace with your actual Google OAuth client ID from https://console.cloud.google.com/
GOOGLE_CLIENT_ID = os.environ.get("GOOGLE_CLIENT_ID", "YOUR_GOOGLE_CLIENT_ID.apps.googleusercontent.com")

# ---------- USER DATA STORE (JSON file) ----------
USERS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "users_data.json")

def load_users():
    """Load all users from JSON file."""
    if os.path.exists(USERS_FILE):
        try:
            with open(USERS_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError):
            return {}
    return {}

def save_users(users):
    """Save all users to JSON file."""
    with open(USERS_FILE, "w", encoding="utf-8") as f:
        json.dump(users, f, indent=2, ensure_ascii=False)

def hash_password(password):
    """Hash a password with SHA-256 + salt."""
    salt = secrets.token_hex(16)
    hashed = hashlib.sha256((salt + password).encode()).hexdigest()
    return salt + ":" + hashed

def verify_password(stored, password):
    """Verify a password against stored hash."""
    parts = stored.split(":", 1)
    if len(parts) != 2:
        return False
    salt, hashed = parts
    return hashlib.sha256((salt + password).encode()).hexdigest() == hashed

def get_current_user():
    """Get current logged-in user email from session."""
    return session.get("user_email")

def login_required(f):
    """Decorator to require login."""
    from functools import wraps
    @wraps(f)
    def decorated(*args, **kwargs):
        if not get_current_user():
            if request.is_json or request.path.startswith("/api/"):
                return jsonify({"status": "error", "message": "Not authenticated"}), 401
            return redirect("/login")
        return f(*args, **kwargs)
    return decorated

# ---------- SHARED DATA STORE ----------
data_lock = threading.Lock()
latest_data = {
    "mode": 0,              # 0=Voice, 1=IR, 2=Breath
    "mode_name": "VOICE",
    "relay1": False,         # Fan
    "relay2": False,         # Light
    "relay3": False,         # Emergency
    "breath_level": 0,
    "breath_count": 0,
    "ir_count": 0,
    "gsm_status": "idle",    # idle / waiting_cancel / calling
    "cancel_remaining": 0,   # seconds left to cancel GSM
    "last_voice_cmd": 0,     # last voice command ID from ESP32
    "voice_action": "",      # human-readable voice action
    "last_updated": "No data yet",
    "history": []            # last 100 readings
}

# Relay commands from the web dashboard -> ESP32 picks these up
relay_commands = {
    "relay1": None,   # None = no pending command, True = turn ON, False = turn OFF
    "relay2": None,
    "relay3": None,
}

MAX_HISTORY = 100
MODE_NAMES = {0: "VOICE", 1: "IR", 2: "BREATH"}
VOICE_ACTIONS = {
    5: "Fan ON", 6: "Fan OFF",
    7: "Light ON", 8: "Light OFF",
    11: "Emergency ON", 12: "Emergency OFF"
}


# ---------- RECEIVE DATA FROM ESP32 ----------
@app.route("/update", methods=["POST"])
def update():
    """ESP32 POSTs JSON here every 5 seconds."""
    try:
        payload = request.get_json(force=True)
        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        with data_lock:
            mode = payload.get("mode", 0)
            latest_data["mode"] = mode
            latest_data["mode_name"] = MODE_NAMES.get(mode, "UNKNOWN")
            latest_data["relay1"] = payload.get("relay1", False)
            latest_data["relay2"] = payload.get("relay2", False)
            latest_data["relay3"] = payload.get("relay3", False)
            latest_data["breath_level"] = payload.get("breath_level", 0)
            latest_data["breath_count"] = payload.get("breath_count", 0)
            latest_data["ir_count"] = payload.get("ir_count", 0)
            latest_data["gsm_status"] = payload.get("gsm_status", "idle")
            latest_data["cancel_remaining"] = payload.get("cancel_remaining", 0)

            # Voice command tracking
            voice_cmd = payload.get("last_voice_cmd", 0)
            if voice_cmd and voice_cmd != 0:
                latest_data["last_voice_cmd"] = voice_cmd
                latest_data["voice_action"] = VOICE_ACTIONS.get(voice_cmd, f"CMD {voice_cmd}")

            latest_data["last_updated"] = now

            # Keep rolling history
            latest_data["history"].append({
                "time": now,
                "mode": latest_data["mode_name"],
                "relay1": latest_data["relay1"],
                "relay2": latest_data["relay2"],
                "relay3": latest_data["relay3"],
                "breath_level": latest_data["breath_level"],
                "voice_action": latest_data.get("voice_action", ""),
            })
            if len(latest_data["history"]) > MAX_HISTORY:
                latest_data["history"] = latest_data["history"][-MAX_HISTORY:]

        r1 = "ON" if latest_data['relay1'] else "OFF"
        r2 = "ON" if latest_data['relay2'] else "OFF"
        r3 = "ON" if latest_data['relay3'] else "OFF"
        print(f"[{now}]  Mode={latest_data['mode_name']}  Fan={r1}  Light={r2}  Emergency={r3}")
        return jsonify({"status": "ok"}), 200

    except Exception as e:
        print(f"Error: {e}")
        return jsonify({"status": "error", "msg": str(e)}), 400


# ---------- API — FRONTEND POLLS THIS ----------
@app.route("/api/data", methods=["GET"])
def get_data():
    """Returns the latest sensor readings as JSON."""
    with data_lock:
        return jsonify(latest_data)


# ---------- RELAY TOGGLE FROM DASHBOARD ----------
@app.route("/api/relay", methods=["POST"])
def toggle_relay():
    """Dashboard sends {relay: 'relay1'/'relay2'/'relay3', state: true/false}."""
    try:
        payload = request.get_json(force=True)
        relay = payload.get("relay")       # "relay1", "relay2", or "relay3"
        state = payload.get("state")       # true or false

        if relay not in ("relay1", "relay2", "relay3"):
            return jsonify({"status": "error", "msg": "Invalid relay"}), 400

        with data_lock:
            relay_commands[relay] = bool(state)
            latest_data[relay] = bool(state)   # update dashboard immediately

        names = {"relay1": "Fan", "relay2": "Light", "relay3": "Emergency"}
        action = "ON" if state else "OFF"
        print(f"[DASHBOARD]  {names.get(relay, relay)} ({relay}) -> {action}")
        return jsonify({"status": "ok", "relay": relay, "state": state}), 200

    except Exception as e:
        return jsonify({"status": "error", "msg": str(e)}), 400


# ---------- ESP32 POLLS THIS FOR RELAY COMMANDS ----------
@app.route("/api/relay/status", methods=["GET"])
def relay_status():
    """ESP32 calls this to check if dashboard toggled a relay.
    Returns pending commands and clears them."""
    with data_lock:
        cmds = {
            "relay1": relay_commands["relay1"],
            "relay2": relay_commands["relay2"],
            "relay3": relay_commands["relay3"],
        }
        # Clear after ESP32 reads them
        relay_commands["relay1"] = None
        relay_commands["relay2"] = None
        relay_commands["relay3"] = None
    return jsonify(cmds)


# ---------- MODE SWITCH FROM DASHBOARD ----------
@app.route("/api/mode", methods=["POST"])
def switch_mode():
    """Dashboard sends {mode: 0/1/2} to switch control mode."""
    try:
        payload = request.get_json(force=True)
        mode = int(payload.get("mode", 0))
        if mode not in (0, 1, 2):
            return jsonify({"status": "error", "msg": "Invalid mode (0-2)"}), 400
        with data_lock:
            latest_data["mode"] = mode
            latest_data["mode_name"] = MODE_NAMES.get(mode, "UNKNOWN")
        print(f"[DASHBOARD]  Mode switched → {MODE_NAMES.get(mode)}")
        return jsonify({"status": "ok", "mode": mode, "mode_name": MODE_NAMES.get(mode)}), 200
    except Exception as e:
        return jsonify({"status": "error", "msg": str(e)}), 400


# ---------- SERVE THE DASHBOARD ----------
@app.route("/")
def index():
    if get_current_user():
        return render_template("index.html")
    return redirect("/login")


# ---------- LOGIN PAGE ----------
@app.route("/login")
def login_page():
    if get_current_user():
        return redirect("/dashboard")
    return render_template("login.html", google_client_id=GOOGLE_CLIENT_ID)


# ---------- AUTH: EMAIL SIGNUP ----------
@app.route("/auth/signup", methods=["POST"])
def auth_signup():
    try:
        payload = request.get_json(force=True)
        name = payload.get("name", "").strip()
        email = payload.get("email", "").strip().lower()
        password = payload.get("password", "")

        if not email or not password:
            return jsonify({"status": "error", "message": "Email and password are required."}), 400
        if len(password) < 6:
            return jsonify({"status": "error", "message": "Password must be at least 6 characters."}), 400

        users = load_users()
        if email in users:
            return jsonify({"status": "error", "message": "An account with this email already exists."}), 400

        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        users[email] = {
            "email": email,
            "name": name,
            "password": hash_password(password),
            "login_method": "email",
            "created_at": now,
            "last_login": now,
            "profile_complete": False,
            "user": {"name": name, "age": None, "phone": "", "relationship": ""},
            "patient": {"name": "", "age": None, "gender": "", "blood_group": "", "emergency_contact": ""},
            "medical": {"conditions": [], "medications": "", "allergies": "", "notes": ""}
        }
        save_users(users)

        session["user_email"] = email
        return jsonify({"status": "ok", "needs_profile": True}), 200

    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500


# ---------- AUTH: EMAIL LOGIN ----------
@app.route("/auth/login", methods=["POST"])
def auth_login():
    try:
        payload = request.get_json(force=True)
        email = payload.get("email", "").strip().lower()
        password = payload.get("password", "")

        users = load_users()
        user = users.get(email)

        if not user:
            return jsonify({"status": "error", "message": "No account found with this email."}), 401

        if not verify_password(user.get("password", ""), password):
            return jsonify({"status": "error", "message": "Incorrect password."}), 401

        # Update last login
        user["last_login"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        save_users(users)

        session["user_email"] = email
        needs_profile = not user.get("profile_complete", False)
        return jsonify({"status": "ok", "needs_profile": needs_profile}), 200

    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500


# ---------- AUTH: GOOGLE LOGIN ----------
@app.route("/auth/google", methods=["POST"])
def auth_google():
    try:
        if not GOOGLE_AUTH_AVAILABLE:
            return jsonify({"status": "error", "message": "Google auth not available on server."}), 500

        payload = request.get_json(force=True)
        credential = payload.get("credential")

        if not credential:
            return jsonify({"status": "error", "message": "No credential provided."}), 400

        # Verify the Google token
        idinfo = id_token.verify_oauth2_token(
            credential, google_requests.Request(), GOOGLE_CLIENT_ID
        )

        email = idinfo.get("email", "").lower()
        name = idinfo.get("name", "")
        picture = idinfo.get("picture", "")

        if not email:
            return jsonify({"status": "error", "message": "Could not get email from Google."}), 400

        users = load_users()
        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        if email not in users:
            # New Google user
            users[email] = {
                "email": email,
                "name": name,
                "password": "",
                "login_method": "google",
                "google_picture": picture,
                "created_at": now,
                "last_login": now,
                "profile_complete": False,
                "user": {"name": name, "age": None, "phone": "", "relationship": ""},
                "patient": {"name": "", "age": None, "gender": "", "blood_group": "", "emergency_contact": ""},
                "medical": {"conditions": [], "medications": "", "allergies": "", "notes": ""}
            }
        else:
            users[email]["last_login"] = now

        save_users(users)
        session["user_email"] = email
        needs_profile = not users[email].get("profile_complete", False)
        return jsonify({"status": "ok", "needs_profile": needs_profile}), 200

    except ValueError as e:
        return jsonify({"status": "error", "message": "Invalid Google token: " + str(e)}), 401
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500


# ---------- AUTH: LOGOUT ----------
@app.route("/auth/logout", methods=["POST"])
def auth_logout():
    session.clear()
    return jsonify({"status": "ok"}), 200


# ---------- PROFILE SETUP PAGE ----------
@app.route("/profile/setup")
@login_required
def profile_setup():
    email = get_current_user()
    users = load_users()
    user = users.get(email, {})
    return render_template("profile_setup.html",
                           user_email=email,
                           user_name=user.get("user", {}).get("name", ""))


# ---------- SAVE PROFILE ----------
@app.route("/profile/save", methods=["POST"])
@login_required
def profile_save():
    try:
        email = get_current_user()
        payload = request.get_json(force=True)
        users = load_users()

        if email not in users:
            return jsonify({"status": "error", "message": "User not found."}), 404

        user = users[email]
        # Merge profile data
        if "user" in payload:
            user["user"] = {**user.get("user", {}), **payload["user"]}
            if payload["user"].get("name"):
                user["name"] = payload["user"]["name"]
        if "patient" in payload:
            user["patient"] = {**user.get("patient", {}), **payload["patient"]}
        if "medical" in payload:
            user["medical"] = {**user.get("medical", {}), **payload["medical"]}

        user["profile_complete"] = True
        save_users(users)

        return jsonify({"status": "ok", "message": "Profile saved successfully."}), 200

    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500


# ---------- GET PROFILE API ----------
@app.route("/api/profile")
@login_required
def api_profile():
    email = get_current_user()
    users = load_users()
    user = users.get(email, {})
    # Don't send password
    safe_user = {k: v for k, v in user.items() if k != "password"}
    return jsonify(safe_user)


# ---------- UPDATE PROFILE API ----------
@app.route("/api/profile/update", methods=["POST"])
@login_required
def api_profile_update():
    try:
        email = get_current_user()
        payload = request.get_json(force=True)
        users = load_users()

        if email not in users:
            return jsonify({"status": "error", "message": "User not found."}), 404

        user = users[email]
        if "user" in payload:
            user["user"] = {**user.get("user", {}), **payload["user"]}
            if payload["user"].get("name"):
                user["name"] = payload["user"]["name"]
        if "patient" in payload:
            user["patient"] = {**user.get("patient", {}), **payload["patient"]}
        if "medical" in payload:
            user["medical"] = {**user.get("medical", {}), **payload["medical"]}

        save_users(users)
        return jsonify({"status": "ok"}), 200

    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500


# ---------- ALL USERS API ----------
@app.route("/api/users")
@login_required
def api_users():
    users = load_users()
    user_list = []
    for email, data in users.items():
        safe = {k: v for k, v in data.items() if k != "password"}
        user_list.append(safe)
    return jsonify(user_list)


# ---------- DASHBOARD PAGE ----------
@app.route("/dashboard")
@login_required
def dashboard():
    return render_template("dashboard.html")


# ---------- REDIRECT /user-dashboard to /dashboard ----------
@app.route("/user-dashboard")
@login_required
def user_dashboard_redirect():
    return redirect("/dashboard")


# ---------- MAIN ----------
if __name__ == "__main__":
    print("=" * 55)
    print("  SENSOR DASHBOARD SERVER")
    print("  Open from ANY device on the same WiFi:")
    print("  http://10.244.179.138:5000")
    print("=" * 55)
    # host="0.0.0.0" makes it accessible to all devices on the LAN
    app.run(host="0.0.0.0", port=5000, debug=True)
