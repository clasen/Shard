// The reference sequences, shared by midid.c (plays them to NS2), render.cpp and cmpref.cpp.
#pragma once
struct RefEvent { double t; int note, vel; double dur; };  // seconds from the start
static const struct RefEvent kRefSeq[] = {
  {0.0, 48, 40, 1.5}, {2.0, 48, 90, 1.5}, {4.0, 48, 127, 1.5},
  {6.0, 60, 40, 1.5}, {8.0, 60, 90, 1.5}, {10.0, 60, 127, 1.5},
  {12.0, 72, 90, 1.5}, {14.0, 84, 90, 1.5}, {16.0, 36, 90, 1.5},
  {18.0, 48, 100, 2.0}, {18.0, 55, 100, 2.0}, {18.0, 60, 100, 2.0}, {18.0, 64, 100, 2.0},
};
#define kRefSeqCount (sizeof kRefSeq / sizeof kRefSeq[0])

// Short notes 5 s apart, so effect tails (delay, reverb) decay in the clear.
static const struct RefEvent kFxSeq[] = {
  {0.0, 60, 100, 0.1}, {5.0, 60, 100, 0.1},
  {10.0, 48, 100, 0.3}, {10.0, 52, 100, 0.3}, {10.0, 55, 100, 0.3}, {15.0, 72, 100, 0.1},
};
#define kFxSeqCount (sizeof kFxSeq / sizeof kFxSeq[0])
