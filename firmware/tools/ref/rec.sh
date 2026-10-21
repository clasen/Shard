#!/bin/zsh
# rec.sh <out.raw> [seq|fxseq]: silences NS2, then records the Loopback device "Master" (48 kHz mono float32)
# while midid plays a sequence of seq.h. Start "<out dir>/midid <out dir>/midi.fifo" first.
D=${0:a:h}
echo "cc 123 0" > $D/midi.fifo
sleep 1.5
$D/arec Master 22.5 "$1" &
sleep 0.5
echo ${2:-seq} > $D/midi.fifo
wait
echo "cc 123 0" > $D/midi.fifo
