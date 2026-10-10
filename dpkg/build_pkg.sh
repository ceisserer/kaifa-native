#!/bin/sh

# build binary
make -C ../.. kaifareader || { echo "Error building kaifareader"; exit 1; }

cd content

# copy files to debian package structure
mkdir -p usr/bin/
mkdir -p etc/kaifareader/
mkdir -p lib/systemd/system/
cp -v ../../kaifareader                  usr/bin/
cp -v ../../meter_template.json          etc/kaifareader/
cp -v ../../systemd/kaifareader.service  lib/systemd/system/

dpkg-buildpackage -uc -us

if [ $? -ne 0 ]; then
  echo "Error creating debian package"
  exit 1
fi

cd ..
