#!/bin/bash
# Writes the mkisofs -sort file for a staged disc to stdout: the files a level
# load reads together lie together on the disc (fewer GD-ROM/GDEMU seeks), in
# the order they are read: a level load reads its level file, then the
# injections, the sound bank and the textures, so the head only sweeps forward.
# Higher weights come first. Patterns are disjoint.
#   tools/dreamcast/disc_sort.sh <stage-dir>
S="$1"
cat << EOF
$S/1ST_READ.BIN 1000
$S/*/cfg/* 900
$S/cfg/* 900
$S/*/modules/* 900
$S/modules/* 900
$S/*/scripts/* 900
$S/scripts/* 900
$S/TRXLUA.BIN 900
$S/*/levels/* 800
$S/*injections/* 790
$S/*main.sfx* 780
$S/*textures.vq 770
$S/*textures.bump 760
$S/*/pix/* 300
$S/*/images/* 300
$S/*music/* 100
$S/*fmv/* 50
EOF
