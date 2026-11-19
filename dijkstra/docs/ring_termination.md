# Ring termination detection

## Purpose

`RingTerminationDetector` implements Figures 6–7. A colored token travels
from process `N-1` down to process 0. A white token returning to an idle,
white leader proves that all processes are idle and that no message
invalidated the probe.

## Atomic actions

- `go_idle(i)` makes process `i` idle.
- `send_message(i, j)` requires active `i`, activates `j`, and colors `i`
  black.
- `try_start_probe()` lets process 0 whiten itself and the token, then pass
  the token to `N-1`.
- `try_pass_token(i)` is enabled when idle process `i > 0` holds the token.
  It passes the token to `i-1`; a black process blackens the token and turns
  itself white.

The implementation uses the explanation's idle condition for token passing;
the negation symbol in the supplied PDF's extracted Figure 7 may not survive
plain-text extraction.

## Detection predicate

Termination is detected when:

```text
token_location == 0
and process 0 is idle
and token_color == white
and process_color[0] == white
```

## Assumptions and limitation

- Computation-message send and receive are one instantaneous atomic action.
- Any set of processes may initially be active.
- The control ring always runs `i -> i-1`, independent of the computation
  message graph.
- Fair scheduling is required so an idle token holder eventually passes it.
