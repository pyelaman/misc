# Tree termination detection

## Purpose

`TreeTerminationDetector` implements Section 5.2. Computation messages build
an implicit acknowledgement tree rooted at the leader. The leader detects
termination when it becomes neutral.

## Neutrality and the up edge

A process is neutral when it is idle, has acknowledgements for all sent
messages, and owes no retained acknowledgement to its parent.

When a neutral non-leader receives a computation message, it becomes active
and retains that message's acknowledgement as its up edge. Additional
messages received while it is already represented in the tree are
acknowledged immediately. When it becomes idle and has no outstanding
children, it sends the retained acknowledgement upward. This can recursively
collapse an idle subtree.

## API sequence

```cpp
auto message = detector.send(source, destination);
detector.receive(message);
// destination performs computation
detector.go_idle(destination);
```

`send()` and `receive()` are separate so a message in transit remains counted
at its sender. The implementation models control acknowledgements as
immediate once allowed; delaying them does not change the detection rule.

## Assumptions

- Initially only the leader is active.
- The leader has only outgoing computation edges.
- Messages are neither lost nor delivered twice.
- A process that can send its final acknowledgement eventually does so.

`detected()` is true exactly when the leader is neutral.
