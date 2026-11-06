#!/bin/sh
# Доля кадров, где PC процессора в момент конца кадра совпадает с Mesen.
#   tools/pc_score.sh <кадров> [переменные окружения для replay...]
N=$1; shift
env "$@" TGL_NOSYNC=1 TGL_REGS=1 TGL_REGS_TO=$N build/replay recordings/tgl.nes ${LOG:-recordings/run1.bin} $N \
 | awk '/^R/{t++; split($5,a,"="); split($9,b,"="); if (a[2]==b[2]) h++; c[$NF]++}
        END{printf "pc match %d/%d (%.1f%%)  ", h, t, 100*h/t; for (k in c) printf "%s:%d ", k, c[k]; print ""}'
