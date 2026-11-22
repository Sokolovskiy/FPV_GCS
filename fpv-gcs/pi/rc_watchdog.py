"""RC command gate with automatic, timed fresh-stream qualification."""
from enum import Enum
import math
import time


class State(str, Enum):
    ACTIVE = "ACTIVE"
    FAILSAFE = "FAILSAFE"
    RECOVERY = "RECOVERY"


class RcWatchdog:
    def __init__(self, timeout=0.300, clock=time.monotonic, *, auto_recovery=True):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be finite and positive")
        self.timeout = timeout
        self.clock = clock
        self.last_valid = None
        self._state = State.FAILSAFE
        self._candidate = None
        self.auto_recovery = auto_recovery
        self.recovery_count = 0
        self.recovery_start = None

    def reset_qualification(self):
        """A rejected/missing packet interrupts consecutive recovery packets."""
        self.recovery_count = 0
        self.recovery_start = None

    def begin_recovery(self):
        """New session: invalidate old commands even before the 300 ms deadline."""
        self._state = State.RECOVERY
        self._candidate = None
        self.reset_qualification()

    def poll(self, now=None):
        now = self.clock() if now is None else now
        if self.last_valid is None or now - self.last_valid > self.timeout:
            self._state = State.FAILSAFE
            self._candidate = None
            self.reset_qualification()
        return self._state

    def observe_valid(self, channels, now=None, *, consecutive=True):
        """Call only after packet/source/sequence validation; inspect elapsed gap first."""
        now = self.clock() if now is None else now
        self.poll(now)
        candidate = tuple(channels)
        if len(candidate) != 16 or any(not 1000 <= v <= 2000 for v in candidate):
            raise ValueError("expected 16 valid RC values")
        gap = None if self.last_valid is None else now - self.last_valid
        self.last_valid = now
        self._candidate = candidate
        if self._state == State.FAILSAFE:
            self.begin_recovery()
            self._candidate = candidate
        if self._state == State.RECOVERY:
            # 60 ms tolerates scheduling jitter but restarts qualification on a
            # substantial pause. 160 ms minimum rejects queued/burst datagrams.
            if not consecutive or gap is None or gap > 0.060 or gap < 0:
                self.reset_qualification()
            if self.recovery_start is None:
                self.recovery_start = now
            self.recovery_count += 1
            if (self.auto_recovery and self.recovery_count >= 10
                    and now - self.recovery_start >= 0.160):
                self._state = State.ACTIVE

    def acknowledge_recovery(self, now=None):
        """Legacy isolated tests only; cannot bypass automatic qualification."""
        if self.auto_recovery or self.poll(now) != State.RECOVERY:
            return False
        self._state = State.ACTIVE
        return True

    def force_failsafe(self):
        """Invalidate commands after an output fault; new packets require recovery."""
        self._state = State.FAILSAFE
        self._candidate = None
        self.last_valid = None
        self.reset_qualification()

    def commands(self, now=None):
        """Only supported current-command API: None whenever use is prohibited."""
        return self._candidate if self.poll(now) == State.ACTIVE else None
