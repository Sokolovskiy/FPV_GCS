"""Task 2 software client: send known raw values to an isolated localhost server."""
import argparse
import math
import socket
import time

from pymavlink.dialects.v20 import common as mavlink


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=14651)
    parser.add_argument("--local-port", type=int, default=14652)
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", args.local_port))
        class Writer:
            def write(self, data):
                return sock.sendto(data, ("127.0.0.1", args.port))
        sender = mavlink.MAVLink(Writer(), srcSystem=255, srcComponent=190)
        for index in range(50):
            value = round(1000 * math.sin(index * math.pi / 12))
            sender.manual_control_send(42, -value, value, index * 20, value // 2, index % 4)
            time.sleep(.04)
        print("Sent 50 MANUAL_CONTROL messages to localhost; expected rate approximately 25 Hz")


if __name__ == "__main__":
    main()
