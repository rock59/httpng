#!/bin/sh

SRC=$1
echo "SRC=$SRC"
CMD="openssl req -x509 -nodes -newkey rsa:2048 -subj /CN=localhost -subject -keyout $SRC/examples/key.pem -out $SRC/examples/cert.pem -days 200000"
if [ ! -e $SRC/examples/key.pem ] || [ ! -e $SRC/examples/cert.pem ] ; then
    echo "Generating self-signed key/certificate..."
    $CMD
else
    echo "key.pem and cert.pem already exist"
fi
