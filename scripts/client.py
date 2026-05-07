import socket

def send_to_db(command):
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(("127.0.0.1", 8888))
    client.send(command.encode())
    response = client.recv(4096).decode()
    print(f"Sent: {command} | Received: {response}")
    client.close()

print("--- VeloxDB Remote Client ---")
send_to_db("INSERT 501 RemoteUser")
send_to_db("SELECT 501")