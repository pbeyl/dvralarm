#!/bin/bash

if [[ $# -eq 0 ]] ; then
    echo 'Script takes exactly one argument which is the version number'
    exit 0
fi

echo Packaging dvralarm_$1beta.tar.gz
tar czvf dvralarm_$1beta.tar.gz Makefile README zmodopipe.c dvralarm_pi.py dvralarm.sh Dev_Testing_Sketch_Pull-up_Resister.png
