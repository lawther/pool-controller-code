#ifndef VERSION_BROADCAST_H
#define VERSION_BROADCAST_H

// Starts a background task that broadcasts this firmware's version on the bus
// as a CMD 0x0A Firmware Version frame from our own address (0xAC1D), once at
// startup and then about once a minute — the same cadence the other devices
// repeat theirs at. The Touchscreen lists the devices it hears a 0x0A from, so
// this is what makes the bridge show up there with its version.
//
// The {major, minor} pair comes from the running build's version string
// (e.g. "v1.19.0" → 1.19); the patch number has no place in the frame.
void version_broadcast_start(void);

#endif // VERSION_BROADCAST_H
