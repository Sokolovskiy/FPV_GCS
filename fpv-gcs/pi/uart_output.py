"""Explicit opt-in CRSF UART output. Importing this module never opens a port."""
from crsf_encoder import BAUD, encode_channels
import os

PORT = "/dev/serial0"


class UartOutputError(RuntimeError):
    pass


def open_serial(baudrate=BAUD):
    import serial  # Loaded only by explicit physical-output construction.
    port = serial.Serial(port=PORT, baudrate=baudrate, timeout=0, write_timeout=0,
                         bytesize=8, parity="N", stopbits=1, exclusive=True,
                         xonxoff=False, rtscts=False, dsrdtr=False)
    try:
        os.set_blocking(port.fileno(), False)
    except Exception:
        port.close()
        raise
    return NonblockingPort(port)


class NonblockingPort:
    """One nonblocking OS write; no PySerial post-write select/timeout or retry."""
    def __init__(self, serial_port):
        self.serial = serial_port

    @property
    def out_waiting(self):
        return self.serial.out_waiting

    def write(self, frame):
        return os.write(self.serial.fileno(), frame)

    def reset_output_buffer(self):
        self.serial.reset_output_buffer()

    def close(self):
        self.serial.close()


class UartOutput:
    def __init__(self, watchdog, factory=open_serial):
        self.watchdog = watchdog
        self.port = None
        self.sent = 0
        self._transmitting = False
        self.failed = False
        try:
            self.port = factory()
        except Exception as error:
            self._fail(error)

    def _fail(self, error):
        self.failed = True
        self.watchdog.force_failsafe()
        try:
            self.close()
        except UartOutputError:
            pass
        raise UartOutputError(f"UART fault; commands invalidated, restart required: {error}") from error

    def block_if_needed(self):
        if self.watchdog.commands() is not None:
            return False
        if self._transmitting and self.port is not None:
            try:
                self.port.reset_output_buffer()
            except Exception as error:
                self._fail(error)
        self._transmitting = False
        return True

    def send(self, frame):
        if self.failed or self.port is None:
            raise UartOutputError("UART is closed or faulted; restart required")
        if self.block_if_needed():
            return False
        commands = self.watchdog.commands()
        if commands is None:
            return False
        # A delayed/cached frame must never be sent for a newer command snapshot.
        if frame != encode_channels(commands):
            self._fail(ValueError("frame does not match current commands"))
        if self.watchdog.commands() is None:
            return False
        try:
            if self.port.out_waiting:
                raise OSError("UART backlog: previous frame still queued; refusing stale accumulation")
            written = self.port.write(frame)
            if written != len(frame):
                raise OSError(f"short UART write: {written}/{len(frame)}")
        except Exception as error:
            self._fail(error)
        self.sent += 1
        self._transmitting = True
        return True

    def close(self):
        port, self.port = self.port, None
        self._transmitting = False
        if port is None:
            return
        error = None
        try:
            port.reset_output_buffer()  # Discard unsent bytes; never flush stale commands.
        except Exception as exc:
            error = exc
        try:
            port.close()
        except Exception as exc:
            error = exc
        if error is not None:
            self.failed = True
            self.watchdog.force_failsafe()
            raise UartOutputError(f"UART close fault: {error}") from error

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
