#!/bin/bash

SRC=$1
CMD="openssl req -x509 -nodes -newkey rsa:2048 -subj /CN=localhost -subject -keyout $SRC/examples/key.pem -out $SRC/examples/cert.pem -days 200000"
if [ ! -e $SRC/examples/key.pem ] || [ ! -e $SRC/examples/cert.pem ] ; then
    $CMD
else
    echo "key.pem and cert.pem already exist"
fi
