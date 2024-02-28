#!/bin/bash

sudo su
cd /usr/src
wget https://www.openssl.org/source/openssl-3.0.2.tar.gz
tar zxvf openssl-3.0.2.tar.gz
cd openssl-3.0.2
./Configure
make -j$(nproc)
make -j$(nproc) install

# if you have an old version of OpenSSL installed, you may have to do:
cd /usr/lib/ssl
unlink openssl.cnf
ln -s /usr/local/ssl/openssl.cnf openssl.cnf
ldconfig
openssl version
exit