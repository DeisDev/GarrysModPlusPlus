#pragma once

// A small window that says why the game stopped responding when its render thread has spent more than a second
// without presenting a frame waiting for this mod's work: shaders a draw needs compiled on the spot (the first launch
// after an update, before the background precache has caught up), or the ray tracing pipeline states. It appears over
// the game without taking focus, and goes once a frame is presented again.
namespace gmodpp::busy {

enum class Reason { Shaders, Pipelines };

// Marks the calling thread (the render thread) as waiting for `reason` while the scope lasts. Scopes may nest; the
// outermost counts.
class Scope {
public:
    explicit Scope(Reason reason);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool m_outer = false;
};

// Called for every frame presented.
void NotePresent();

}
