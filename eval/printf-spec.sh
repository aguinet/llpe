#!/bin/bash

~/integrator/scripts/integrate-prepared.sh printf-pre.bc -o printf-opt.bc --spec-env=3,../../spec_env --spec-argv=1,2,../../printf_spec_argv --spec-param=0,main --spec-param=4,0 --intheuristics-root=__uClibc_main_spec --int-optimistic-loop=_vfprintf_internal,17,23.loopexit2 --int-optimistic-loop=vasnprintf,50,627 --int-ignore-loop=_charpad,4 --int-loop-max=vasnprintf,484,0 --int-loop-max=vasnprintf,484.outer,0 --int-ignore-loop=_stdlib_strto_l_l,16.outer --int-always-inline=__error --int-assume-edge=vstrtoimax,entry,7 --int-assume-edge=vstrtoimax,entry,3 --int-assume-edge=vstrtoimax,4,5 --int-assume-edge=vstrtoimax,3,7 --int-assume-edge=vstrtoimax,3,4 --int-assume-edge=_vfprintf_internal,14,16 --int-assume-edge=verify_numeric,entry,4 --int-assume-edge=verify_numeric,4,5 --int-assume-edge=verify_numeric,5,6 --int-assume-edge=verify_numeric,5,7 --int-assume-edge=verify_numeric,entry,3 --int-assume-edge=__error,5,6 --int-assume-edge=xprintf,entry,3.i --int-assume-edge=xprintf,3.i,4.i $@