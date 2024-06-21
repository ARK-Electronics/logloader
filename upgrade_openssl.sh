#!/bin/bash

# https://unix.stackexchange.com/questions/696381/upgrading-openssl-to-version-3-0-2-from-source

# Prompt for sudo password at the start to cache it
sudo true

pushd .
cd /usr/src
sudo wget https://www.openssl.org/source/openssl-3.0.2.tar.gz
sudo tar zxvf openssl-3.0.2.tar.gz
cd openssl-3.0.2
sudo ./Configure
sudo make -j$(nproc)
sudo make -j$(nproc) install

# if you have an old version of OpenSSL installed, you may have to do:
cd /usr/lib/ssl
sudo unlink openssl.cnf
sudo ln -s /usr/local/ssl/openssl.cnf openssl.cnf
sudo ldconfig

popd

# Update CA certificates
sudo apt-get install --reinstall ca-certificates

if ! grep -q 'SSL_CERT_FILE' /etc/environment; then
    echo "export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt" | sudo tee -a /etc/environment
fi

openssl version
