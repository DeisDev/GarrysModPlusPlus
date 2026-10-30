#pragma once

namespace gmodpp::watchdog {

void Start();
void NotePresent();
// Samples the calling thread's stack about every millisecond for the given time on another thread, then logs the
// functions it spent most time in, by itself and with their callees.
void ProfileThisThread(int seconds);

}
