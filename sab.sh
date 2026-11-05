#!/bin/bash
# sab.sh NAME [env...] : deterministic run of the recorded evening raw frame through the full graph
N=$1; shift; cd ~/Projects/ipu7-re && env SYNTH_RAW=dump-frame151/FULL_t0301_n0_term5.bin NFRAMES=2 NODE0_ONLY=1 "$@" ./synth g6 0 0 0 1.006 3.641 synthdump/S_$N >/dev/null 2>&1
