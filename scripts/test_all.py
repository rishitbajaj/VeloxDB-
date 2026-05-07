import socket
import time
import threading

def request(command, label):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(("127.0.0.1", 9999))
        s.send(command.encode())
        response = s.recv(1024).decode().strip()
        print(f"[{label}] {response}")
        s.close()
    except Exception as e:
        print(f"[{label}] Error: {e}")

print("--- Step 1: Testing Multi-threading ---")
t1 = threading.Thread(target=request, args=("INSERT 101 User_A", "Client A"))
t2 = threading.Thread(target=request, args=("INSERT 102 User_B", "Client B"))
t1.start(); t2.start()
t1.join(); t2.join()
time.sleep(0.5) # Give the database 500ms to settle the threads
print("\n--- Step 2: Testing Persistence ---")
request("SELECT 101", "Verify A")
request("SELECT 102", "Verify B")

print("\n--- Step 3: Testing TTL (Wait 35s) ---")
time.sleep(35)
request("SELECT 101", "TTL Check")