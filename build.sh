#!/bin/sh

gcc -g -Wall -fPIC -shared -o imagefile-partition.so preload.c -ldl
