"""A minimal Modbus TCP server, shared by every simulated machine here.

Extracted from heater.py when the Gymnasium bridge became the second user.
Two copies of a wire protocol is one copy that gets a bug fixed and one that
does not — and both of them would look correct in isolation.

A backend supplies two methods:

    read(register)          -> int, or None to raise an illegal-address error
    write(register, value)  -> int (the value the device accepts, echoed back),
                               or None to refuse

Refusing by returning None matters: the client compares the echo against what
it sent, so a device that silently accepts a different value than commanded is
caught rather than believed.

Standard library only, deliberately — a test dependency that has to be
installed is a test that will one day be skipped.
"""

import socket
import socketserver
import struct


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        while True:
            header = self._recv_exact(6)
            if header is None:
                return
            txn, protocol, length = struct.unpack(">HHH", header)
            if protocol != 0 or not 2 <= length <= 253:
                return
            body = self._recv_exact(length)
            if body is None:
                return
            reply = self._dispatch(txn, body)
            if reply is None:
                return
            self.request.sendall(reply)

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.request.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def _dispatch(self, txn, body):
        unit, function = body[0], body[1]
        plant = self.server.plant
        if function == 0x03 and len(body) >= 6:
            register, count = struct.unpack(">HH", body[2:6])
            value = plant.read(register) if count == 1 else None
            if value is None:
                return self._exception(txn, unit, function, 0x02)
            return self._frame(
                txn, unit, struct.pack(">BBh", function, 2, value)
            )
        if function == 0x06 and len(body) >= 6:
            register, raw = struct.unpack(">Hh", body[2:6])
            written = plant.write(register, raw)
            if written is None:
                return self._exception(txn, unit, function, 0x03)
            return self._frame(
                txn, unit, struct.pack(">Hh", register, written), function
            )
        return self._exception(txn, unit, function, 0x01)

    def _frame(self, txn, unit, pdu, function=None):
        payload = pdu if function is None else struct.pack(">B", function) + pdu
        return struct.pack(">HHHB", txn, 0, len(payload) + 1, unit) + payload

    def _exception(self, txn, unit, function, code):
        return self._frame(txn, unit, struct.pack(">BB", function | 0x80, code))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def serve(host, port, backend, banner):
    """Bind, announce the real port, and serve forever.

    The port is printed because tests pass 0 and let the kernel choose — a
    hard-coded 5502 fails mysteriously on a machine that already runs one.
    """
    server = Server((host, port), Handler)
    server.plant = backend
    print(f"{banner} on {host}:{server.server_address[1]}", flush=True)
    server.serve_forever()
