"""
test_locking.py - Quick integration test for simultaneous-user device locking.

Run from the project root (while uvicorn is running on port 8000):
    python test_locking.py

Uses MOCK_GITHUB=true so no real GitHub token is needed.
"""

import asyncio
import json
import sys
import urllib.request
import urllib.error

BASE = "http://127.0.0.1:8000"
WS_DEVICE = "ws://127.0.0.1:8000/ws/device"


def post(path, body):
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        BASE + path, data=data,
        headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        r = urllib.request.urlopen(req)
        return json.loads(r.read())
    except urllib.error.HTTPError as e:
        return json.loads(e.read())


def get(path):
    r = urllib.request.urlopen(BASE + path)
    return json.loads(r.read())


def ok(msg):
    print(f"  [PASS] {msg}")


def fail(msg):
    print(f"  [FAIL] {msg}")
    sys.exit(1)


async def run():
    try:
        import websockets
    except ImportError:
        print("websockets package not installed. Run: pip install websockets")
        sys.exit(1)

    print()
    print("=" * 60)
    print("   GitMusic - Device Locking Integration Test")
    print("=" * 60)

    # ----------------------------------------------------------------
    # Step 1 - Register mock ESP32
    # ----------------------------------------------------------------
    print()
    print("[1] Registering mock ESP32 device...")
    async with websockets.connect(WS_DEVICE) as esp_ws:
        await esp_ws.send(json.dumps({
            "type": "device_register",
            "device_id": "gitmusic-01"
        }))
        reg = json.loads(await esp_ws.recv())
        if reg.get("type") != "device_registered":
            fail(f"Unexpected registration response: {reg}")
        ok(f"Device registered. Status: {reg['status']}")

        # REST status check
        status = get("/api/device/status")
        if status["status"] != "AVAILABLE":
            fail(f"Expected AVAILABLE, got {status['status']}")
        ok(f"REST /api/device/status -> {status['status']}")

        # ----------------------------------------------------------------
        # Step 2 - User A connects
        # ----------------------------------------------------------------
        print()
        print("[2] User A ('demo') connecting...")
        resp_a = post("/api/connect", {"username": "demo"})
        if not resp_a.get("success"):
            fail(f"User A should succeed: {resp_a}")
        session_a = resp_a["session_id"]
        stats_a = resp_a.get("stats", {})
        ok(f"User A got session: {session_a[:8]}...")
        ok(f"Stats: streak={stats_a.get('current_streak')}, today={stats_a.get('today_contributions')}")

        # ESP32 should receive github_update
        msg = json.loads(await esp_ws.recv())
        if msg.get("type") != "github_update":
            fail(f"Expected github_update, got: {msg}")
        ok(f"ESP32 received 'github_update' for '{msg['username']}'")
        ok(f"Music payload: pattern={msg['music']['pattern']}, intensity={msg['music']['intensity']}")

        # ----------------------------------------------------------------
        # Step 3 - User B tries to connect while A owns the device
        # ----------------------------------------------------------------
        print()
        print("[3] User B ('torvalds') connecting simultaneously (should fail)...")
        resp_b = post("/api/connect", {"username": "torvalds"})
        if resp_b.get("success"):
            fail(f"User B should have been rejected but succeeded: {resp_b}")
        if resp_b.get("error") != "DEVICE_BUSY":
            fail(f"Expected DEVICE_BUSY error, got: {resp_b}")
        ok(f"User B correctly rejected: {resp_b['error']}")
        ok(f"Message: {resp_b['message']}")

        # ----------------------------------------------------------------
        # Step 4 - User A disconnects
        # ----------------------------------------------------------------
        print()
        print("[4] User A disconnecting...")
        disc = post("/api/disconnect", {"session_id": session_a})
        if not disc.get("success"):
            fail(f"Disconnect failed: {disc}")
        ok(f"Disconnected: {disc['message']}")

        # ESP32 should receive session_end
        msg = json.loads(await esp_ws.recv())
        if msg.get("type") != "session_end":
            fail(f"Expected session_end, got: {msg}")
        ok("ESP32 received 'session_end'")

        # ----------------------------------------------------------------
        # Step 5 - Device should be AVAILABLE again
        # ----------------------------------------------------------------
        status = get("/api/device/status")
        if status["status"] != "AVAILABLE":
            fail(f"Expected AVAILABLE, got {status['status']}")
        ok(f"Device is now {status['status']}")

        # ----------------------------------------------------------------
        # Step 6 - User B can now connect
        # ----------------------------------------------------------------
        print()
        print("[5] User B ('torvalds') retrying (should succeed now)...")
        resp_b2 = post("/api/connect", {"username": "torvalds"})
        if not resp_b2.get("success"):
            fail(f"User B retry should succeed: {resp_b2}")
        ok(f"User B connected! Session: {resp_b2['session_id'][:8]}...")

        # Clean up
        post("/api/disconnect", {"session_id": resp_b2["session_id"]})
        ok("Cleanup disconnect sent.")

    print()
    print("=" * 60)
    print("   All tests PASSED!")
    print("=" * 60)
    print()


if __name__ == "__main__":
    asyncio.run(run())
