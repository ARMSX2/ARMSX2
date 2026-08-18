#version 450

// This can't be an option without slowing the shader down.
// Note that only the fine mask works on SNES Classic Edition
// due to Mali 400 gpu precision.
#define FINEMASK

#include "zfast_crt_impl.inc"
