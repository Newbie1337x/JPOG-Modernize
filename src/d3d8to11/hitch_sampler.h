// hitch_sampler.h - where the game thread spends a long frame ([Performance] HitchSampler).
//
// A worker samples the game thread about every millisecond: it suspends it, reads its
// instruction pointer and the top of its stack, and resumes it. When a frame (Present to Present
// on the game thread) is longer than [Performance] HitchThresholdMs, the samples of that frame are
// written to the log: the code addresses where the thread was, and the return addresses found on
// its stack (callers), each as module+offset with the share of samples. With
// [Performance] HitchProfileSeconds the samples of all such frames are summed instead, and one
// report per period also lists the share of each module.
//
// While the game thread is suspended the worker only reads memory and takes no lock (no heap,
// no logging): a suspended thread may hold any lock.
#pragma once
#include "common.h"

namespace hitch_sampler {
// Called by the game thread at each Present.
void FramePresented();
void Stop();
}
