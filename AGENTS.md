# StreamPatch

- Use PostgreSQL for concurrency tests. Coordinate competing writes
  explicitly so the tests run without timing-dependent sleeps.
- A repair must check the expected row version in the same statement
  that updates the quantity. Preserve the last applied Kafka offset.
- When a live write loses a version check, reread the row and confirm
  the event is still pending before recalculating and retrying.
