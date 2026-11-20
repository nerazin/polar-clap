#pragma once
// Polar's keyboard input: the original's note manager (note on 0x23d60, note off 0x23a20)
// driving the device's one MIDI module, which sits in its mono list, in relative linear
// mode (0x23540). A new key always takes over. Releasing the sounding key falls back to
// the nearest held key above it, then below it, without retriggering (the gate stays
// 1.01); with no key left the gate drops to 0 and the note value holds.
// value = (key - 60) / 84: the device multiplies it by 7 for keyboard pitch (octaves) and
// adds it, scaled by FilterKbd, to the filter cutoff.

namespace polar_recovered {

struct MonoKeyboard {
    bool held[128] = {};  // manager +0x34
    int key = -1;         // sounding key, module +0x120
    float value = 0;      // module +0x35c
    float gate = 0;       // module +0x234

    void noteOn(int k) {
        if (k < 0 || k > 127) return;
        held[k] = true;
        take(k);
        gate = 1.0099999904632568f;
    }
    void noteOff(int k) {
        if (k < 0 || k > 127) return;
        held[k] = false;
        if (k != key) return;
        for (int c = k; c < 128; ++c) if (held[c]) { take(c); return; }
        for (int c = k; c >= 0; --c) if (held[c]) { take(c); return; }
        gate = 0;
    }
    void allNotesOff() { for (int k = 0; k < 128; ++k) noteOff(k); }  // 0x239f0

private:
    void take(int k) { key = k; value = float(k - 60) / 84.0f; }
};

}  // namespace polar_recovered
