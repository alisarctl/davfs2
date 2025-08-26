#!/bin/sh

set -ex

PACKAGES="
  libneon27-dev
  meson
  ninja-build
  po4a
"

sudo -E apt-get -y install $PACKAGES
