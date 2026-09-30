# AudioInputBuffer fill policy

Implemented for the ESP-beatsy Audio Library `work` branch:

- `AudioSourceFile`: fill chunk = 2 KiB, target = 2 KiB, one read per `fill()` call.
- `AudioSourceStream`: target = 32 KiB, continuous filling until 32 KiB or `WOULD_BLOCK` / EOF.
- The physical `AudioInputBuffer` capacity remains independently configurable.
- A file buffer is only refilled when its total buffered data is below 2 KiB. Adding one 2 KiB chunk therefore keeps the normal fill level below 4 KiB.
- Streaming `WOULD_BLOCK` is preserved as a normal temporary condition.
