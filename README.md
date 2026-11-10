# StreamPatch

StreamPatch is a C++ CLI for repairing Kafka-derived inventory in PostgreSQL while the live consumer keeps running.

**MVP** — Replay one SKU’s events with corrected logic, preview the change, and apply it only if the row revision still matches. Verify the repair against a full replay.
