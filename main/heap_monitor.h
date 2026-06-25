#ifndef HEAP_MONITOR_H
#define HEAP_MONITOR_H

// Starts a low-priority background task that periodically logs heap statistics
// (current free, minimum-free watermark since boot, and largest free block).
//
// The point is diagnostic history: a slow leak or growing fragmentation shows
// up as a downward trend in free/largest-block over hours or days, visible in
// the console and TCP log stream *before* it exhausts the heap and crashes —
// which is exactly the information you can't recover from a device that has
// already locked up.
void heap_monitor_start(void);

#endif // HEAP_MONITOR_H
