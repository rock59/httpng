#!/bin/bash
openssl req -x509 -nodes -newkey rsa:2048 -subj /CN=localhost -subject -keyout $1/examples/key.pem -out $1/examples/cert.pem -days 200000
