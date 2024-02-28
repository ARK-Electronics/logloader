#!/bin/bash

# https://unix.stackexchange.com/questions/696381/upgrading-openssl-to-version-3-0-2-from-source

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

# apt-get install --reinstall ca-certificates
echo "export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt" >> /etc/environment

openssl version
exit