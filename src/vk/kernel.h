#pragma once
// Adapter (this project): the part of the NFL Head Coach 09 for PC runtime's kernel interface that the
// renderer and pacing code use, implemented over this project's kernel (src/vk/vk_adapter.cpp).
#include "runtime.h"

#include <cstdint>

// The calling guest thread's id (0 for a host thread).
uint64_t KernelCurrentTid();
// The kernel's interrupt time (KeQueryInterruptTime's clock) in nanoseconds.
uint64_t KernelInterruptTimeNs();
