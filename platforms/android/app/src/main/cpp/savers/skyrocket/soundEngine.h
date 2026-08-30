/*
 * Stub for Skyrocket's sound engine.
 *
 * Upstream drives OpenAL and carries roughly 7MB of firework samples baked into headers. The
 * port runs with dSound = 0, so soundengine stays null and every call site is already guarded
 * by "if(soundengine)" -- nothing here is ever reached. The class exists only so the type
 * resolves; the real implementation and the sample data are not built.
 *
 * If audio is wanted later it should go through Oboe rather than OpenAL, which is what the rest
 * of the app already uses.
 */
#ifndef SOUND_H
#define SOUND_H

#include <rsMath/rsMath.h>

/* The sound ids stay: particle.cpp names them when asking for a sound, so they have to resolve
 * even though nothing plays. */
#define LAUNCH1SOUND 0
#define LAUNCH2SOUND 1
#define BOOM1SOUND   2
#define BOOM2SOUND   3
#define BOOM3SOUND   4
#define BOOM4SOUND   5
#define POPPERSOUND  6
#define SUCKSOUND    7
#define NUKESOUND    8
#define WHISTLESOUND 9

class SoundEngine {
public:
    SoundEngine(void *, float) {}
    ~SoundEngine() {}
    void insertSoundNode(int, rsVec, rsVec) {}
    void update(float *, float *, float *, float, bool) {}
};

#endif
